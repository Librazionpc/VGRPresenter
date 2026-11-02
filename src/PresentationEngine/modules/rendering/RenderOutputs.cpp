#include "modules/rendering/RenderOutputs.hpp"

#include "core/logging/ChangeGate.hpp"
#include "core/logging/Logger.hpp"
#include "modules/rendering/ImageScale.hpp"
#include "core/services/ServiceManager.hpp"
#include "modules/settings/Telemetry.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>

namespace bps::rendering {
namespace settings = bps::settings;

namespace {
RgbaImage ScaleImage(const RgbaImage& src, int dstW, int dstH) {
    RgbaImage out;
    if (src.empty() || dstW <= 0 || dstH <= 0) return out;
    out.width = dstW;
    out.height = dstH;
    out.pixels.resize(static_cast<size_t>(dstW) * dstH);
    const double stepX = static_cast<double>(src.width) / dstW;
    const double stepY = static_cast<double>(src.height) / dstH;
    std::vector<ScaleTaps> xTaps(static_cast<size_t>(dstW));
    std::vector<ScaleTaps> yTaps(static_cast<size_t>(dstH));
    for (int x = 0; x < dstW; ++x)
        BuildScaleTaps(x * stepX, stepX, src.width, xTaps[static_cast<size_t>(x)]);
    for (int y = 0; y < dstH; ++y)
        BuildScaleTaps(y * stepY, stepY, src.height, yTaps[static_cast<size_t>(y)]);
    for (int y = 0; y < dstH; ++y)
        for (int x = 0; x < dstW; ++x)
            out.pixels[static_cast<size_t>(y) * dstW + x] =
                CombineTaps(src, xTaps[static_cast<size_t>(x)], yTaps[static_cast<size_t>(y)]);
    return out;
}
} // namespace

namespace {
logging::ChangeGate g_presentSanity;

// Frame::pixels is PACKED 0xAABBGGRR (Color::Pack — red in the LOW byte), the
// engine's own convention throughout the render path.
void ReportPresentSanity(const std::string& name, const std::string& sceneId, int w, int h,
                         const std::vector<uint32_t>& pixels) {
    std::string state;
    // What counts as a change is the frame's SHAPE — size, black or not, the scene on air.
    // The mean luma of a static picture still moves by a rounding step between frames, and
    // gating on it made this line fire once per frame (the bulk of a session's engine.log).
    std::string signature;
    if (w <= 0 || h <= 0 || pixels.empty()) {
        state = "no pixels";
        signature = "no pixels";
    } else {
        long long sum = 0;
        int count = 0;
        int maxLuma = 0;
        int maxAlpha = 0;
        const int stepY = std::max(1, h / 16);
        const int stepX = std::max(1, w / 16);
        for (int y = 0; y < h; y += stepY) {
            for (int x = 0; x < w; x += stepX) {
                const uint32_t px = pixels[static_cast<size_t>(y) * w + x];
                const int r = static_cast<int>(px & 0xFFu);
                const int g = static_cast<int>((px >> 8) & 0xFFu);
                const int b = static_cast<int>((px >> 16) & 0xFFu);
                const int a = static_cast<int>((px >> 24) & 0xFFu);
                const int luma = (r * 299 + g * 587 + b * 114) / 1000;
                sum += luma;
                ++count;
                if (luma > maxLuma) maxLuma = luma;
                if (a > maxAlpha) maxAlpha = a;
            }
        }
        const double mean = count > 0 ? static_cast<double>(sum) / count : 0.0;
        const char* black = maxLuma == 0 ? "yes" : "no";
        state = std::format("{}x{} meanLuma={:.1f} maxLuma={} maxAlpha={} allBlack={}",
                            w, h, mean, maxLuma, maxAlpha, black);
        signature = std::format("{}x{} allBlack={}", w, h, black);
    }
    state += std::format(" scene='{}'", sceneId);
    signature += std::format(" scene='{}'", sceneId);
    if (g_presentSanity.Changed(name, signature))
        Logger::Instance().Info(std::format("output frame sanity: '{}' {}", name, state),
                                "RenderOutputs");
}
} // namespace

Result<void> FrameBufferOutput::Present(const Frame& frame) {
    if (!enabled_.load()) return Ok();
    lastPresentMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
    auto next = std::make_shared<Frame>();
    next->sceneId = frame.sceneId;
    next->frame = frame.frame;
    next->timestampMs = frame.timestampMs;
    if (frame.width == static_cast<int>(target_.width) &&
        frame.height == static_cast<int>(target_.height)) {
        next->width = frame.width;
        next->height = frame.height;
        next->pixels = frame.pixels;
    } else {
        RgbaImage img;
        img.width = frame.width;
        img.height = frame.height;
        img.pixels = frame.pixels;
        RgbaImage scaled = ScaleImage(img, static_cast<int>(target_.width),
                                      static_cast<int>(target_.height));
        next->width = scaled.width;
        next->height = scaled.height;
        next->pixels = std::move(scaled.pixels);
    }
    ReportPresentSanity(name_, next->sceneId, next->width, next->height, next->pixels);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFrame_ = std::move(next);
    }
    received_.fetch_add(1);
    return Ok();
}

Frame FrameBufferOutput::LastFrame() const {
    const auto snapshot = LastFrameSnapshot();
    return snapshot ? *snapshot : Frame{};
}

std::shared_ptr<const Frame> FrameBufferOutput::LastFrameSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrame_;
}

Result<void> ScreenshotOutput::SavePpm(std::string_view path) const {
    Frame f = LastFrame();
    if (f.empty()) return Error::Make(Err::InvalidState, "Screenshot", "no frame captured");
    std::string ppm = std::format("P6\n{} {}\n255\n", f.width, f.height);
    ppm.reserve(ppm.size() + static_cast<size_t>(f.width) * f.height * 3);
    for (const uint32_t px : f.pixels)
        ppm.push_back(static_cast<char>((px >> 0) & 0xFF));
    for (const uint32_t px : f.pixels)
        ppm.push_back(static_cast<char>((px >> 8) & 0xFF));
    for (const uint32_t px : f.pixels)
        ppm.push_back(static_cast<char>((px >> 16) & 0xFF));
    std::vector<uint8_t> bytes(ppm.begin(), ppm.end());
    return platform::PlatformAccessor::Get().Filesystem().WriteBinary(path, bytes);
}

// --- OutputManager ----------------------------------------------------------

Result<void> OutputManager::Add(std::shared_ptr<IRenderOutput> output) {
    if (!output) return Error::Make(Err::InvalidArgument, "Outputs", "null output");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& o : outputs_)
        if (o->Name() == output->Name())
            return Error::Make(Err::AlreadyExists, "Outputs",
                               "output '" + std::string(output->Name()) + "' exists");
    outputs_.push_back(std::move(output));
    return Ok();
}

Result<std::shared_ptr<IRenderOutput>> OutputManager::Get(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& o : outputs_)
        if (o->Name() == name) return o;
    return Error::Make(Err::Render_OutputNotFound, "Outputs",
                       "output '" + std::string(name) + "' not found");
}

Result<void> OutputManager::Remove(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if ((*it)->Name() == name) {
            outputs_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Render_OutputNotFound, "Outputs",
                       "output '" + std::string(name) + "' not found");
}

std::vector<std::shared_ptr<IRenderOutput>> OutputManager::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputs_;
}

void OutputManager::Distribute(const Frame& frame) {
    std::vector<std::shared_ptr<IRenderOutput>> enabled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled.reserve(outputs_.size());
        for (const auto& o : outputs_)
            if (o->Enabled() && o->AcceptsDistributedFrames()) enabled.push_back(o);
    }
    for (const auto& o : enabled) (void)o->Present(frame);

    if (auto* t = bps::ServiceManager::Instance().Get<settings::Telemetry>(); t != nullptr)
        t->OnOutputPresented(static_cast<int>(enabled.size()));
}

void OutputManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_.clear();
}
} // namespace bps::rendering