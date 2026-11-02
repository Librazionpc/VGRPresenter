#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {
struct ListStyle {
    std::string key;       // stored in a block's meta ("disc", "decimal", ...); "none" = no list
    std::string label;     // what a picker calls it ("Bullet", "Numbers")
    std::string sample;    // its first marker ("•", "1."), for a picker's chip
};

// Every list style, "none" first, in the order a picker shows them (FreeShow's list styles plus a dash).
const std::vector<ListStyle>& ListStyles();

// The marker of the `n`th item (1-based) in `style` ("" for "none" or an unknown style): "•", "1.", "01.", "b.", "iv."...
std::string ListMarker(std::string_view style, size_t n);

// `text` with every non-blank line prefixed by its marker and a space. A "none", empty or unknown style returns `text` as is.
std::string ApplyList(std::string_view text, std::string_view style);
} // namespace bps::presentation