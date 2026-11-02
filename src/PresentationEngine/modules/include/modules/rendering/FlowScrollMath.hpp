#pragma once

#include <algorithm>
#include <cmath>

// THE ONE WAY A SCROLL POSITION BECOMES A LINE (and back).
//
// The reading walk is positioned in CHARACTERS of the passage. Every surface that
// shows it (the monitor tiles, the in-app preview, the HDMI/NDI/recording renderers)
// owns its own wrap, so each has its own table of where its lines start. A surface
// turns the shared character position into its own line with LineForChar(); the
// service turns the animated reference line into that character position with
// CharForLine(). Nothing else may re-derive either, or two screens move at
// different speeds for the same words.
namespace bps::rendering::flowscroll {

// The line a character falls on: the last line whose start is <= ch.
template <class Starts>
inline int LineIndexOf(const Starts& starts, int ch) {
    if (starts.empty())
        return 0;
    const auto it = std::upper_bound(starts.begin(), starts.end(), ch);
    return it == starts.begin() ? 0 : static_cast<int>(it - starts.begin()) - 1;
}

// How many characters one line holds. The last line has no successor, so it borrows
// the one before it.
template <class Starts>
inline int LineSpan(const Starts& starts, int index) {
    const int count = static_cast<int>(starts.size());
    if (index + 1 < count)
        return std::max(1, static_cast<int>(starts[index + 1]) - static_cast<int>(starts[index]));
    if (index > 0)
        return std::max(1, static_cast<int>(starts[index]) - static_cast<int>(starts[index - 1]));
    return 1;
}

// Character position -> fractional line. Linear in characters inside a line, so a
// glide covers the same words in the same time on every surface. Never past the
// last line's start (a position beyond the passage rests on its last line).
// No table: the caller is already speaking lines.
template <class Starts>
inline double LineForChar(const Starts& starts, double charAt) {
    if (starts.empty() || charAt < 0.0)
        return charAt;
    const int index = LineIndexOf(starts, static_cast<int>(std::floor(charAt)));
    const double line = index + (charAt - static_cast<double>(starts[index])) / LineSpan(starts, index);
    return std::min(line, static_cast<double>(starts.size() - 1));
}

// Fractional line -> character position (the inverse of LineForChar).
template <class Starts>
inline double CharForLine(const Starts& starts, double lines) {
    if (starts.empty())
        return lines;
    const int count = static_cast<int>(starts.size());
    const double clamped = std::clamp(lines, 0.0, static_cast<double>(count));
    const int index = std::min(count - 1, static_cast<int>(std::floor(clamped)));
    return static_cast<double>(starts[index]) + (clamped - index) * LineSpan(starts, index);
}

} // namespace bps::rendering::flowscroll
