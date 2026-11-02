#pragma once

#include <cmath>

namespace bps::presentation::template_text_metrics {

inline constexpr double kNaturalLineHeight = 1.0;
inline constexpr double kNaturalLineSpacing = 0.0;

inline double lineHeightMultiplier(double value) noexcept
{
    return std::isfinite(value) && value > 0.0 ? value : kNaturalLineHeight;
}

inline double lineSpacing(double value) noexcept
{
    return std::isfinite(value) ? value : kNaturalLineSpacing;
}

inline double linePitch(double naturalHeightPx, double multiplier, double gapPx = 0.0) noexcept
{
    if (!std::isfinite(naturalHeightPx) || naturalHeightPx <= 0.0)
        return 0.0;
    return naturalHeightPx * lineHeightMultiplier(multiplier) + lineSpacing(gapPx);
}

inline double qmlLineHeight(double naturalHeightPx, double multiplier, double gapPx) noexcept
{
    const double factor = lineHeightMultiplier(multiplier);
    if (!std::isfinite(naturalHeightPx) || naturalHeightPx <= 0.0)
        return factor;
    return linePitch(naturalHeightPx, factor, gapPx) / naturalHeightPx;
}

inline double engineLineSpacing(double fontSizePx, double naturalHeightPx,
                                double multiplier) noexcept
{
    const double safeFontSize = std::isfinite(fontSizePx) && fontSizePx > 0.0
        ? fontSizePx : 0.0;
    const double safeNaturalHeight = std::isfinite(naturalHeightPx) && naturalHeightPx > 0.0
        ? naturalHeightPx : safeFontSize;
    return linePitch(safeNaturalHeight, multiplier) - safeFontSize;
}

} // namespace bps::presentation::template_text_metrics
