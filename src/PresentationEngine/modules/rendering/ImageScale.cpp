#include "modules/rendering/ImageScale.hpp"

#include <algorithm>
#include <cmath>

namespace bps::rendering {

namespace {

// A footprint longer than this is sampled on a stride rather than every pixel:
// the tap count (and so the per-pixel cost) stays bounded however far a picture
// is shrunk, and four evenly spaced samples inside the footprint already remove
// the aliasing four would — a stride only thins a box that covers many pixels.
constexpr int kMaxTapsPerAxis = 4;

inline int ClampIndex(int i, int extent) {
    if (i < 0) return 0;
    if (i >= extent) return extent - 1;
    return i;
}

} // namespace

int BuildScaleTaps(double srcStart, double srcSpan, int srcExtent, ScaleTaps& out) {
    out.count = 0;
    if (srcExtent <= 0) return 0;
    // A span of zero (or a degenerate rect) is a point sample: the pixel under
    // the start. Never divide by it.
    if (!(srcSpan > 0.0)) {
        out.count = 1;
        out.index[0] = ClampIndex(static_cast<int>(std::floor(srcStart)), srcExtent);
        out.weight[0] = 1.0;
        return 1;
    }

    if (srcSpan <= 1.0) {
        // MAGNIFYING — bilinear around the destination pixel's centre. The
        // `- 0.5` is what makes a 1:1 draw land on a pixel centre (frac 0) and
        // return exactly that pixel; without it every picture would be a half
        // pixel soft, including the ones drawn at their own size.
        const double center = srcStart + srcSpan * 0.5 - 0.5;
        const double base = std::floor(center);
        const double frac = center - base;
        const int i0 = ClampIndex(static_cast<int>(base), srcExtent);
        const int i1 = ClampIndex(static_cast<int>(base) + 1, srcExtent);
        out.count = 2;
        out.index[0] = i0;
        out.index[1] = i1;
        out.weight[0] = 1.0 - frac;
        out.weight[1] = frac;
        return 2;
    }

    // MINIFYING — a box average over the footprint. `begin`/`end` are the source
    // pixels the destination pixel actually covers; with more than
    // kMaxTapsPerAxis of them, sample evenly across the box instead.
    const double begin = srcStart;
    const double end = srcStart + srcSpan;
    const int first = static_cast<int>(std::floor(begin));
    const int last = static_cast<int>(std::ceil(end));
    const int covered = std::max(1, last - first);
    if (covered <= kMaxTapsPerAxis) {
        int n = 0;
        for (int i = first; i < last && n < kMaxTapsPerAxis; ++i, ++n) {
            out.index[n] = ClampIndex(i, srcExtent);
            out.weight[n] = 1.0;
        }
        out.count = n;
    } else {
        const double step = srcSpan / kMaxTapsPerAxis;
        for (int n = 0; n < kMaxTapsPerAxis; ++n) {
            const double center = begin + step * (n + 0.5);
            out.index[n] = ClampIndex(static_cast<int>(std::floor(center)), srcExtent);
            out.weight[n] = 1.0;
        }
        out.count = kMaxTapsPerAxis;
    }
    // Normalise: a clamped tap list can repeat an index (a footprint running off
    // the edge), and equal weights are what keep the average honest.
    double sum = 0.0;
    for (int n = 0; n < out.count; ++n)
        sum += out.weight[n];
    if (sum <= 0.0) {
        out.count = 1;
        out.index[0] = ClampIndex(static_cast<int>(std::floor(srcStart)), srcExtent);
        out.weight[0] = 1.0;
        return 1;
    }
    for (int n = 0; n < out.count; ++n)
        out.weight[n] /= sum;
    return out.count;
}

uint32_t CombineTaps(const RgbaImage& src, const ScaleTaps& xs, const ScaleTaps& ys) {
    if (src.empty() || xs.count <= 0 || ys.count <= 0) return 0;
    double sumA = 0.0, sumR = 0.0, sumG = 0.0, sumB = 0.0, sumW = 0.0;
    for (int ty = 0; ty < ys.count; ++ty) {
        const int sy = std::clamp(ys.index[ty], 0, src.height - 1);
        const size_t row = static_cast<size_t>(sy) * static_cast<size_t>(src.width);
        for (int tx = 0; tx < xs.count; ++tx) {
            const double w = xs.weight[tx] * ys.weight[ty];
            if (w == 0.0) continue;
            const int sx = std::clamp(xs.index[tx], 0, src.width - 1);
            const uint32_t px = src.pixels[row + static_cast<size_t>(sx)];
            // Straight-alpha 0xAABBGGRR → premultiplied, weight, accumulate.
            const double a = static_cast<double>((px >> 24) & 0xFFu) / 255.0;
            sumA += a * w;
            sumR += (static_cast<double>((px >> 0) & 0xFFu) / 255.0) * a * w;
            sumG += (static_cast<double>((px >> 8) & 0xFFu) / 255.0) * a * w;
            sumB += (static_cast<double>((px >> 16) & 0xFFu) / 255.0) * a * w;
            sumW += w;
        }
    }
    // Round back to 8 bits per channel. A destination pixel whose whole
    // footprint was transparent stays transparent (sumA == 0) — a black pixel
    // with alpha 0 would be a wrong colour for anything that later reads the
    // frame as straight alpha.
    if (sumA <= 0.0) return 0u;
    const auto toByte = [](double v) {
        const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return static_cast<uint32_t>(c * 255.0 + 0.5) & 0xFFu;
    };
    const uint32_t aa = toByte(sumA / (sumW > 0.0 ? sumW : 1.0));
    const uint32_t rr = toByte(sumR / sumA);
    const uint32_t gg = toByte(sumG / sumA);
    const uint32_t bb = toByte(sumB / sumA);
    return (aa << 24) | (bb << 16) | (gg << 8) | rr;
}

} // namespace bps::rendering
