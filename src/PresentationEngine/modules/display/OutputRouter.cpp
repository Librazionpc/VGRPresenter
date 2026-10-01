#include "modules/display/OutputRouter.hpp"

#include <algorithm>
#include <cmath>

namespace bps::display {

namespace {

// Scale/crop of a frame region into a destination rectangle. Downscale
// area-averages (box filter — every source pixel contributes to every output
// pixel it touches, so a 2× reduction can no longer alias thin text strokes
// away); upscale bilinearly interpolates (no blocky 2× pixels). The old
// nearest-neighbor pick was the CPU-hotspot AND the quality complaint in one:
// at output scales ≈1:1 it sampled ONE source pixel per output pixel, so
// song lyrics on a 1600×900 screen from a 1920×1080 feed shimmered/aliased
// on every frame. Pixels are Color::Pack 0xAABBGGRR; channels are unpacked
// and averaged in wider integers (premultiplied-alpha is NOT assumed — the
// compositor hands over flattened frames).
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
    const auto sample = [&src, sw](int x, int y) -> uint32_t {
        return src.pixels[static_cast<size_t>(std::clamp(y, 0, src.height - 1)) * sw
                          + std::clamp(x, 0, sw - 1)];
    };
    const auto unpack = [](uint32_t p, int& r, int& g, int& b, int& a) {
        r = static_cast<int>(p & 0xFFu);
        g = static_cast<int>((p >> 8) & 0xFFu);
        b = static_cast<int>((p >> 16) & 0xFFu);
        a = static_cast<int>((p >> 24) & 0xFFu);
    };

    for (int dy = 0; dy < out.height; ++dy) {
        // Source footprint of this output row/column (in source pixels).
        const double fy0 = static_cast<double>(srcH) * dy / out.height;
        const double fy1 = static_cast<double>(srcH) * (dy + 1) / out.height;
        for (int dx = 0; dx < out.width; ++dx) {
            const double fx0 = static_cast<double>(srcW) * dx / out.width;
            const double fx1 = static_cast<double>(srcW) * (dx + 1) / out.width;
            uint32_t px;
            if (fx1 - fx0 >= 1.0 || fy1 - fy0 >= 1.0) {
                // DOWNSCALE (footprint ≥ 1 source px): box-average the
                // covered area. Integer core + edge fractions keeps it fast;
                // accumulate in 64-bit so a 100×100 footprint cannot roll
                // a 8-bit channel over.
                const int ix0 = sx0 + static_cast<int>(fx0);
                const int iy0 = sy0 + static_cast<int>(fy0);
                const int ix1 = sx0 + static_cast<int>(std::ceil(fx1));
                const int iy1 = sy0 + static_cast<int>(std::ceil(fy1));
                uint64_t rSum = 0, gSum = 0, bSum = 0, aSum = 0, wSum = 0;
                for (int y = iy0; y < iy1 && y < sy1; ++y) {
                    const double wy = std::min(fy1, static_cast<double>(y + 1))
                                      - std::max(fy0, static_cast<double>(y));
                    if (wy <= 0) continue;
                    for (int x = ix0; x < ix1 && x < sx1; ++x) {
                        const double wx = std::min(fx1, static_cast<double>(x + 1))
                                          - std::max(fx0, static_cast<double>(x));
                        if (wx <= 0) continue;
                        const double w = wx * wy;
                        int r, g, b, a;
                        unpack(sample(x, y), r, g, b, a);
                        rSum += static_cast<uint64_t>(r * w);
                        gSum += static_cast<uint64_t>(g * w);
                        bSum += static_cast<uint64_t>(b * w);
                        aSum += static_cast<uint64_t>(a * w);
                        wSum += static_cast<uint64_t>(w * 255.0);
                    }
                }
                if (wSum == 0) {
                    px = sample(sx0 + static_cast<int>(fx0), sy0 + static_cast<int>(fy0));
                } else {
                    const auto chan = [wSum](uint64_t s) -> uint32_t {
                        return static_cast<uint32_t>(std::min<uint64_t>(
                            255, (s + wSum / 2) / wSum));
                    };
                    px = chan(aSum) << 24 | chan(bSum) << 16 | chan(gSum) << 8 | chan(rSum);
                }
            } else {
                // UPSCALE / ~1:1 (footprint < 1 px): bilinear at the center.
                const double fx = fx0 + (fx1 - fx0) * 0.5;
                const double fy = fy0 + (fy1 - fy0) * 0.5;
                const double gx = sx0 + std::clamp(fx, 0.0, static_cast<double>(srcW - 1));
                const double gy = sy0 + std::clamp(fy, 0.0, static_cast<double>(srcH - 1));
                const int x0 = static_cast<int>(gx);
                const int y0 = static_cast<int>(gy);
                const int x1 = std::min(x0 + 1, sx1 - 1);
                const int y1 = std::min(y0 + 1, sy1 - 1);
                const double tx = gx - x0;
                const double ty = gy - y0;
                int r00, g00, b00, a00, r01, g01, b01, a01;
                int r10, g10, b10, a10, r11, g11, b11, a11;
                unpack(sample(x0, y0), r00, g00, b00, a00);   // top-left
                unpack(sample(x1, y0), r01, g01, b01, a01);   // top-right
                unpack(sample(x0, y1), r10, g10, b10, a10);   // bottom-left
                unpack(sample(x1, y1), r11, g11, b11, a11);   // bottom-right
                const auto lerp2 = [tx, ty](int c00, int c01, int c10, int c11) {
                    const double top = c00 + (c01 - c00) * tx;
                    const double bot = c10 + (c11 - c10) * tx;
                    return static_cast<int>(top + (bot - top) * ty + 0.5);
                };
                int r = std::clamp(lerp2(r00, r01, r10, r11), 0, 255);
                int g = std::clamp(lerp2(g00, g01, g10, g11), 0, 255);
                int b = std::clamp(lerp2(b00, b01, b10, b11), 0, 255);
                int a = std::clamp(lerp2(a00, a01, a10, a11), 0, 255);
                px = static_cast<uint32_t>(a) << 24 | static_cast<uint32_t>(b) << 16
                     | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(r);
            }
            out.pixels[static_cast<size_t>(dy) * out.width + dx] = px;
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
