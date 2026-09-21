#pragma once

// TextFormat: how the ENGINE turns a text block's typed text into what is shown, for the formatting that changes the
// characters themselves. Today that is LISTS (FreeShow's per-text-item "list" option): every line becomes a list item,
// marked with a bullet, a dash, a number, a letter ... The typed text is never changed - a block keeps what was typed and the
// list style beside it (meta "list"), and whatever draws the text (the Edit canvas, the library cards, later the output)
// asks here, so every surface shows the same markers.
//
// Numbering counts only non-blank lines and a blank line gets no marker, so paragraphs separated by a blank line still read as
// one list. The markers are plain characters, so they work with any font and wrap like text.

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
