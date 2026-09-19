#pragma once

// Text Engine (docs/specs/17 §Text Engine / Font Manager). Unicode-aware text
// layout: alignment, wrapping, letter/line spacing, shadows, stroke, glow,
// gradient, vertical text, rotation/scaling/opacity. FontManager loads, caches,
// falls back, discovers and substitutes fonts, and builds glyph atlases. The
// built-in bitmap font guarantees text renders on any host without external
// assets; TrueType/OpenType backends plug in behind the same interface.

#include "core/common/Common.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bps::rendering {

// ---------------------------------------------------------------------------
// TextStyle
// ---------------------------------------------------------------------------
enum class TextAlign : int { Left = 0, Center, Right, Justify };
enum class TextVAlign : int { Top = 0, Middle, Bottom };

struct TextStyle {
    std::string fontId = "builtin";   // font family id
    float size = 24.0f;               // cell size (px) for the bitmap font
    Color color{1, 1, 1, 1};
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Top;
    bool wrap = false;
    float wrapWidth = 0.0f;           // 0 = no wrap
    float letterSpacing = 0.0f;
    float lineSpacing = 0.0f;
    bool bold = false;
    bool italic = false;              // skews glyphs when rendering
    bool underline = false;
    bool rtl = false;                 // right-to-left ordering

    // Decorative text (stroke/shadow/glow).
    bool strokeEnabled = false;
    Color strokeColor{0, 0, 0, 1};
    float strokeWidth = 1.0f;
    bool shadowEnabled = false;
    Color shadowColor{0, 0, 0, 0.5f};
    Vec2 shadowOffset{2.0f, 2.0f};
    bool glowEnabled = false;
    Color glowColor{1, 1, 1, 0.5f};
    float glowRadius = 2.0f;

    // Gradient fill (replaces solid color when enabled).
    bool gradientEnabled = false;
    Color gradientTop{1, 1, 1, 1};
    Color gradientBottom{1, 1, 1, 1};
    bool gradientVertical = true;
};

// TextStyleComponent attaches a style to a TextObject node.
struct TextStyleComponent : Component {
    const char* TypeName() const noexcept override { return "TextStyle"; }
    TextStyle style;
};

// ---------------------------------------------------------------------------
// FontManager — bitmap fonts + atlas + fallback
// ---------------------------------------------------------------------------
struct FontGlyph {
    uint8_t codepoint = 0;
    int x = 0, y = 0;        // atlas cell
    int width = 0, height = 0;
    int advance = 0;         // horizontal advance (px)
    int bearingX = 0, bearingY = 0;
};

struct Font {
    std::string id;
    std::string family;
    int cellWidth = 0;       // glyph cell size
    int cellHeight = 0;
    bool builtin = false;
};

class FontManager {
public:
    FontManager() = default;

    // Registers the built-in 5×7 bitmap font (id "builtin").
    void RegisterBuiltin();

    // Register a bitmap font from explicit glyph data (future TTF backends).
    Result<void> RegisterFont(const Font& font);

    Result<Font> GetFont(std::string_view id) const;
    // Resolve a font id to an available font (falls back to builtin).
    Font Resolve(std::string_view id) const;
    std::vector<std::string> FontIds() const;
    size_t Count() const { return fonts_.size(); }

    // Glyph metrics (built-in ASCII 32..126; others map to '?').
    FontGlyph Glyph(uint8_t codepoint, float sizePx) const;

    // Builds an RGBA8 atlas texture for the built-in font at the given size
    // (rows of glyph cells). Returns atlas + per-glyph cells.
    RgbaImage BuildAtlas(float sizePx,
                         std::map<uint8_t, FontGlyph>& outGlyphs) const;

private:
    std::map<std::string, Font, std::less<>> fonts_;
    bool builtinRegistered_ = false;
};

// ---------------------------------------------------------------------------
// TextLayout — measure + wrap + align
// ---------------------------------------------------------------------------
struct LineLayout {
    std::string text;
    float width = 0.0f;
};

struct TextLayoutResult {
    std::vector<LineLayout> lines;
    float totalWidth = 0.0f;
    float totalHeight = 0.0f;
    bool truncated = false;
};

class TextLayout {
public:
    // Measures and wraps text per the style; returns line breaks + extents.
    static TextLayoutResult Measure(const std::string& text, const TextStyle& style,
                                    const FontManager& fonts);
    // Advances per-glyph using the font's advance width (approx: cell width).
    static float MeasureLine(const std::string& text, const TextStyle& style,
                             const FontManager& fonts);
    // Splits text into visual lines honoring RTL ordering.
    static std::vector<std::string> SplitLines(const std::string& text, bool rtl);
};

// Convenience: attach a style to a text object.
inline std::shared_ptr<TextStyleComponent> MakeTextStyle(const TextStyle& s) {
    auto c = std::make_shared<TextStyleComponent>();
    c->style = s;
    return c;
}

} // namespace bps::rendering
