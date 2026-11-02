#pragma once

#include "modules/rendering/IGraphicsBackend.hpp"

namespace bps::rendering {
// Taps of one axis of a resize: which source indices a destination pixel takes,
// and with what weight (weights sum to 1).
struct ScaleTaps {
    int count = 0;
    int index[4] = {0, 0, 0, 0};
    double weight[4] = {1.0, 0.0, 0.0, 0.0};
};

int BuildScaleTaps(double srcStart, double srcSpan, int srcExtent, ScaleTaps& out);

// The filtered colour for one destination pixel: the product of the two axes'
// taps, averaged in premultiplied space (see the header comment).
uint32_t CombineTaps(const RgbaImage& src, const ScaleTaps& xs, const ScaleTaps& ys);
} // namespace bps::rendering