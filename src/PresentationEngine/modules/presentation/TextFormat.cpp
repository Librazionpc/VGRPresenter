#include "modules/presentation/TextFormat.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace bps::presentation {

namespace {

// Base-26 letters the way CSS lists them: a ... z, aa, ab ...
std::string Letters(size_t n, bool upper) {
    std::string out;
    while (n > 0) {
        --n;
        out.insert(out.begin(), static_cast<char>((upper ? 'A' : 'a') + n % 26));
        n /= 26;
    }
    return out;
}

std::string Roman(size_t n, bool upper) {
    if (n == 0 || n >= 4000) return {};
    static constexpr std::array<std::pair<size_t, const char*>, 13> kValues{{
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
        {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"} }};
    std::string out;
    for (const auto& [value, glyph] : kValues)
        while (n >= value) {
            out += glyph;
            n -= value;
        }
    if (upper) std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Greek small letters alpha .. omega (without the final sigma), as UTF-8.
const char* Greek(size_t n) {
    static constexpr std::array<const char*, 24> kLetters{
        "α", "β", "γ", "δ", "ε", "ζ", "η", "θ", "ι", "κ", "λ", "μ",
        "ν", "ξ", "ο", "π", "ρ", "σ", "τ", "υ", "φ", "χ", "ψ", "ω" };
    return n >= 1 && n <= kLetters.size() ? kLetters[n - 1] : nullptr;
}

bool Blank(std::string_view line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

} // namespace

const std::vector<ListStyle>& ListStyles() {
    // Sample = the marker of the first item, so a picker can show what the style looks like.
    static const std::vector<ListStyle> kStyles = [] {
        std::vector<ListStyle> list{
            { "none", "None", "" },
            { "disc", "Bullet", "" },
            { "circle", "Circle", "" },
            { "square", "Square", "" },
            { "dash", "Dash", "" },
            { "disclosure-closed", "Arrow", "" },
            { "decimal", "Numbers", "" },
            { "decimal-leading-zero", "Numbers 01", "" },
            { "lower-alpha", "Letters a", "" },
            { "upper-alpha", "Letters A", "" },
            { "lower-roman", "Roman i", "" },
            { "upper-roman", "Roman I", "" },
            { "lower-greek", "Greek", "" } };
        for (ListStyle& style : list) style.sample = style.key == "none" ? "" : ListMarker(style.key, 1);
        return list;
    }();
    return kStyles;
}

std::string ListMarker(std::string_view style, size_t n) {
    if (style == "disc") return "•";
    if (style == "circle") return "◦";
    if (style == "square") return "▪";
    if (style == "dash") return "–";
    if (style == "disclosure-closed") return "▸";
    if (style == "disclosure-open") return "▾";
    if (style == "decimal") return std::to_string(n) + ".";
    if (style == "decimal-leading-zero") return (n < 10 ? "0" : "") + std::to_string(n) + ".";
    if (style == "lower-alpha" || style == "upper-alpha") return Letters(n, style == "upper-alpha") + ".";
    if (style == "lower-roman" || style == "upper-roman") {
        const std::string roman = Roman(n, style == "upper-roman");
        return (roman.empty() ? std::to_string(n) : roman) + ".";
    }
    if (style == "lower-greek") {
        const char* glyph = Greek(n);
        return (glyph ? std::string(glyph) : std::to_string(n)) + ".";
    }
    return {};
}

std::string ApplyList(std::string_view text, std::string_view style) {
    if (style.empty() || style == "none" || ListMarker(style, 1).empty()) return std::string(text);
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    size_t item = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        if (!Blank(line)) {
            out += ListMarker(style, ++item);
            out += ' ';
        }
        out += line;
        if (last) break;
        out += '\n';
        start = end + 1;
    }
    return out;
}

} // namespace bps::presentation
