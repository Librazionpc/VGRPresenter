#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <cstring>

namespace bps::rendering {

// ---------------------------------------------------------------------------
// Built-in 5×7 bitmap font (ASCII 32..126). Each glyph is 5 bytes; each byte's
// low 7 bits are the rows (bit 0 = bottom row). Pixel order: left = MSB.
// ---------------------------------------------------------------------------
namespace {

const uint8_t kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x14,0x08,0x3E,0x08,0x14}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x10,0x08,0x08,0x10,0x08}, // ~
};

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;

} // namespace

// ---------------------------------------------------------------------------
// FontManager
// ---------------------------------------------------------------------------

void FontManager::RegisterBuiltin() {
    if (builtinRegistered_) return;
    Font f;
    f.id = "builtin";
    f.family = "Builtin 5x7";
    f.cellWidth = kGlyphWidth;
    f.cellHeight = kGlyphHeight;
    f.builtin = true;
    fonts_[f.id] = f;
    builtinRegistered_ = true;
}

Result<void> FontManager::RegisterFont(const Font& font) {
    if (font.id.empty()) return Error::Make(Err::InvalidArgument, "Font", "empty id");
    fonts_[font.id] = font;
    return Ok();
}

Result<Font> FontManager::GetFont(std::string_view id) const {
    auto it = fonts_.find(id);
    if (it == fonts_.end())
        return Error::Make(Err::Render_FontNotFound, "Font",
                           "font '" + std::string(id) + "' not found");
    return it->second;
}

Font FontManager::Resolve(std::string_view id) const {
    auto it = fonts_.find(id);
    if (it != fonts_.end()) return it->second;
    auto b = fonts_.find("builtin");
    if (b != fonts_.end()) return b->second;   // fallback
    return Font{"builtin", "Builtin 5x7", kGlyphWidth, kGlyphHeight, true};
}

std::vector<std::string> FontManager::FontIds() const {
    std::vector<std::string> out;
    for (const auto& [id, f] : fonts_) {
        (void)f;
        out.push_back(id);
    }
    return out;
}

FontGlyph FontManager::Glyph(uint8_t codepoint, float sizePx) const {
    FontGlyph g;
    const uint8_t cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
    g.codepoint = cp;
    g.width = kGlyphWidth;
    g.height = kGlyphHeight;
    g.advance = kGlyphWidth + 1;   // 1px spacing
    // Scale relative to the requested size.
    const float scale = sizePx > 0 ? sizePx / static_cast<float>(kGlyphHeight) : 1.0f;
    g.width = static_cast<int>(static_cast<float>(g.width) * scale + 0.5f);
    g.height = static_cast<int>(static_cast<float>(g.height) * scale + 0.5f);
    g.advance = static_cast<int>(static_cast<float>(g.advance) * scale + 0.5f);
    return g;
}

RgbaImage FontManager::BuildAtlas(float sizePx,
                                  std::map<uint8_t, FontGlyph>& outGlyphs) const {
    const int cellW = kGlyphWidth;
    const int cellH = kGlyphHeight;
    const float scale = sizePx > 0 ? sizePx / static_cast<float>(kGlyphHeight) : 1.0f;
    const int scW = static_cast<int>(static_cast<float>(cellW) * scale + 0.5f);
    const int scH = static_cast<int>(static_cast<float>(cellH) * scale + 0.5f);
    const int cols = 16;
    const int rows = 6;   // covers 96 glyphs (32..126)
    RgbaImage atlas;
    atlas.width = cols * (scW + 1);
    atlas.height = rows * (scH + 1);
    atlas.pixels.assign(static_cast<size_t>(atlas.width) * atlas.height, 0u);

    int cell = 0;
    for (int cp = 32; cp <= 126 && cell < cols * rows; ++cp, ++cell) {
        const int cx = (cell % cols) * (scW + 1);
        const int cy = (cell / cols) * (scH + 1);
        const uint8_t* glyph = kFont5x7[cp - 32];
        // Each glyph byte is one column; the low 7 bits are the rows
        // (bit 0 = bottom row, per the font data layout comment above).
        for (int col = 0; col < cellW; ++col) {
            const uint8_t bits = glyph[col];
            for (int row = 0; row < cellH; ++row) {
                if ((bits & (1u << row)) == 0) continue;
                const int gx = cx + static_cast<int>(static_cast<float>(col) * scale);
                const int gy = cy + static_cast<int>(static_cast<float>(row) * scale);
                for (int dy = 0; dy < static_cast<int>(scale); ++dy)
                    for (int dx = 0; dx < static_cast<int>(scale); ++dx) {
                        const int px = gx + dx, py = gy + dy;
                        if (px < atlas.width && py < atlas.height)
                            atlas.pixels[static_cast<size_t>(py) * atlas.width + px] =
                                Color::White().Pack();
                    }
            }
        }
        FontGlyph g;
        g.codepoint = static_cast<uint8_t>(cp);
        g.x = cx;
        g.y = cy;
        g.width = scW;
        g.height = scH;
        g.advance = scW + 1;
        outGlyphs[static_cast<uint8_t>(cp)] = g;
    }
    return atlas;
}

// ---------------------------------------------------------------------------
// TextLayout
// ---------------------------------------------------------------------------

std::vector<std::string> TextLayout::SplitLines(const std::string& text, bool rtl) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty() || text.empty()) lines.push_back(cur);
    if (rtl) std::reverse(lines.begin(), lines.end());
    return lines;
}

float TextLayout::MeasureLine(const std::string& text, const TextStyle& style,
                              const FontManager& fonts) {
    if (text.empty()) return 0.0f;
    Font f = fonts.Resolve(style.fontId);
    float advance = f.cellWidth > 0 ? style.size * static_cast<float>(f.cellWidth + 1) /
                                          static_cast<float>(f.cellHeight)
                                    : style.size;
    return static_cast<float>(text.size()) * advance +
           static_cast<float>(text.size() - 1) * style.letterSpacing;
}

TextLayoutResult TextLayout::Measure(const std::string& text, const TextStyle& style,
                                     const FontManager& fonts) {
    TextLayoutResult result;
    auto raw = SplitLines(text, style.rtl);
    Font f = fonts.Resolve(style.fontId);
    const float lineHeight = style.size + style.lineSpacing;
    const float advance = f.cellWidth > 0 ? style.size * static_cast<float>(f.cellWidth + 1) /
                                                static_cast<float>(f.cellHeight)
                                          : style.size;

    for (const auto& rawLine : raw) {
        if (!style.wrap || style.wrapWidth <= 0.0f || rawLine.empty()) {
            LineLayout ll;
            ll.text = rawLine;
            ll.width = static_cast<float>(rawLine.size()) * advance +
                       static_cast<float>(rawLine.size() > 0 ? rawLine.size() - 1 : 0) *
                           style.letterSpacing;
            result.totalWidth = std::max(result.totalWidth, ll.width);
            result.totalHeight += lineHeight;
            result.lines.push_back(std::move(ll));
            continue;
        }
        // Word wrap.
        std::string current;
        float currentWidth = 0.0f;
        for (char ch : rawLine) {
            const float chWidth = advance;
            if (currentWidth + chWidth > style.wrapWidth && !current.empty()) {
                result.totalWidth = std::max(result.totalWidth, currentWidth);
                result.totalHeight += lineHeight;
                result.lines.push_back({current, currentWidth});
                current.clear();
                currentWidth = 0.0f;
            }
            current.push_back(ch);
            currentWidth += chWidth;
        }
        if (!current.empty()) {
            result.totalWidth = std::max(result.totalWidth, currentWidth);
            result.totalHeight += lineHeight;
            result.lines.push_back({current, currentWidth});
        }
    }
    return result;
}

} // namespace bps::rendering
