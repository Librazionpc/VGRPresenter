#pragma once

#include "core/common/Common.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <QFont>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {
// ---------------------------------------------------------------------------
// TextStyle
// ---------------------------------------------------------------------------
enum class TextAlign : int { Left = 0, Center, Right, Justify };
enum class TextVAlign : int { Top = 0, Middle, Bottom };

// LINE BREAKS DECIDED ELSEWHERE. The reading column's breaks are cut once, at the stage's
// own size, by the service that also steps the walk (the table every monitor surface
// scrolls through). A surface that is handed them draws exactly those lines, scaled to
// its output, instead of wrapping the text again at a size and width of its own — which is
// what let two screens put different words on a line. `textHash` ties the table to the text
// it was cut from (TextLayout::TextHash); text that does not match it is wrapped normally.
struct ForcedLineBreaks {
    std::vector<int> starts;      // UTF-16 offset of each line's first character
    uint64_t textHash = 0;
};

struct TextStyle {
    std::string fontId = "builtin";   // font family id
    std::shared_ptr<const ForcedLineBreaks> forcedBreaks;   // null = wrap by wrapWidth
    float size = 24.0f;               // cell size (px) for the bitmap font
    Color color{1, 1, 1, 1};
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Top;
    bool wrap = false;
    float wrapWidth = 0.0f;           // 0 = no wrap
    float letterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    float lineSpacing = 0.0f;
    float lineGap = 0.0f;
    Color lineBg{0, 0, 0, 0};
    float lineRadius = 0.0f;
    bool bold = false;
    QFont::Weight fontWeight = QFont::Medium;
    bool italic = false;              // skews glyphs when rendering
    bool underline = false;
    bool strikeout = false;
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
    float advance = 0.0f;    // horizontal advance (px)
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

    // QFont pixel sizes are positive integers; truncation matches QML's
    // font.pixelSize behavior and is also used by the font-atlas cache key.
    static int QtPixelSize(float sizePx);

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

    RgbaImage BuildSystemAtlas(const std::string& family, float sizePx, bool bold, bool italic,
                               std::map<uint8_t, FontGlyph>& outGlyphs,
                               QFont::Weight fontWeight = QFont::Medium, bool underline = false,
                               bool strikeout = false) const;
    bool HasSystemFont(const std::string& family) const;

    std::string SubstituteFamily(std::string_view family) const;

    std::string SystemUiFamily() const;

    // The face to draw with when a design names NONE — SystemUiFamily(), and
    // "builtin" only when no system font backend resolves at all.
    std::string DefaultFamily() const;
    float SystemCharAdvance(const std::string& family, float sizePx, bool bold, bool italic,
                            uint8_t codepoint, QFont::Weight fontWeight = QFont::Medium) const;
    // Batch form — Qt horizontal advances, one entry per character of `text`
    // in the same order. Empty if the family doesn't resolve.
    std::vector<float> SystemCharAdvances(const std::string& family, float sizePx, bool bold,
                                          bool italic, const std::string& text,
                                          QFont::Weight fontWeight = QFont::Medium) const;

    struct LineMetrics {
        float baselineStep = 0.0f;   // natural baseline-to-baseline distance
        float inkHeight = 0.0f;      // ascent+descent — one line's visual height
    };
    bool SystemLineMetrics(const std::string& family, float sizePx, bool bold, bool italic,
                           LineMetrics& out, QFont::Weight fontWeight = QFont::Medium) const;

private:
    std::map<std::string, Font, std::less<>> fonts_;
    bool builtinRegistered_ = false;
    mutable std::mutex substituteMutex_;
    mutable std::map<std::string, std::string, std::less<>> substitutes_;
    mutable std::string systemUiFamily_;
    mutable bool systemUiFamilyResolved_ = false;
};

// ---------------------------------------------------------------------------
// TextLayout — measure + wrap + align
// ---------------------------------------------------------------------------
struct LineLayout {
    std::string text;
    float width = 0.0f;
    bool paragraphEnd = true;
};

struct TextLayoutResult {
    std::vector<LineLayout> lines;
    float totalWidth = 0.0f;
    float totalHeight = 0.0f;
    float lineStep = 0.0f;
    float lineBox = 0.0f;
    bool truncated = false;
};

class TextLayout {
public:
    // Measures and wraps text per the style; returns line breaks + extents.
    static TextLayoutResult Measure(const std::string& text, const TextStyle& style,
                                    const FontManager& fonts);
    // Character offsets for the measured lines, expressed in UTF-16 code units
    // so callers sharing positions with Qt/QML use the same unit.
    static std::vector<int> FlowCharacterStarts(const std::string& text,
                                                const TextStyle& style,
                                                const FontManager& fonts);
    // Advances per-glyph using the font's advance width (approx: cell width).
    static float MeasureLine(const std::string& text, const TextStyle& style,
                             const FontManager& fonts);
    // Splits text into visual lines honoring RTL ordering.
    static std::vector<std::string> SplitLines(const std::string& text, bool rtl);
    // The identity ForcedLineBreaks is tied to: FNV-1a over the text's UTF-8 bytes. The
    // service hashes the same bytes with this same function.
    static uint64_t TextHash(const std::string& text);
};

// Convenience: attach a style to a text object.
inline std::shared_ptr<TextStyleComponent> MakeTextStyle(const TextStyle& s) {
    auto c = std::make_shared<TextStyleComponent>();
    c->style = s;
    return c;
}
} // namespace bps::rendering