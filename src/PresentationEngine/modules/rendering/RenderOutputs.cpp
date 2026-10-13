#include "modules/rendering/RenderOutputs.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/ImageScale.hpp"
#include "core/services/ServiceManager.hpp"
#include "modules/settings/Telemetry.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>

namespace bps::rendering {

namespace settings = bps::settings;

namespace {

// Resample a frame to the OUTPUT's size. This runs only when the scene was
// built at another size than the output pushes (a mid-session resolution
// change — see LiveOutputController's SetTargetSize), and the pixels are a
// whole composed picture: a media frame or a background image inside it must
// not lose its crispness on the way. The FILTER is the shared one
// (ImageScale: bilinear when magnifying, box average when minifying) rather
// than the nearest grab that used to live here — the same defect as the blit
// path's, in the other place a picture gets resized.
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

// One-shot-per-output-per-STATE pixel sanity, the same idiom SceneBuilder's
// own layer diagnostics use. Present() is the LAST place inside the engine that
// sees an output's pixels, and it is what splits the two halves of "the outputs
// are black": an all-black line here means the engine itself rasterized black,
// while a real picture here with a black monitor means the frame was dropped
// after this point (the send/present path). The QML-side provider cannot
// answer this at all — it only sees buffers the UI happens to ask for, and a
// live session can easily ask for none.
std::mutex g_presentSanityMutex;
std::map<std::string, std::string> g_presentSanity;

// Frame::pixels is PACKED 0xAABBGGRR (Color::Pack — red in the LOW byte), the
// engine's own convention throughout the render path.
void ReportPresentSanity(const std::string& name, const std::string& sceneId, int w, int h,
                         const std::vector<uint32_t>& pixels) {
    std::string state;
    if (w <= 0 || h <= 0 || pixels.empty()) {
        state = "no pixels";
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
        state = std::format("{}x{} meanLuma={:.1f} maxLuma={} maxAlpha={} allBlack={}",
                            w, h, mean, maxLuma, maxAlpha,
                            maxLuma == 0 ? "yes" : "no");
    }
    // The SCENE id rides the line: a black frame is a different bug when it came
    // from the styled scene (the renderer dropped the objects) than when it came
    // from some placeholder/empty scene (the loop presented the wrong one).
    state += std::format(" scene='{}'", sceneId);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_presentSanityMutex);
        std::string& last = g_presentSanity[name];
        if (last != state) {
            last = state;
            changed = true;
        }
    }
    if (changed)
        Logger::Instance().Info(std::format("output frame sanity: '{}' {}", name, state),
                                "RenderOutputs");
}

} // namespace

Result<void> FrameBufferOutput::Present(const Frame& frame) {
    if (!enabled_.load()) return Ok();
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
    // Copy the output list under the lock, then Present outside it: Present may
    // scale a large frame (expensive) and locks each output's own mutex — never
    // hold the manager lock during that work (blocks Add/Remove/Get).
    std::vector<std::shared_ptr<IRenderOutput>> enabled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled.reserve(outputs_.size());
        for (const auto& o : outputs_)
            // AcceptsDistributedFrames(): a per-output-pass buffer is written by
            // its OWN styled pass every frame — letting the main pass write it
            // too made the NDI/HDMI feed flip between the program scene and the
            // output's scene (two writers, one buffer). Its pass owns it.
            if (o->Enabled() && o->AcceptsDistributedFrames()) enabled.push_back(o);
    }
    for (const auto& o : enabled) (void)o->Present(frame);

    // Feed the Telemetry module's output meter: how many enabled outputs just
    // received a frame. Best-effort include (never fails; a no-op before the
    // module initializes).
    if (auto* t = bps::ServiceManager::Instance().Get<settings::Telemetry>(); t != nullptr)
        t->OnOutputPresented(static_cast<int>(enabled.size()));
}

void OutputManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_.clear();
}

} // namespace bps::rendering
