#include "modules/rendering/RenderOutputs.hpp"

#include "core/services/ServiceManager.hpp"
#include "modules/settings/Telemetry.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <format>

namespace bps::rendering {

namespace settings = bps::settings;

namespace {

// Bilinear-ish nearest scale of src RGBA8 to dst size.
RgbaImage ScaleImage(const RgbaImage& src, int dstW, int dstH) {
    RgbaImage out;
    if (src.empty() || dstW <= 0 || dstH <= 0) return out;
    out.width = dstW;
    out.height = dstH;
    out.pixels.resize(static_cast<size_t>(dstW) * dstH);
    for (int y = 0; y < dstH; ++y) {
        const int sy = std::min(src.height - 1, (y * src.height) / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int sx = std::min(src.width - 1, (x * src.width) / dstW);
            out.pixels[static_cast<size_t>(y) * dstW + x] =
                src.pixels[static_cast<size_t>(sy) * src.width + sx];
        }
    }
    return out;
}

} // namespace

Result<void> FrameBufferOutput::Present(const Frame& frame) {
    if (!enabled_.load()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    lastFrame_.sceneId = frame.sceneId;
    lastFrame_.frame = frame.frame;
    lastFrame_.timestampMs = frame.timestampMs;
    if (frame.width == static_cast<int>(target_.width) &&
        frame.height == static_cast<int>(target_.height)) {
        lastFrame_.width = frame.width;
        lastFrame_.height = frame.height;
        lastFrame_.pixels = frame.pixels;
    } else {
        RgbaImage img;
        img.width = frame.width;
        img.height = frame.height;
        img.pixels = frame.pixels;
        RgbaImage scaled = ScaleImage(img, static_cast<int>(target_.width),
                                      static_cast<int>(target_.height));
        lastFrame_.width = scaled.width;
        lastFrame_.height = scaled.height;
        lastFrame_.pixels = std::move(scaled.pixels);
    }
    received_.fetch_add(1);
    return Ok();
}

Frame FrameBufferOutput::LastFrame() const {
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
            if (o->Enabled()) enabled.push_back(o);
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
