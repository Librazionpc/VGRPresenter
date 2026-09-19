#include "modules/display/OutputRouter.hpp"

#include <algorithm>
#include <cmath>

namespace bps::display {

namespace {

// Nearest-neighbor scale/crop of a frame region into a destination rectangle.
rendering::Frame ScaleFrame(const rendering::Frame& src, const rendering::Rect& srcRect,
                            const rendering::Rect& dstRect) {
    rendering::Frame out;
    out.sceneId = src.sceneId;
    out.frame = src.frame;
    out.width = static_cast<int>(dstRect.width);
    out.height = static_cast<int>(dstRect.height);
    out.timestampMs = src.timestampMs;
    if (out.width <= 0 || out.height <= 0 || src.pixels.empty()) return out;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);

    const int sw = src.width;
    const int sh = src.height;
    // Empty source rect means the full frame.
    const rendering::Rect region = (srcRect.width > 0 && srcRect.height > 0)
                                       ? srcRect
                                       : rendering::Rect(0, 0, static_cast<float>(sw),
                                                         static_cast<float>(sh));
    const int sx0 = std::max(0, static_cast<int>(region.x));
    const int sy0 = std::max(0, static_cast<int>(region.y));
    const int sx1 = std::min(sw, static_cast<int>(region.Right()));
    const int sy1 = std::min(sh, static_cast<int>(region.Bottom()));
    if (sx1 <= sx0 || sy1 <= sy0) return out;

    const int srcW = sx1 - sx0;
    const int srcH = sy1 - sy0;
    for (int dy = 0; dy < out.height; ++dy) {
        int sy = sy0 + static_cast<int>(static_cast<double>(srcH) * dy / out.height);
        sy = std::clamp(sy, sy0, sy1 - 1);
        for (int dx = 0; dx < out.width; ++dx) {
            int sx = sx0 + static_cast<int>(static_cast<double>(srcW) * dx / out.width);
            sx = std::clamp(sx, sx0, sx1 - 1);
            out.pixels[static_cast<size_t>(dy) * out.width + dx] =
                src.pixels[static_cast<size_t>(sy) * sw + sx];
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Output registry
// ---------------------------------------------------------------------------
Result<void> OutputRouter::AddOutput(const Output& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& o : outputs_)
        if (o.id == output.id)
            return Error::Make(Err::Display_OutputExists, "OutputRouter",
                               "output '" + output.id + "' already exists");
    outputs_.push_back(output);
    return Ok();
}

Result<void> OutputRouter::RemoveOutput(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if (it->id == id) {
            outputs_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Display_OutputNotFound, "OutputRouter",
                       "output '" + std::string(id) + "' not found");
}

Result<Output*> OutputRouter::GetOutput(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& o : outputs_)
        if (o.id == id) return &o;
    return Error::Make(Err::Display_OutputNotFound, "OutputRouter",
                       "output '" + std::string(id) + "' not found");
}

std::vector<Output> OutputRouter::Outputs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputs_;
}

size_t OutputRouter::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputs_.size();
}

Result<void> OutputRouter::SetEnabled(std::string_view id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& o : outputs_)
        if (o.id == id) {
            o.enabled = enabled;
            if (!enabled) o.state = OutputState::Off;
            return Ok();
        }
    return Error::Make(Err::Display_OutputNotFound, "OutputRouter",
                       "output '" + std::string(id) + "' not found");
}

void OutputRouter::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    outputs_.clear();
}

Result<void> OutputRouter::SetTransform(std::string_view id, const OutputTransform& transform) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& o : outputs_)
        if (o.id == id) {
            o.transform = transform;
            return Ok();
        }
    return Error::Make(Err::Display_OutputNotFound, "OutputRouter",
                       "output '" + std::string(id) + "' not found");
}

// ---------------------------------------------------------------------------
// Scaling math
// ---------------------------------------------------------------------------
rendering::Rect OutputRouter::ComputeDestRect(const DisplayDevice& device,
                                              const OutputTransform& transform,
                                              const rendering::Frame& frame) {
    // Source region: transform.sourceRect or the full frame.
    rendering::Rect src;
    if (transform.sourceRect.width > 0 && transform.sourceRect.height > 0) {
        src = transform.sourceRect;
    } else {
        src = rendering::Rect(0, 0, static_cast<float>(frame.width),
                              static_cast<float>(frame.height));
    }

    // Destination region on the device.
    int dstW = transform.width > 0 ? transform.width : device.width;
    int dstH = transform.height > 0 ? transform.height : device.height;
    if (dstW <= 0) dstW = frame.width;
    if (dstH <= 0) dstH = frame.height;
    const float dstX = static_cast<float>(transform.x);
    const float dstY = static_cast<float>(transform.y);

    if (src.width <= 0 || src.height <= 0 || dstW <= 0 || dstH <= 0)
        return rendering::Rect(dstX, dstY, static_cast<float>(dstW), static_cast<float>(dstH));

    const double srcAspect = static_cast<double>(src.width) / src.height;
    const double dstAspect = static_cast<double>(dstW) / dstH;

    switch (transform.scaling) {
        case ScalingMode::Stretch:
            return rendering::Rect(dstX, dstY, static_cast<float>(dstW),
                                   static_cast<float>(dstH));
        case ScalingMode::Native: {
            // 1:1 at native size, centered.
            const float w = std::min<float>(src.width, dstW);
            const float h = std::min<float>(src.height, dstH);
            return rendering::Rect(dstX + (dstW - w) / 2.0f, dstY + (dstH - h) / 2.0f, w, h);
        }
        case ScalingMode::PixelPerfect: {
            // Integer scale factor, centered, never upscaled past the device.
            float scale = 1.0f;
            while (src.width * (scale + 1) <= dstW && src.height * (scale + 1) <= dstH)
                scale += 1.0f;
            const float w = src.width * scale;
            const float h = src.height * scale;
            return rendering::Rect(dstX + (dstW - w) / 2.0f, dstY + (dstH - h) / 2.0f, w, h);
        }
        case ScalingMode::Fit:
        case ScalingMode::Letterbox: {
            double scale;
            if (srcAspect > dstAspect) scale = static_cast<double>(dstW) / src.width;
            else scale = static_cast<double>(dstH) / src.height;
            const float w = static_cast<float>(src.width * scale);
            const float h = static_cast<float>(src.height * scale);
            return rendering::Rect(dstX + (dstW - w) / 2.0f, dstY + (dstH - h) / 2.0f, w, h);
        }
        case ScalingMode::Fill:
        case ScalingMode::Crop: {
            double scale;
            if (srcAspect > dstAspect) scale = static_cast<double>(dstH) / src.height;
            else scale = static_cast<double>(dstW) / src.width;
            const float w = static_cast<float>(src.width * scale);
            const float h = static_cast<float>(src.height * scale);
            return rendering::Rect(dstX + (dstW - w) / 2.0f, dstY + (dstH - h) / 2.0f, w, h);
        }
    }
    return rendering::Rect(dstX, dstY, static_cast<float>(dstW), static_cast<float>(dstH));
}

// ---------------------------------------------------------------------------
// Frame routing
// ---------------------------------------------------------------------------
void OutputRouter::RouteFrame(const rendering::Frame& frame, const DeviceResolver& devices,
                              const Sink& sink) {
    if (!sink) return;
    std::vector<Output> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = outputs_;
    }
    for (auto& output : snapshot) {
        if (!output.enabled || output.displayId.empty() || output.state == OutputState::Lost)
            continue;
        if (output.state == OutputState::Off || output.state == OutputState::Starting)
            output.state = OutputState::Running;

        // Resolve the bound device to know the real destination size.
        const DisplayDevice* device = devices ? devices(output.displayId) : nullptr;
        if (!device) continue;
        if (!device->connected) continue;

        // Destination rect on the device (transform target or native size).
        int dstW = output.transform.width > 0 ? output.transform.width : device->width;
        int dstH = output.transform.height > 0 ? output.transform.height : device->height;
        if (dstW <= 0) dstW = frame.width;
        if (dstH <= 0) dstH = frame.height;
        OutputTransform t = output.transform;
        t.x = output.transform.x;
        t.y = output.transform.y;
        t.width = dstW;
        t.height = dstH;

        const rendering::Rect destRect = ComputeDestRect(*device, t, frame);
        rendering::Frame scaled = ScaleFrame(frame, t.sourceRect, destRect);
        output.framesDelivered++;
        sink(output, destRect, scaled);
    }
}

} // namespace bps::display
