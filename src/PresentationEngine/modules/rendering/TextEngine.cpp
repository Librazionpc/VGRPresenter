#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <cstring>

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
    return MakeGdiplusFont(family, 24.0f, false, false) != nullptr;
}

float FontManager::SystemCharAdvance(const std::string& family, float sizePx, bool bold,
                                     bool italic, uint8_t codepoint) const {
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);
    if (!font) return 0.0f;
    // A 1x1 bitmap's Graphics is a real, valid measuring context (GDI+
    // measurement doesn't rasterize into it — a device context is just
    // where the DPI/transform come from) without needing a window or the
    // full atlas bitmap this method is called ahead of building.
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&probe);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const wchar_t ch = static_cast<wchar_t>(codepoint);
    Gdiplus::RectF bounds;
    g.MeasureString(&ch, 1, font.get(), Gdiplus::PointF(0, 0), &TightFormat(), &bounds);
    return bounds.Width;
}

std::vector<float> FontManager::SystemCharAdvances(const std::string& family, float sizePx,
                                                    bool bold, bool italic,
                                                    const std::string& text) const {
    auto font = MakeGdiplusFont(family, sizePx, bold, italic);
    if (!font) return {};
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&probe);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    std::vector<float> out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        const wchar_t ch = static_cast<wchar_t>(c);
        Gdiplus::RectF bounds;
        g.MeasureString(&ch, 1, font.get(), Gdiplus::PointF(0, 0), &TightFormat(), &bounds);
        out.push_back(std::max(1.0f, bounds.Width));
    }
    return out;
}

RgbaImage FontManager::BuildSystemAtlas(const std::string& family, float sizePx, bool bold,
                                        bool italic, std::map<uint8_t, FontGlyph>& outGlyphs) const {
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
    for (int cp = 32; cp <= 126; ++cp) {
        const wchar_t ch = static_cast<wchar_t>(cp);
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

    Gdiplus::Bitmap sheet(cols * (scW + 1), rows * (scH + 1), PixelFormat32bppARGB);
    Gdiplus::Graphics draw(&sheet);
    draw.Clear(Gdiplus::Color(0, 0, 0, 0));
    draw.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    draw.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));

    int cell = 0;
    for (int cp = 32; cp <= 126 && cell < cols * rows; ++cp, ++cell) {
        const int cx = (cell % cols) * (scW + 1);
        const int cy = (cell / cols) * (scH + 1);
        const wchar_t ch = static_cast<wchar_t>(cp);
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
RgbaImage FontManager::BuildSystemAtlas(const std::string&, float, bool, bool,
                                        std::map<uint8_t, FontGlyph>&) const { return {}; }
#endif

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

namespace {
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
    if (text.empty()) return 0.0f;
    if (auto real = RealAdvances(text, style, fonts); !real.empty()) {
        float w = 0.0f;
        for (float a : real) w += a;
        return w + static_cast<float>(text.size() - 1) * style.letterSpacing;
    }
    Font f = fonts.Resolve(style.fontId);
    const float advance = UniformAdvance(style, f);
    return static_cast<float>(text.size()) * advance +
           static_cast<float>(text.size() - 1) * style.letterSpacing;
}

TextLayoutResult TextLayout::Measure(const std::string& text, const TextStyle& style,
                                     const FontManager& fonts) {
    TextLayoutResult result;
    auto raw = SplitLines(text, style.rtl);
    Font f = fonts.Resolve(style.fontId);
    const float lineHeight = style.size + style.lineSpacing;
    const float advance = UniformAdvance(style, f);

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
    return result;
}

} // namespace bps::rendering
