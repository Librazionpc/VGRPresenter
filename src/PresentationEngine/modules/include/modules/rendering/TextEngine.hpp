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
#include <mutex>
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
    // Extra advance added after every SPACE glyph, in the same units as
    // letterSpacing (an additive px value whose neutral is 0). Measured and
    // drawn from this one field, and carried in the layout cache key, so a
    // layout measured with it can never be served to a style without it.
    float wordSpacing = 0.0f;
    float lineSpacing = 0.0f;
    // Extra space BETWEEN lines, additive px, on top of lineSpacing — which
    // carries the line-height multiplier. Two fields because they answer two
    // different questions: lineSpacing sets how tall a LINE BOX is (and so how
    // tall a per-line background band is), lineGap is blank air inserted
    // between one line box and the next (FreeShow's "line spacing" row).
    // Neutral 0 = no extra gap.
    float lineGap = 0.0f;
    // Per-LINE background band (FreeShow's lineBg): the fill drawn behind EACH
    // line's own ink width, so a verse can be highlighted line by line instead
    // of painting one rectangle behind the whole block (that is the item's own
    // background, a different control). Alpha 0 = none.
    Color lineBg{0, 0, 0, 0};
    // Corner radius of those bands, in DESTINATION pixels (FreeShow's
    // `lineRadius`). 0 draws plain rectangles — the cheap path — and anything
    // above it goes out as a rounded DrawShape, which both backends rasterize.
    // Clamped to half the band's height (a radius past that is a stadium, not a
    // different shape).
    float lineRadius = 0.0f;
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

    // REAL system font backend (Windows GDI+, docs/specs/17's own "TrueType/
    // OpenType backends plug in behind the same interface"). `family` names
    // an installed font ("Segoe UI"...); ASCII 32..126, antialiased, with
    // REAL measured per-glyph advances — not the builtin font's uniform
    // cellWidth-ratio estimate — so wrapping and drawing agree on width.
    // Returns an empty atlas on non-Windows or if the family/GDI+ can't be
    // resolved; every caller falls back to BuildAtlas (the builtin font)
    // when this comes back empty, so a missing font never blanks a slide.
    RgbaImage BuildSystemAtlas(const std::string& family, float sizePx, bool bold, bool italic,
                               std::map<uint8_t, FontGlyph>& outGlyphs) const;
    // True if `family` is usable as a system font on this platform right
    // now (GDI+ available and the family resolves) — callers branch on
    // this rather than trying BuildSystemAtlas and inspecting for empty,
    // since an empty atlas can also legitimately mean "no glyphs fit".
    bool HasSystemFont(const std::string& family) const;

    // The family a style should ACTUALLY draw with: `family` when the host
    // resolves it, otherwise the first installed substitute (the app's UI
    // default, then the faces every Windows install carries). Empty when no
    // substitute resolves — the caller then keeps the builtin bitmap font.
    //
    // This is the difference between "the design names a font this machine does
    // not have" and "the output paints giant pixel blocks": the builtin 5x7
    // font is pixel-doubled to the requested size, so at a lower third's 1080p
    // text size it drew ~13x blocks instead of type, with monospace advances
    // that wrapped the verse off the frame. QML surfaces never showed it (Qt
    // substitutes silently), and the machine that AUTHORED the design has the
    // font — which is how one install renders correctly and another does not.
    // Every consumer (layout measure, atlas build, atlas texture identity) must
    // resolve through THIS so what is measured is what is painted.
    std::string SubstituteFamily(std::string_view family) const;

    // The face the OPERATING SYSTEM itself uses for UI text (Windows: the shell's
    // message font from NONCLIENTMETRICS), or "" when no system-font backend can
    // answer. This is the one substitute that is not a literal name: the point of
    // a font-agnostic renderer is that nothing in it decides "Segoe UI" on the
    // app's behalf — the host already has a UI face, and the engine borrows it.
    // Cached (it is asked per text object through the substitution path).
    std::string SystemUiFamily() const;

    // The face to draw with when a design names NONE — SystemUiFamily(), and
    // "builtin" only when no system font backend resolves at all.
    std::string DefaultFamily() const;
    // Real measured advance width (px) for ONE character in the named
    // system font — what TextLayout::Measure/MeasureLine call per
    // character when the style names a real font, instead of the builtin
    // font's uniform estimate. 0 if the family can't be resolved.
    float SystemCharAdvance(const std::string& family, float sizePx, bool bold, bool italic,
                            uint8_t codepoint) const;
    // Batch form — TextLayout::Measure's own word-wrap loop needs many
    // characters' widths per call; this builds ONE GDI+ Font/Graphics
    // context for the whole string instead of SystemCharAdvance's
    // per-call setup cost repeated per character. One entry per character
    // of `text`, same order; empty if the family doesn't resolve (the
    // caller falls back to the builtin estimate for the whole string).
    std::vector<float> SystemCharAdvances(const std::string& family, float sizePx, bool bold,
                                          bool italic, const std::string& text) const;

    // The font's REAL vertical rhythm: line gap between baselines and the
    // last line's ink extent (ascent+descent, GDI+ GetCellDescent/GetLineSpacing
    // family). Every other font metric in this engine is already REAL
    // (SystemCharAdvances) — line height stayed the one estimated number,
    // and it diverges per font (a display serif's natural line spacing is
    // ~15% taller than Segoe UI's at the same pixel size), which is exactly
    // the "fit works with one font, clips with another" report. False when
    // the family can't be resolved or off Windows: callers fall back to the
    // size+lineSpacing estimate, unchanged.
    struct LineMetrics {
        float baselineStep = 0.0f;   // natural baseline-to-baseline distance
        float inkHeight = 0.0f;      // ascent+descent — one line's visual height
    };
    bool SystemLineMetrics(const std::string& family, float sizePx, bool bold, bool italic,
                           LineMetrics& out) const;

private:
    std::map<std::string, Font, std::less<>> fonts_;
    bool builtinRegistered_ = false;
    // Substitution answers, keyed by the REQUESTED family (it is asked on every
    // measured and painted text object, and resolving costs a GDI+ FontFamily).
    // Cached because the set of installed fonts does not change under a running
    // process; a family that was missing stays missing for the session.
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
};

struct TextLayoutResult {
    std::vector<LineLayout> lines;
    float totalWidth = 0.0f;
    // Total painted extent. For real fonts this is (nLines-1)·baselineStep +
    // last-line ink (ascent+descent) — NOT n·lineHeight: a baseline grid's
    // last line carries only its own ink, and counting a full line box for
    // it over-measures by (descender of line n−1 + ascender gap), which
    // grows with the font's natural line height — the font-dependent
    // "fits the solver, clips the last line" bug. Builtin font: n·step,
    // its glyphs are uniform cells with no extra ink beyond the cell.
    // DrawTextObject's own per-line painting model matches this exactly.
    float totalHeight = 0.0f;
    // The per-line advance Measure used (real-font baseline step, or the
    // size+lineSpacing estimate). DrawTextObject MUST step lines by THIS —
    // the value it derives from style alone can diverge for real fonts,
    // and painted spacing that disagrees with measured spacing un-does the
    // fit solve above (fit says it fits; the paint spreads and clips).
    float lineStep = 0.0f;
    // The height of ONE line BOX — the visible band a per-line background
    // (TextStyle::lineBg) covers. It is the font's own ascent+descent, NOT
    // lineStep: Qt paints a rich-text span's background at the natural line
    // box, so the extra line height shows as air between the bands. The canvas
    // and the previews draw their bands with Qt's own painting for exactly that
    // reason, so the engine must band the same box or the output's bands are
    // taller than the ones the operator approved on screen.
    float lineBox = 0.0f;
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
