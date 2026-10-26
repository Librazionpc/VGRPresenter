#include "modules/rendering/Effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace bps::rendering {

namespace {

// The engine's packed pixel is 0xAABBGGRR (Color::Pack — see RenderOutputs);
// every channel helper below goes through these so the filter pass can never
// disagree with the rest of the render path about which byte is which.
inline uint32_t Ch(uint32_t px, int shift) { return (px >> shift) & 0xFFu; }
inline uint32_t Clamp8(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint32_t>(v + 0.5);
}
inline uint32_t Pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// One 3x3 colour matrix over the region, in 0..255 space. The CSS filter
// functions are all expressed as one of these (hue-rotate, grayscale and
// saturate are literal matrices; brightness/contrast/invert reduce to a
// diagonal matrix plus an offset), which keeps the arithmetic identical to what
// the browser-based original produces.
struct ColorMatrix {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double offset[3] = {0, 0, 0};
};

void ApplyMatrix(RgbaImage& img, const ColorMatrix& cm) {
    const size_t count = static_cast<size_t>(img.width) * img.height;
    for (size_t i = 0; i < count; ++i) {
        uint32_t& px = img.pixels[i];
        const double r = static_cast<double>(Ch(px, 0));
        const double g = static_cast<double>(Ch(px, 8));
        const double b = static_cast<double>(Ch(px, 16));
        const double a = static_cast<double>(Ch(px, 24));
        px = Pack(Clamp8(cm.m[0][0] * r + cm.m[0][1] * g + cm.m[0][2] * b + cm.offset[0]),
                  Clamp8(cm.m[1][0] * r + cm.m[1][1] * g + cm.m[1][2] * b + cm.offset[1]),
                  Clamp8(cm.m[2][0] * r + cm.m[2][1] * g + cm.m[2][2] * b + cm.offset[2]),
                  static_cast<uint32_t>(a));
    }
}

// Box blur over the whole image, radius in pixels. Two passes (horizontal then
// vertical) rather than an N×N neighbourhood: same average, a fraction of the
// work, which matters because this runs on the live frame path.
void BoxBlur(RgbaImage& img, int radius) {
    if (radius < 1 || img.empty()) return;
    const int w = img.width;
    const int h = img.height;
    std::vector<uint32_t> tmp(img.pixels.size());
    const auto window = [radius](int centre, int extent) {
        return std::pair<int, int>{std::max(0, centre - radius), std::min(extent - 1, centre + radius)};
    };

    // Horizontal.
    for (int y = 0; y < h; ++y) {
        const size_t row = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const auto [x0, x1] = window(x, w);
            double r = 0, g = 0, b = 0, a = 0;
            for (int sx = x0; sx <= x1; ++sx) {
                const uint32_t sp = img.pixels[row + static_cast<size_t>(sx)];
                r += Ch(sp, 0); g += Ch(sp, 8); b += Ch(sp, 16); a += Ch(sp, 24);
            }
            const double n = static_cast<double>(x1 - x0 + 1);
            tmp[row + static_cast<size_t>(x)] =
                Pack(Clamp8(r / n), Clamp8(g / n), Clamp8(b / n), Clamp8(a / n));
        }
    }
    // Vertical.
    for (int y = 0; y < h; ++y) {
        const auto [y0, y1] = window(y, h);
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = y0; sy <= y1; ++sy) {
                const uint32_t sp = tmp[static_cast<size_t>(sy) * w + static_cast<size_t>(x)];
                r += Ch(sp, 0); g += Ch(sp, 8); b += Ch(sp, 16); a += Ch(sp, 24);
            }
            const double n = static_cast<double>(y1 - y0 + 1);
            img.pixels[static_cast<size_t>(y) * w + static_cast<size_t>(x)] =
                Pack(Clamp8(r / n), Clamp8(g / n), Clamp8(b / n), Clamp8(a / n));
        }
    }
}

// CSS `hue-rotate(θ)` — the standard SVG feColorMatrix recipe, degrees.
ColorMatrix HueRotateMatrix(double degrees) {
    const double rad = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    ColorMatrix cm;
    cm.m[0][0] = 0.213 + c * 0.787 - s * 0.213;
    cm.m[0][1] = 0.715 - c * 0.715 - s * 0.715;
    cm.m[0][2] = 0.072 - c * 0.072 + s * 0.928;
    cm.m[1][0] = 0.213 - c * 0.213 + s * 0.143;
    cm.m[1][1] = 0.715 + c * 0.285 + s * 0.140;
    cm.m[1][2] = 0.072 - c * 0.072 - s * 0.283;
    cm.m[2][0] = 0.213 - c * 0.213 - s * 0.787;
    cm.m[2][1] = 0.715 - c * 0.715 + s * 0.715;
    cm.m[2][2] = 0.072 + c * 0.928 + s * 0.072;
    return cm;
}

// CSS `grayscale(p)`: p of the way to luma at every channel.
ColorMatrix GrayscaleMatrix(double p) {
    const double k = std::clamp(p, 0.0, 1.0);
    const double lr = 0.2126, lg = 0.7152, lb = 0.0722;
    ColorMatrix cm;
    cm.m[0][0] = lr + (1 - lr) * (1 - k);
    cm.m[0][1] = lg - lg * (1 - k);
    cm.m[0][2] = lb - lb * (1 - k);
    cm.m[1][0] = lr - lr * (1 - k);
    cm.m[1][1] = lg + (1 - lg) * (1 - k);
    cm.m[1][2] = lb - lb * (1 - k);
    cm.m[2][0] = lr - lr * (1 - k);
    cm.m[2][1] = lg - lg * (1 - k);
    cm.m[2][2] = lb + (1 - lb) * (1 - k);
    return cm;
}

// CSS `saturate(s)`: s = 1 identity, 0 fully grey.
ColorMatrix SaturateMatrix(double s) {
    const double k = std::max(0.0, s);
    const double lr = 0.2126, lg = 0.7152, lb = 0.0722;
    ColorMatrix cm;
    cm.m[0][0] = lr + (1 - lr) * k;
    cm.m[0][1] = lg - lg * k;
    cm.m[0][2] = lb - lb * k;
    cm.m[1][0] = lr - lr * k;
    cm.m[1][1] = lg + (1 - lg) * k;
    cm.m[1][2] = lb - lb * k;
    cm.m[2][0] = lr - lr * k;
    cm.m[2][1] = lg - lg * k;
    cm.m[2][2] = lb + (1 - lb) * k;
    return cm;
}

// CSS `invert(p)`: p of the way to the negative.
ColorMatrix InvertMatrix(double p) {
    const double k = std::clamp(p, 0.0, 1.0);
    ColorMatrix cm;
    for (int i = 0; i < 3; ++i)
        cm.m[i][i] = 1.0 - 2.0 * k;
    for (int i = 0; i < 3; ++i)
        cm.offset[i] = 255.0 * k;
    return cm;
}

// CSS `brightness(b)`: a plain multiplier.
ColorMatrix BrightnessMatrix(double b) {
    ColorMatrix cm;
    const double k = std::max(0.0, b);
    for (int i = 0; i < 3; ++i)
        cm.m[i][i] = k;
    return cm;
}

// CSS `contrast(c)`: scale about mid-grey.
ColorMatrix ContrastMatrix(double c) {
    ColorMatrix cm;
    const double k = std::max(0.0, c);
    for (int i = 0; i < 3; ++i) {
        cm.m[i][i] = k;
        cm.offset[i] = 127.5 * (1.0 - k);
    }
    return cm;
}

} // namespace

void ApplyFilters(RgbaImage& img, const SlideFilterValues& f) {
    if (img.empty() || f.IsIdentity()) return;
    // CSS order: hue-rotate, invert, blur, grayscale, brightness, contrast,
    // saturate — the same order the rows are listed in, so what the operator
    // reads top-to-bottom is what the pixels get.
    if (f.hueRotateDeg != 0.0) ApplyMatrix(img, HueRotateMatrix(f.hueRotateDeg));
    if (f.invert > 0.0) ApplyMatrix(img, InvertMatrix(f.invert));
    if (f.blurPx > 0.0) BoxBlur(img, std::max(1, static_cast<int>(f.blurPx + 0.5)));
    if (f.grayscale > 0.0) ApplyMatrix(img, GrayscaleMatrix(f.grayscale));
    if (f.brightness != 1.0) ApplyMatrix(img, BrightnessMatrix(f.brightness));
    if (f.contrast != 1.0) ApplyMatrix(img, ContrastMatrix(f.contrast));
    if (f.saturate != 1.0) ApplyMatrix(img, SaturateMatrix(f.saturate));
}

void EffectStack::SetEnabled(size_t index, bool enabled) {
    if (index < effects_.size()) effects_[index].enabled = enabled;
}

bool EffectStack::Has(EffectType t) const {
    for (const auto& e : effects_)
        if (e.type == t && e.enabled) return true;
    return false;
}

} // namespace bps::rendering
