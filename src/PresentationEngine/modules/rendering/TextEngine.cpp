#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// Real system font backend (Windows GDI+) — BuildSystemAtlas/HasSystemFont/
// SystemCharAdvance below. gdiplus.lib is already linked (CMakeLists' win32
// block, alongside gdi32) for the app's own icon/bitmap handling, so this
// needs no new dependency. GDI+ (not classic GDI) specifically because its
// text rendering is alpha-aware — DrawString onto a 32bppARGB bitmap with
// AntiAlias hinting produces a real per-pixel coverage alpha channel, which
// is exactly the "white glyph, alpha = shape" atlas convention DrawGlyph's
// tint-blit (GraphicsBackends.cpp) already expects; classic GDI's TextOut
// does not populate alpha at all on a 32bpp DIB.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace {

// One-time process-wide GDI+ init. Never shut down: this matches the
// process lifetime and sidesteps GDI+'s well-known shutdown-ordering
// fragility (GdiplusShutdown after static destructors have already run
// elsewhere in the process can crash on exit) — an intentional, common
// leak-on-purpose for an app-lifetime graphics subsystem.
void EnsureGdiplusStarted() {
    static bool started = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        return true;
    }();
    (void)started;
}

// GDI+ objects (Font/FontFamily/Bitmap/Graphics) are NOT documented as
// thread-safe for concurrent use, and this engine renders MULTIPLE passes
// (the shared preview + one gated pass per styled output) on separate
// threads — live evidence (TEMPDIAG) showed DrawTextObject firing from
// three different thread ids concurrently. RenderCache's own mutex only
// serializes within ONE cache instance; with a separate RenderEngine/
// RenderCache per pass, two instances could still call INTO GDI+ at the
// exact same moment. One process-wide lock around every GDI+ call below
// (not just the cache's map bookkeeping) is the standard fix for this
// class of bug — reported live as "text missing/black on first go-live,
// shows correctly after reselecting" (a race, not a hard failure, exactly
// matches unsynchronized GDI+ corruption rather than a real logic bug).
std::mutex& GdiplusMutex() {
    static std::mutex m;
    return m;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// A ready-to-use GDI+ Font for (family, size, bold, italic), or nullptr if
// the family doesn't resolve — the single point every method below asks
// "is this font usable" through.
std::unique_ptr<Gdiplus::Font> MakeGdiplusFont(const std::string& family, float sizePx,
                                               bool bold, bool italic) {
    EnsureGdiplusStarted();
    const std::wstring wfamily = Utf8ToWide(family);
    if (wfamily.empty()) return nullptr;
    Gdiplus::FontFamily fontFamily(wfamily.c_str());
    if (fontFamily.GetLastStatus() != Gdiplus::Ok) return nullptr;
    int style = Gdiplus::FontStyleRegular;
    if (bold) style |= Gdiplus::FontStyleBold;
    if (italic) style |= Gdiplus::FontStyleItalic;
    auto font = std::make_unique<Gdiplus::Font>(&fontFamily, sizePx, style, Gdiplus::UnitPixel);
    if (font->GetLastStatus() != Gdiplus::Ok) return nullptr;
    return font;
}

// Tight (no paragraph padding) string format — GDI+'s default MeasureString
// pads for line-break fitting, which over-reports single-glyph width; the
// well-known fix is GenericTypographic + these two flags.
const Gdiplus::StringFormat& TightFormat() {
    // GDI+'s StringFormat copy ctor is private — Clone() (returning a
    // generic Gdiplus::Image*-style base) is the documented way to get a
    // mutable copy of the built-in GenericTypographic format.
    static const Gdiplus::StringFormat* fmt = [] {
        auto* f = Gdiplus::StringFormat::GenericTypographic()->Clone();
        f->SetFormatFlags(f->GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces
                          | Gdiplus::StringFormatFlagsNoWrap);
        return f;
    }();
    return *fmt;
}

// True when the font actually OWNS `ch`'s glyph — GDI, marking
// non-existing glyphs. GDI+ silently draws a font's .notdef for characters
// it lacks, and .notdef is often BLANK ink: that is exactly how "TT Nooks
// Trial" (a display font with no U+2019 in its cmap) made every folded
// apostrophe vanish from the engine outputs while the QML tiles (Qt falls
// back across fonts) kept showing them.
bool FontOwnsGlyph(const std::string& family, bool bold, wchar_t ch) {
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return true;   // can't ask: assume yes (draw as before)
    // Glyph PRESENCE is a property of the font's cmap, not of the size —
    // a fixed height keeps this cheap and exact.
    HFONT f = CreateFontW(-24, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE,
                          FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, Utf8ToWide(family).c_str());
    if (!f) {
        DeleteDC(hdc);
        return true;
    }
    HFONT old = static_cast<HFONT>(SelectObject(hdc, f));
    WORD index = 0;
    const BOOL ok = GetGlyphIndicesW(hdc, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(hdc, old);
    DeleteObject(f);
    DeleteDC(hdc);
    return ok && index != 0xFFFF;
}

// The wide char a codepoint's atlas cell draws AND measures: the folded
// apostrophe renders as the typographic U+2019 WHEN the font owns it, else
// as the ASCII quote it does have. Every site (atlas measure pass, atlas
// draw pass, advance batch) must ask through THIS helper so wrap widths and
// rasterized ink stay in agreement for fonts both with and without the
// curly glyph. Call it ONCE per operation and reuse the result per loop —
// it runs a font-metrics query.
wchar_t AtlasCharFor(const std::string& family, bool bold, uint8_t cp) {
    if (cp == 0x27)
        return FontOwnsGlyph(family, bold, L'\u2019') ? L'\u2019' : L'\'';
    return static_cast<wchar_t>(cp);
}

} // namespace
#endif // _WIN32

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

#ifdef _WIN32
bool FontManager::HasSystemFont(const std::string& family) const {
    std::lock_guard<std::mutex> lock(GdiplusMutex());
    return MakeGdiplusFont(family, 24.0f, false, false) != nullptr;
}

float FontManager::SystemCharAdvance(const std::string& family, float sizePx, bool bold,
                                     bool italic, uint8_t codepoint) const {
    std::lock_guard<std::mutex> lock(GdiplusMutex());
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);
    if (!font) return 0.0f;
    // A 1x1 bitmap's Graphics is a real, valid measuring context (GDI+
    // measurement doesn't rasterize into it — a device context is just
    // where the DPI/transform come from) without needing a window or the
    // full atlas bitmap this method is called ahead of building.
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&probe);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    // Same apostrophe rule the atlas draws by (AtlasCharFor) — this single-
    // char path measured the straight quote while the batch path measured
    // U+2019, disagreeing with the ink whenever a style named a font
    // lacking the curly glyph.
    const wchar_t ch = AtlasCharFor(family, bold, codepoint);
    Gdiplus::RectF bounds;
    g.MeasureString(&ch, 1, font.get(), Gdiplus::PointF(0, 0), &TightFormat(), &bounds);
    return bounds.Width;
}

std::vector<float> FontManager::SystemCharAdvances(const std::string& family, float sizePx,
                                                    bool bold, bool italic,
                                                    const std::string& text) const {
    std::lock_guard<std::mutex> lock(GdiplusMutex());
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);
    if (!font) return {};
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&probe);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    std::vector<float> out;
    out.reserve(text.size());
    // Folded apostrophes measure as the TYPOGRAPHIC glyph they'll draw as
    // (AtlasCharFor) so wrapped line widths match the rasterized output byte
    // for byte — and a font without U+2019 measures the ASCII quote it will
    // actually render, never a blank .notdef advance.
    const wchar_t quote = AtlasCharFor(family, bold, 0x27);
    for (unsigned char c : text) {
        const wchar_t ch = c == 0x27 ? quote : static_cast<wchar_t>(c);
        Gdiplus::RectF bounds;
        g.MeasureString(&ch, 1, font.get(), Gdiplus::PointF(0, 0), &TightFormat(), &bounds);
        out.push_back(std::max(1.0f, bounds.Width));
    }
    return out;
}

bool FontManager::SystemLineMetrics(const std::string& family, float sizePx, bool bold,
                                    bool italic, LineMetrics& out) const {
    std::lock_guard<std::mutex> lock(GdiplusMutex());
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);   // resolves the family
    if (!font) return false;
    Gdiplus::FontFamily ff;
    if (font->GetFamily(&ff) != Gdiplus::Ok) return false;
    const int fstyle = font->GetStyle();
    if (ff.GetLastStatus() != Gdiplus::Ok || ff.GetEmHeight(fstyle) <= 0) return false;
    // Font design units -> pixels at THIS raster size (the design metrics are
    // the same font data at every size; the em height in units is not). The
    // metrics are queried for the font's own style — a synthesized bold/italic
    // leaves the family's vertical metrics alone, but querying with the real
    // style keeps the call honest for families that ship per-style metrics.
    const float scale = sizePx / static_cast<float>(ff.GetEmHeight(fstyle));
    const float step = static_cast<float>(ff.GetLineSpacing(fstyle)) * scale;
    const float ink = (static_cast<float>(ff.GetCellAscent(fstyle))
                       + static_cast<float>(ff.GetCellDescent(fstyle))) * scale;
    if (step <= 0.0f || ink <= 0.0f) return false;
    out.baselineStep = step;
    out.inkHeight = ink;
    return true;
}

RgbaImage FontManager::BuildSystemAtlas(const std::string& family, float sizePx, bool bold,
                                        bool italic, std::map<uint8_t, FontGlyph>& outGlyphs) const {
    std::lock_guard<std::mutex> lock(GdiplusMutex());
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);
    if (!font) return {};

    // Pass 1: measure every glyph's real advance + a shared cell size (the
    // tallest/widest glyph at this size+weight) so the atlas grid is sized
    // once, like BuildAtlas's own two-pass shape (measure via scale, draw
    // via cells) — a per-glyph-sized packer would draw correctly but is
    // more atlas-management complexity than this ASCII-only, 96-glyph atlas
    // needs.
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics measureG(&probe);
    measureG.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    std::map<uint8_t, float> advances;
    float cellW = 1.0f, cellH = sizePx * 1.4f;   // line-height-ish floor for descenders
    const wchar_t quote = AtlasCharFor(family, bold, 0x27);
    for (int cp = 32; cp <= 126; ++cp) {
        const wchar_t ch = cp == 0x27 ? quote : static_cast<wchar_t>(cp);
        Gdiplus::RectF bounds;
        measureG.MeasureString(&ch, 1, font.get(), Gdiplus::PointF(0, 0), &TightFormat(), &bounds);
        const float adv = std::max(1.0f, bounds.Width);
        advances[static_cast<uint8_t>(cp)] = adv;
        cellW = std::max(cellW, adv);
        cellH = std::max(cellH, bounds.Height);
    }
    const int scW = static_cast<int>(cellW + 1.5f);
    const int scH = static_cast<int>(cellH + 1.5f);
    const int cols = 16;
    const int rows = 6;   // covers 96 glyphs (32..126), same layout as BuildAtlas
    // Cell gap: GDI+ glyph ink spills ~1-2px past its measured advance/height
    // (antialiasing + overshoot). The old 1px packing let that spill cross
    // into the NEIGHBOR cell's blit region — every glyph that followed a
    // wide-ink character carried a small foreign tick (the stray
    // "apostrophe" marks under ti/nt/ro in live text). 8px absorbs it.
    constexpr int kCellGap = 8;

    Gdiplus::Bitmap sheet(cols * (scW + kCellGap), rows * (scH + kCellGap), PixelFormat32bppARGB);
    Gdiplus::Graphics draw(&sheet);
    draw.Clear(Gdiplus::Color(0, 0, 0, 0));
    draw.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    draw.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));

    int cell = 0;
    for (int cp = 32; cp <= 126 && cell < cols * rows; ++cp, ++cell) {
        const int cx = (cell % cols) * (scW + kCellGap);
        const int cy = (cell / cols) * (scH + kCellGap);
        // The apostrophe cell draws the TYPOGRAPHIC U+2019 glyph, not the
        // ASCII straight quote. FoldToAtlasAscii maps every ’ the transcripts
        // use onto 0x27 so the byte-wise draw loop always hits the atlas — but
        // rasterizing the straight quote made every one of them read as a big
        // harsh tick (the live "the ' is too much" report) next to the QML
        // tiles, which render the real curly glyph via Qt. Same key, prettier
        // ink — WHEN the font owns it: a display font without U+2019 drew
        // .notdef (blank) here and the apostrophe vanished entirely;
        // AtlasCharFor falls back to the ASCII quote so every font shows
        // SOMETHING for the folded apostrophe, and the measure pass above
        // made the identical choice.
        const wchar_t ch = cp == 0x27 ? quote : static_cast<wchar_t>(cp);
        draw.DrawString(&ch, 1, font.get(), Gdiplus::PointF(static_cast<float>(cx), static_cast<float>(cy)),
                        &TightFormat(), &white);
        FontGlyph g;
        g.codepoint = static_cast<uint8_t>(cp);
        g.x = cx;
        g.y = cy;
        g.width = scW;
        g.height = scH;
        g.advance = static_cast<int>(advances[static_cast<uint8_t>(cp)] + 0.5f);
        outGlyphs[static_cast<uint8_t>(cp)] = g;
    }

    // Read the bitmap back as the engine's own RGBA8 (0xAABBGGRR) format —
    // GDI+'s 32bppARGB scan0 is BGRA byte order in memory (little-endian),
    // so R and B swap on the way out (see Color::Pack's own comment for the
    // engine's convention; ARGB32 vs RGBA8888 mixed up the same two channels
    // the LivePreviewProvider comment already flags as an easy mistake here).
    Gdiplus::BitmapData data;
    Gdiplus::Rect full(0, 0, sheet.GetWidth(), sheet.GetHeight());
    if (sheet.LockBits(&full, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return {};
    RgbaImage atlas;
    atlas.width = static_cast<int>(sheet.GetWidth());
    atlas.height = static_cast<int>(sheet.GetHeight());
    atlas.pixels.resize(static_cast<size_t>(atlas.width) * atlas.height);
    const auto* src = static_cast<const uint8_t*>(data.Scan0);
    for (int y = 0; y < atlas.height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < atlas.width; ++x) {
            const uint8_t b = row[x * 4 + 0], gCh = row[x * 4 + 1],
                          r = row[x * 4 + 2], a = row[x * 4 + 3];
            atlas.pixels[static_cast<size_t>(y) * atlas.width + x] =
                (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16)
                | (static_cast<uint32_t>(gCh) << 8) | static_cast<uint32_t>(r);
        }
    }
    sheet.UnlockBits(&data);
    return atlas;
}
#else
bool FontManager::HasSystemFont(const std::string&) const { return false; }
float FontManager::SystemCharAdvance(const std::string&, float, bool, bool, uint8_t) const { return 0.0f; }
std::vector<float> FontManager::SystemCharAdvances(const std::string&, float, bool, bool,
                                                    const std::string&) const { return {}; }
bool FontManager::SystemLineMetrics(const std::string&, float, bool, bool, LineMetrics&) const {
    return false;
}
RgbaImage FontManager::BuildSystemAtlas(const std::string&, float, bool, bool,
                                        std::map<uint8_t, FontGlyph>&) const { return {}; }
#endif

// ---------------------------------------------------------------------------
// TextLayout
// ---------------------------------------------------------------------------

namespace {
// Defined below (with the other layout helpers); SplitLines is the fold's
// first call site, so it needs the name visible up here.
std::string FoldToAtlasAscii(const std::string& text);
} // namespace

std::vector<std::string> TextLayout::SplitLines(const std::string& text, bool rtl) {
    // Fold to atlas ASCII up front: the draw loop (RenderEngine::DrawTextObject)
    // steps line text BYTE-wise against an ASCII-only atlas, and the RTL path
    // below reverses these very bytes — raw UTF-8 would both miss the atlas
    // (the "that     s" apostrophe gap) and reverse multibyte sequences into
    // mojibake. Every consumer of LineLayout::text draws via that loop, so
    // folding here covers all of them.
    const std::string folded = FoldToAtlasAscii(text);
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : folded) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty() || folded.empty()) lines.push_back(cur);
    if (rtl) std::reverse(lines.begin(), lines.end());
    return lines;
}

namespace {
// Fold the Unicode punctuation the transcripts actually use (evidence: a
// corpus-wide scan of src/downloads_vgr_txt — U+2019 715k hits, U+201C/D
// 225k each, U+2026 127k, U+2014 94k, U+2018 8k, plus a long tail of
// accented letters) down to the ASCII that BOTH glyph atlases contain
// (BuildAtlas and BuildSystemAtlas rasterize codepoints 32..126 only).
//
// Without this, every U+2019 (') reached the layout/draw path as its 3-byte
// UTF-8 encoding E2 80 99: none of those bytes are in the atlas, so
// RenderEngine::DrawTextObject's per-byte glyph lookup missed three times
// per apostrophe, each miss burning a style.size*0.6 blank advance — live
// symptom: "that     s when" instead of "that's" on the engine-rendered
// output (NDI), while the QML tiles (Qt's own text stack) rendered fine.
//
// Folding at LAYOUT ENTRY (not just at draw time) keeps every width
// calculation — wrap points, line widths, centering — computed on exactly
// the same characters the atlas draw loop will later step through, and
// keeps layout cache keys (raw text) deduplicating lines that differ only
// in punctuation flavor. Runs are scanned with a hand-rolled decoder
// rather than std::codecvt (deprecated in C++17) or MultiByteToWideChar
// (Windows-only; this TU also builds for the test host).
std::string FoldToAtlasAscii(const std::string& text) {
    bool asciiOnly = true;
    for (unsigned char c : text)
        if (c >= 0x80) { asciiOnly = false; break; }
    if (asciiOnly) return text;

    // UTF-8 codepoint -> ASCII replacement ('\0' slot = drop silently;
    // reserved for control/zero-width characters we don't want a gap for).
    const auto fold = [](uint32_t cp) -> char {
        switch (cp) {
            case 0x2018: case 0x2019: case 0x201B: case 0x2032:
                return '\'';   // left/right single quotes, primes
            case 0x201C: case 0x201D: case 0x201F:
                return '"';    // double quotes
            case 0x2011: case 0x2013: case 0x2014: case 0x2212:
                return '-';     // nb-hyphen, en/em dash, minus
            case 0x037E: return ';';  // Greek question mark (';' lookalike, in corpus)
            case 0x2026: return '.';  // ellipsis: "…" -> "..." below
            case 0x00A0: case 0x2007: case 0x202F:
                return ' ';     // no-break / figure / narrow spaces
            case 0x00A9: return 'c';  // (c)
            case 0x00AE: return 'r';  // (r)
            case 0x00B0: return '*';  // degree
            case 0x2022: return '*';  // bullet
            case 0x00AD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF:
                return '\0';   // soft hyphen / zero-width: drop (no gap)
            default: return '\1'; // unmapped: handled by caller
        }
    };

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { out.push_back(text[i]); ++i; continue; }
        // Decode one UTF-8 sequence (strict-ish: overlongs/invalid fall to '?' ).
        uint32_t cp = 0;
        size_t len = 0;
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()
            && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
            cp = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()
                   && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80
                   && (static_cast<unsigned char>(text[i + 2]) & 0xC0) == 0x80) {
            cp = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6
                 | (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()
                   && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80
                   && (static_cast<unsigned char>(text[i + 2]) & 0xC0) == 0x80
                   && (static_cast<unsigned char>(text[i + 3]) & 0xC0) == 0x80) {
            cp = (c & 0x07u) << 18 | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12
                 | (static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6
                 | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            len = 4;
        } else {
            out.push_back('?');  // invalid lead/continuation byte: one gap-char, not three
            ++i;
            continue;
        }
        const char mapped = fold(cp);
        if (mapped == '\1') {
            // Not in the fold table (accented letters etc.): substitute the
            // atlas's own placeholder. One visible '?' beat three blank gaps.
            out.push_back('?');
        } else if (mapped != '\0') {
            out.push_back(mapped);
        }
        if (cp == 0x2026) out.append("..");   // ellipsis: '.' + '..' = "..."
        i += len;
    }
    return out;
}

// Uniform per-character estimate — the builtin bitmap font's only option
// (monospace by construction), and the fallback whenever style.fontId
// names a real font the system can't actually resolve right now.
float UniformAdvance(const TextStyle& style, const Font& f) {
    return f.cellWidth > 0 ? style.size * static_cast<float>(f.cellWidth + 1) /
                                 static_cast<float>(f.cellHeight)
                           : style.size;
}
// Real per-character advances for `text` when style.fontId names a font
// the system can actually resolve; empty otherwise (caller falls back to
// UniformAdvance) — the SAME "try the real font, degrade to builtin"
// contract BuildSystemAtlas's own callers (SceneBuilder) already follow,
// so wrapping decisions and what actually gets drawn always agree.
std::vector<float> RealAdvances(const std::string& text, const TextStyle& style,
                                const FontManager& fonts) {
    if (style.fontId.empty() || style.fontId == "builtin") return {};
    if (!fonts.HasSystemFont(style.fontId)) return {};
    return fonts.SystemCharAdvances(style.fontId, style.size, style.bold, style.italic, text);
}
} // namespace

float TextLayout::MeasureLine(const std::string& text, const TextStyle& style,
                              const FontManager& fonts) {
    // Same fold as Measure/SplitLines — callers of this one-off measure must
    // agree with the folded line text the atlas draw loop actually renders.
    const std::string& folded = FoldToAtlasAscii(text);
    if (folded.empty()) return 0.0f;
    if (auto real = RealAdvances(folded, style, fonts); !real.empty()) {
        float w = 0.0f;
        for (float a : real) w += a;
        return w + static_cast<float>(folded.size() - 1) * style.letterSpacing;
    }
    Font f = fonts.Resolve(style.fontId);
    const float advance = UniformAdvance(style, f);
    return static_cast<float>(folded.size()) * advance +
           static_cast<float>(folded.size() - 1) * style.letterSpacing;
}

TextLayoutResult TextLayout::Measure(const std::string& text, const TextStyle& style,
                                     const FontManager& fonts) {
    TextLayoutResult result;
    // SplitLines folds to atlas ASCII before splitting (see its comment): the
    // RealAdvances/word-wrap/width math below indexes per-BYTE advances of the
    // line text, and RenderEngine::DrawTextObject later walks the same line
    // text byte-wise against an ASCII-only atlas — every stage needs exactly
    // the folded string.
    auto raw = SplitLines(text, style.rtl);
    Font f = fonts.Resolve(style.fontId);
    const float lineHeight = style.size + style.lineSpacing;
    const float advance = UniformAdvance(style, f);
    // REAL vertical ink extent when the style names a resolvable system
    // font: ascent+descent, the visible height of ONE line of glyphs. The
    // per-line STEP below stays size+lineSpacing (DrawTextObject advances
    // lines by exactly that, and SceneBuilder now derives lineSpacing from
    // the font's own natural line height — the font dependence lives there).
    // Measuring the last line as a FULL step over-estimates by
    // (step − ink): lineGap plus the space below the last baseline — a
    // number that swings per font (negative for Segoe UI, ~15% of the em
    // for many serifs) — which is why the fit solver accepted sizes that
    // painted past the box only with certain fonts. Builtin font / non-
    // Windows / unresolved family: ink == lineHeight and every total is
    // byte-for-byte the legacy n·lineHeight.
    FontManager::LineMetrics fm{};
    const bool haveReal = fonts.SystemLineMetrics(style.fontId, style.size, style.bold,
                                                  style.italic, fm);
    const float ink = haveReal ? fm.inkHeight : lineHeight;
    result.totalHeight = 0.0f;
    result.lineStep = lineHeight;   // the step every line below advances by

    for (const auto& rawLine : raw) {
        // Real per-character advances for THIS line (empty = no real font
        // available; every width calc below falls back to the uniform
        // estimate in that case, same as before this method knew about
        // real fonts at all).
        const std::vector<float> real = RealAdvances(rawLine, style, fonts);
        const auto charAdvance = [&](size_t idx) {
            return (idx < real.size()) ? real[idx] : advance;
        };
        const auto widthOf = [&](size_t start, size_t end) {
            if (end <= start) return 0.0f;
            float w = 0.0f;
            for (size_t k = start; k < end; ++k) w += charAdvance(k);
            return w + static_cast<float>(end - start - 1) * style.letterSpacing;
        };

        if (!style.wrap || style.wrapWidth <= 0.0f || rawLine.empty()) {
            LineLayout ll;
            ll.text = rawLine;
            ll.width = widthOf(0, rawLine.size());
            result.totalWidth = std::max(result.totalWidth, ll.width);
            result.totalHeight += lineHeight;
            result.lines.push_back(std::move(ll));
            continue;
        }
        // Word wrap: break BETWEEN words at spaces, never mid-word. The old
        // loop wrapped per character, so a verse that didn't fit read as
        // scrambled text ("com / e back", "mayb / e"). A single word wider
        // than the wrap width is hard-broken per character as a last resort.
        std::string current;
        float currentWidth = 0.0f;
        auto flush = [&](const std::string& line, float width) {
            result.totalWidth = std::max(result.totalWidth, width);
            result.totalHeight += lineHeight;
            result.lines.push_back({line, width});
        };
        size_t i = 0;
        while (i < rawLine.size()) {
            // One word = a run of non-space characters.
            size_t j = i;
            while (j < rawLine.size() && rawLine[j] != ' ') ++j;
            const std::string word = rawLine.substr(i, j - i);
            const float wordWidth = widthOf(i, j);
            const float spaceWidth = charAdvance(i > 0 ? i - 1 : 0) + style.letterSpacing;
            if (!current.empty()) {
                if (currentWidth + spaceWidth + wordWidth > style.wrapWidth) {
                    flush(current, currentWidth);
                    current = word;
                    currentWidth = wordWidth;
                } else {
                    currentWidth += spaceWidth + wordWidth;
                    current += ' ';
                    current += word;
                }
            } else if (wordWidth > style.wrapWidth) {
                // Monster word alone on the line: hard-break per character.
                for (size_t k = i; k < j; ++k) {
                    const float chW = charAdvance(k);
                    if (currentWidth + chW > style.wrapWidth && !current.empty()) {
                        flush(current, currentWidth);
                        current.clear();
                        currentWidth = 0.0f;
                    }
                    current.push_back(rawLine[k]);
                    currentWidth += chW;
                }
            } else {
                current = word;
                currentWidth = wordWidth;
            }
            i = (j < rawLine.size()) ? j + 1 : j;   // skip the breaking space
        }
        if (!current.empty())
            flush(current, currentWidth);
    }
    // Baseline-grid correction: every line pushed a full `lineHeight` step,
    // but the LAST line contributes only its ink (ascent+descent for real
    // fonts, the same as one step for the builtin cell font) — exactly the
    // rectangle DrawTextObject paints: (n−1) baselines between lines plus
    // one line of glyphs. SWAP the last line's step for its ink: add the
    // ink, subtract the step it never spends (ink == lineHeight for the
    // builtin font, so the legacy n·lineHeight total is preserved there).
    if (!result.lines.empty())
        result.totalHeight += ink - lineHeight;
    return result;
}

} // namespace bps::rendering
