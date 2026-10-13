#pragma once

// ONE home for "draw these source pixels at that size, and make it look like a
// picture" — the filter behind every scaled draw in the engine.
//
// The software backend is the engine's only real rasterizer (GraphicsBackends
// .cpp), and a media frame, a camera tap, a style's background image and a
// template's own picture all reach it at a size that is NOT their pixel size:
// a 720p sermon playing into a 1080p output, a 4K photo cover-fit onto the
// stage, a taken screen smaller than the frame it is composited into. The
// rasterizer used to answer every one of those with the NEAREST source pixel
// (`sx = sx0 + dx * sw / w`), which is why those pictures read blocky when the
// source is smaller than the draw and aliased/moiré ("meshed") when it is
// larger — the reported "the media frame looks pixel like, not crispy clear".
//
// The filter is chosen per axis from the ratio alone, so there is nothing for a
// caller to decide:
//   * magnifying (the source footprint of a destination pixel is <= 1 pixel):
//     BILINEAR — the two neighbouring source pixels, weighted by sub-pixel
//     position. A 1:1 draw lands exactly on a pixel centre and returns that
//     pixel untouched, so an unscaled draw stays bit-exact.
//   * minifying (footprint > 1 pixel): a BOX average over the footprint, so
//     every source pixel the destination pixel covers contributes. Point
//     sampling here is what made a downscaled picture shimmer and moiré; a box
//     average is the cheap filter that removes it.
//
// Averaging runs in PREMULTIPLIED space: the engine's pixels are straight-alpha
// 0xAABBGGRR, and averaging those directly drags the colour of transparent
// (a=0) pixels into the result — a halo around every anti-aliased edge. Weights
// are normalised, so a transparent footprint stays transparent.
//
// Callers keep their own loops (Blit writes into the live framebuffer with tint
// and blend; a resize returns a new image) but build their taps THROUGH here, so
// the two can never answer the picture differently.

#include "modules/rendering/IGraphicsBackend.hpp"

namespace bps::rendering {

// Taps of one axis of a resize: which source indices a destination pixel takes,
// and with what weight (weights sum to 1).
struct ScaleTaps {
    int count = 0;
    int index[4] = {0, 0, 0, 0};
    double weight[4] = {1.0, 0.0, 0.0, 0.0};
};

// Build the taps for ONE axis. `srcStart`/`srcSpan` are the source interval this
// destination pixel covers (in source pixels), `srcExtent` the source size along
// that axis (indices are clamped into it — an edge extends its last pixel rather
// than leaving a hole). Returns the tap count (1..4).
int BuildScaleTaps(double srcStart, double srcSpan, int srcExtent, ScaleTaps& out);

// The filtered colour for one destination pixel: the product of the two axes'
// taps, averaged in premultiplied space (see the header comment).
uint32_t CombineTaps(const RgbaImage& src, const ScaleTaps& xs, const ScaleTaps& ys);

} // namespace bps::rendering
