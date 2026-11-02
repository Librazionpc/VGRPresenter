#include "modules/rendering/TextEngine.hpp"

#include "core/logging/Logger.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTextLayout>
#include <QTextOption>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace {
QFont MakeQtFont(const std::string& family, float sizePx, bool bold, bool italic,
                 QFont::Weight fontWeight, bool underline = false, bool strikeout = false)
{
    QFont font(QString::fromUtf8(family.data(), static_cast<qsizetype>(family.size())));
    font.setPixelSize(bps::rendering::FontManager::QtPixelSize(sizePx));
    font.setWeight(bold ? QFont::Bold : fontWeight);
    font.setItalic(italic);
    font.setUnderline(underline);
    font.setStrikeOut(strikeout);
    return font;
}

QString QtGlyphFor(uint8_t codepoint)
{
    if (codepoint == 0x27)
        return QString(QChar(0x2019));
    return QString(QChar(static_cast<ushort>(codepoint)));
}
} // namespace

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

int FontManager::QtPixelSize(float sizePx) {
    if (!std::isfinite(sizePx) || sizePx < 1.0f)
        return 1;
    return static_cast<int>(sizePx);
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

RgbaImage FontManager::BuildSystemAtlas(const std::string& family, float sizePx, bool bold,
                                        bool italic, std::map<uint8_t, FontGlyph>& outGlyphs,
                                        QFont::Weight fontWeight, bool underline, bool strikeout) const {
    if (!HasSystemFont(family) || !std::isfinite(sizePx) || sizePx <= 0.0f)
        return {};
    const QFont font = MakeQtFont(family, sizePx, bold, italic, fontWeight, underline, strikeout);
    const QFontMetricsF metrics(font);
    std::map<uint8_t, QString> glyphs;
    std::map<uint8_t, float> advances;
    qreal leftExtent = 0.0;
    qreal rightExtent = 0.0;
    qreal bottomExtent = metrics.height();
    for (int cp = 32; cp <= 126; ++cp) {
        const QString glyph = QtGlyphFor(static_cast<uint8_t>(cp));
        const QRectF bounds = metrics.tightBoundingRect(glyph);
        const float adv = std::max(1.0f, static_cast<float>(metrics.horizontalAdvance(glyph)));
        glyphs[static_cast<uint8_t>(cp)] = glyph;
        advances[static_cast<uint8_t>(cp)] = adv;
        leftExtent = std::min(leftExtent, bounds.left());
        rightExtent = std::max({rightExtent, static_cast<qreal>(adv), bounds.right()});
        bottomExtent = std::max(bottomExtent, metrics.ascent() + bounds.bottom());
    }
    constexpr int kCellPad = 3;
    const int penX = kCellPad - static_cast<int>(std::floor(leftExtent));
    const int cellPx = static_cast<int>(std::ceil(rightExtent - leftExtent)) + kCellPad * 2;
    const int cellPy = static_cast<int>(std::ceil(bottomExtent)) + kCellPad * 2;
    const qreal baselineY = kCellPad + metrics.ascent();
    const int cols = 16;
    constexpr int rows = 6;
    constexpr int cellGap = 2;
    QImage sheet(cols * (cellPx + cellGap), rows * (cellPy + cellGap),
                 QImage::Format_ARGB32);
    if (sheet.isNull())
        return {};
    sheet.fill(Qt::transparent);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::white);

    int cell = 0;
    for (int cp = 32; cp <= 126 && cell < cols * rows; ++cp, ++cell) {
        const int cx = (cell % cols) * (cellPx + cellGap);
        const int cy = (cell / cols) * (cellPy + cellGap);
        painter.drawText(QPointF(cx + penX, cy + baselineY),
                         glyphs[static_cast<uint8_t>(cp)]);
        FontGlyph g;
        g.codepoint = static_cast<uint8_t>(cp);
        g.x = cx;
        g.y = cy;
        g.width = cellPx;
        g.height = cellPy;
        g.bearingX = -penX;
        g.bearingY = -kCellPad;
        g.advance = advances[static_cast<uint8_t>(cp)];
        outGlyphs[static_cast<uint8_t>(cp)] = g;
    }

    // Convert Qt's ARGB pixels to the engine's 0xAABBGGRR texture format.
    painter.end();
    RgbaImage atlas;
    atlas.width = sheet.width();
    atlas.height = sheet.height();
    atlas.pixels.resize(static_cast<size_t>(atlas.width) * atlas.height);
    for (int y = 0; y < atlas.height; ++y) {
        for (int x = 0; x < atlas.width; ++x) {
            const QRgb pixel = sheet.pixel(x, y);
            atlas.pixels[static_cast<size_t>(y) * atlas.width + x] =
                (static_cast<uint32_t>(qAlpha(pixel)) << 24)
                | (static_cast<uint32_t>(qBlue(pixel)) << 16)
                | (static_cast<uint32_t>(qGreen(pixel)) << 8)
                | static_cast<uint32_t>(qRed(pixel));
        }
    }
    return atlas;
}
bool FontManager::HasSystemFont(const std::string& family) const {
    if (!QGuiApplication::instance() || family.empty())
        return false;
    const QString requested = QString::fromUtf8(family.data(), static_cast<qsizetype>(family.size()));
    const auto families = QFontDatabase::families();
    return std::any_of(families.begin(), families.end(), [&requested](const QString& installed) {
        return installed.compare(requested, Qt::CaseInsensitive) == 0;
    });
}

float FontManager::SystemCharAdvance(const std::string& family, float sizePx, bool bold,
                                     bool italic, uint8_t codepoint, QFont::Weight fontWeight) const {
    const auto advances = SystemCharAdvances(
        family, sizePx, bold, italic, std::string(1, static_cast<char>(codepoint)), fontWeight);
    return advances.empty() ? 0.0f : advances.front();
}

std::vector<float> FontManager::SystemCharAdvances(const std::string& family, float sizePx,
                                                    bool bold, bool italic,
                                                    const std::string& text,
                                                    QFont::Weight fontWeight) const {
    if (!HasSystemFont(family) || !std::isfinite(sizePx) || sizePx <= 0.0f)
        return {};
    const QFont font = MakeQtFont(family, sizePx, bold, italic, fontWeight);
    const QFontMetricsF metrics(font);
    std::vector<float> out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        const QString glyph = QtGlyphFor(c);
        out.push_back(std::max(1.0f, static_cast<float>(metrics.horizontalAdvance(glyph))));
    }
    return out;
}

bool FontManager::SystemLineMetrics(const std::string& family, float sizePx, bool bold,
                                    bool italic, LineMetrics& out,
                                    QFont::Weight fontWeight) const {
    if (!HasSystemFont(family) || !QGuiApplication::instance()
        || !std::isfinite(sizePx) || sizePx <= 0.0f)
        return false;

    const QFont font = MakeQtFont(family, sizePx, bold, italic, fontWeight);
    const QFontMetricsF metrics(font);
    const qreal height = metrics.height();
    const qreal lineSpacing = metrics.lineSpacing();
    if (!(height > 0.0) || !(lineSpacing > 0.0))
        return false;

    out.baselineStep = static_cast<float>(lineSpacing);
    out.inkHeight = static_cast<float>(height);
    return true;
}

std::string FontManager::SubstituteFamily(std::string_view family) const {
    const std::string name(family);
    // "" and "builtin" are not missing fonts — they are the styles that never
    // named one, and the builtin bitmap font is exactly what they asked for.
    if (name.empty() || name == "builtin") return {};
    {
        std::lock_guard<std::mutex> lock(substituteMutex_);
        const auto it = substitutes_.find(name);
        if (it != substitutes_.end()) return it->second;
    }
    // Font resolution uses the same Qt font database as the previews. Logging
    // stays outside the cache lock.
    std::string resolved = name;
    if (!HasSystemFont(name)) {
        resolved = SystemUiFamily();
        Logger::Instance().Warning(
            std::format("font family '{}' is not installed — drawing it with '{}' instead "
                        "(the builtin 5x7 bitmap it used to fall back to paints pixel blocks "
                        "at output sizes)",
                        name, resolved.empty() ? "the builtin bitmap font" : resolved),
            "TextEngine");
    }
    {
        std::lock_guard<std::mutex> lock(substituteMutex_);
        substitutes_[name] = resolved;
    }
    return resolved;
}

std::string FontManager::SystemUiFamily() const {
    {
        std::lock_guard<std::mutex> lock(substituteMutex_);
        if (systemUiFamilyResolved_) return systemUiFamily_;
    }
    // Asked OUTSIDE the cache lock: it is a system query (and a font-set probe),
    // not map work.
    std::string family;
    if (QGuiApplication::instance())
        family = QGuiApplication::font().family().toUtf8().toStdString();
    if (!family.empty() && !HasSystemFont(family)) family.clear();
    {
        std::lock_guard<std::mutex> lock(substituteMutex_);
        systemUiFamily_ = family;
        systemUiFamilyResolved_ = true;
    }
    return family;
}

std::string FontManager::DefaultFamily() const {
    const std::string preferred = SystemUiFamily();
    return preferred.empty() ? "builtin" : preferred;
}

// ---------------------------------------------------------------------------
// TextLayout
// ---------------------------------------------------------------------------

namespace {
// Defined below (with the other layout helpers); SplitLines is the fold's
// first call site, so it needs the name visible up here.
std::string FoldToAtlasAscii(const std::string& text);
} // namespace

std::vector<std::string> TextLayout::SplitLines(const std::string& text, bool rtl) {
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

float UniformAdvance(const TextStyle& style, const Font& f) {
    return f.cellWidth > 0 ? style.size * static_cast<float>(f.cellWidth + 1) /
                                 static_cast<float>(f.cellHeight)
                           : style.size;
}
std::vector<float> RealAdvances(const std::string& text, const TextStyle& style,
                                const FontManager& fonts) {
    if (style.fontId.empty() || style.fontId == "builtin") return {};
    const std::string family = fonts.SubstituteFamily(style.fontId);
    if (family.empty() || !fonts.HasSystemFont(family)) return {};
    return fonts.SystemCharAdvances(family, style.size, style.bold, style.italic, text,
                                    style.fontWeight);
}

bool MeasureWithQtLayout(const std::string& source, const TextStyle& style,
                         const FontManager& fonts, TextLayoutResult& result) {
    const std::string family = fonts.SubstituteFamily(style.fontId);
    if (family.empty() || !fonts.HasSystemFont(family))
        return false;

    QFont font(QString::fromUtf8(family.data(), static_cast<qsizetype>(family.size())));
    font.setPixelSize(FontManager::QtPixelSize(style.size));
    font.setWeight(style.bold ? QFont::Bold : style.fontWeight);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    font.setStrikeOut(style.strikeout);
    if (style.letterSpacing != 0.0f)
        font.setLetterSpacing(QFont::AbsoluteSpacing, style.letterSpacing);
    if (style.wordSpacing != 0.0f)
        font.setWordSpacing(style.wordSpacing);

    FontManager::LineMetrics metrics{};
    if (!fonts.SystemLineMetrics(family, style.size, style.bold, style.italic, metrics,
                                 style.fontWeight))
        return false;

    const float step = style.size + style.lineSpacing + style.lineGap;
    const float lineHeight = step > 0.0f ? step : style.size;
    result.lineStep = lineHeight;
    result.lineBox = metrics.inkHeight;
    // THE SHARED BREAKS, when this text is the one they were cut from: no wrapping here at
    // all. Each line is the span between two of the table's starts; a line that ends at a
    // paragraph break (or the text's end) is a paragraph's last line and is not justified.
    if (style.forcedBreaks && !style.forcedBreaks->starts.empty()
            && style.forcedBreaks->textHash == TextLayout::TextHash(source)) {
        const QString whole = QString::fromUtf8(source.data(),
                                                static_cast<qsizetype>(source.size()));
        const std::vector<int>& starts = style.forcedBreaks->starts;
        bool consistent = true;
        for (size_t i = 0; i < starts.size() && consistent; ++i) {
            const int from = starts[i];
            const int to = i + 1 < starts.size() ? starts[i + 1] : static_cast<int>(whole.size());
            consistent = from >= 0 && from <= to && to <= whole.size();
        }
        if (consistent) {
            const QFontMetricsF fontMetrics(font);
            const auto foldedLine = [](const QString& text) {
                return FoldToAtlasAscii(text.toUtf8().toStdString());
            };
            for (size_t i = 0; i < starts.size(); ++i) {
                const int from = starts[i];
                const int to = i + 1 < starts.size() ? starts[i + 1]
                                                     : static_cast<int>(whole.size());
                QString line = whole.mid(from, to - from);
                bool paragraphEnd = (i + 1 == starts.size());
                if (line.endsWith(QLatin1Char('\n'))) {
                    line.chop(1);
                    paragraphEnd = true;
                }
                while (line.endsWith(QLatin1Char(' ')))
                    line.chop(1);   // the break's own space is not part of the line's ink
                const float width = static_cast<float>(fontMetrics.horizontalAdvance(line));
                result.lines.push_back({foldedLine(line), width, paragraphEnd});
                result.totalWidth = std::max(result.totalWidth, width);
                result.totalHeight += lineHeight;
            }
            if (!result.lines.empty())
                result.totalHeight += result.lineBox - lineHeight;
            return true;
        }
    }

    QStringList paragraphs = QString::fromUtf8(source.data(),
                                               static_cast<qsizetype>(source.size()))
                                 .split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (style.rtl)
        std::reverse(paragraphs.begin(), paragraphs.end());

    for (const QString& layoutText : paragraphs) {
        const auto foldedLine = [](const QString& line) {
            const std::string utf8 = line.toUtf8().toStdString();
            return FoldToAtlasAscii(utf8);
        };
        if (layoutText.isEmpty()) {
            result.lines.push_back({{}, 0.0f, true});
            result.totalHeight += lineHeight;
            continue;
        }

        if (!style.wrap || style.wrapWidth <= 0.0f) {
            const float width =
                static_cast<float>(QFontMetricsF(font).horizontalAdvance(layoutText));
            result.lines.push_back({foldedLine(layoutText), width, true});
            result.totalWidth = std::max(result.totalWidth, width);
            result.totalHeight += lineHeight;
            continue;
        }

        QTextLayout layout(layoutText, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setTextDirection(style.rtl ? Qt::RightToLeft : Qt::LeftToRight);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(style.wrapWidth);
            const int start = line.textStart();
            const int length = line.textLength();
            const float width = static_cast<float>(line.naturalTextWidth());
            result.lines.push_back({
                foldedLine(layoutText.mid(start, length)),
                width, start + length >= layoutText.size()
            });
            result.totalWidth = std::max(result.totalWidth, width);
            result.totalHeight += lineHeight;
        }
        layout.endLayout();
    }

    if (!result.lines.empty())
        result.totalHeight += result.lineBox - lineHeight;
    return true;
}
} // namespace

float TextLayout::MeasureLine(const std::string& text, const TextStyle& style,
                              const FontManager& fonts) {
    // Same fold as Measure/SplitLines — callers of this one-off measure must
    // agree with the folded line text the atlas draw loop actually renders.
    const std::string& folded = FoldToAtlasAscii(text);
    if (folded.empty()) return 0.0f;
    float wordExtra = 0.0f;
    for (char c : folded)
        if (c == ' ') wordExtra += style.wordSpacing;
    if (auto real = RealAdvances(folded, style, fonts); !real.empty()) {
        float w = 0.0f;
        for (float a : real) w += a;
        return w + static_cast<float>(folded.size() - 1) * style.letterSpacing + wordExtra;
    }
    Font f = fonts.Resolve(style.fontId);
    const float advance = UniformAdvance(style, f);
    return static_cast<float>(folded.size()) * advance +
           static_cast<float>(folded.size() - 1) * style.letterSpacing + wordExtra;
}

TextLayoutResult TextLayout::Measure(const std::string& text, const TextStyle& style,
                                     const FontManager& fonts) {
    TextLayoutResult result;
    auto raw = SplitLines(text, style.rtl);
    if (MeasureWithQtLayout(text, style, fonts, result))
        return result;

    Font f = fonts.Resolve(style.fontId);
    const float step = style.size + style.lineSpacing + style.lineGap;
    const float lineHeight = step > 0.0f ? step : style.size;
    const float advance = UniformAdvance(style, f);
    FontManager::LineMetrics fm{};
    const std::string family = fonts.SubstituteFamily(style.fontId);
    const bool haveReal = !family.empty() && fonts.HasSystemFont(family)
                          && fonts.SystemLineMetrics(family, style.size, style.bold,
                                                     style.italic, fm, style.fontWeight);
    const float ink = haveReal ? fm.inkHeight : (style.size + style.lineSpacing);
    result.totalHeight = 0.0f;
    result.lineStep = lineHeight;   // the step every line below advances by
    // One line BOX, for the per-line background band (see TextLayoutResult):
    // the same ink extent the last line's measurement uses.
    result.lineBox = ink;

    for (const auto& rawLine : raw) {
        const std::vector<float> real = RealAdvances(rawLine, style, fonts);
        const auto charAdvance = [&](size_t idx) {
            return (idx < real.size()) ? real[idx] : advance;
        };
        const auto widthOf = [&](size_t start, size_t end) {
            if (end <= start) return 0.0f;
            float w = 0.0f;
            size_t spaces = 0;
            for (size_t k = start; k < end; ++k) {
                w += charAdvance(k);
                if (rawLine[k] == ' ') ++spaces;
            }
            // Same two additive terms DrawTextObject applies per glyph: one
            // letterSpacing per gap, one wordSpacing per space.
            return w + static_cast<float>(end - start - 1) * style.letterSpacing
                     + static_cast<float>(spaces) * style.wordSpacing;
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
        std::string current;
        float currentWidth = 0.0f;
        auto flush = [&](const std::string& line, float width, bool paragraphEnd) {
            result.totalWidth = std::max(result.totalWidth, width);
            result.totalHeight += lineHeight;
            result.lines.push_back({line, width, paragraphEnd});
        };
        size_t i = 0;
        while (i < rawLine.size()) {
            // One word = a run of non-space characters.
            size_t j = i;
            while (j < rawLine.size() && rawLine[j] != ' ') ++j;
            const std::string word = rawLine.substr(i, j - i);
            const float wordWidth = widthOf(i, j);
            const float spaceWidth = charAdvance(i > 0 ? i - 1 : 0) + style.letterSpacing
                                   + style.wordSpacing;
            if (!current.empty()) {
                if (currentWidth + spaceWidth + wordWidth > style.wrapWidth) {
                    flush(current, currentWidth, false);
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
                        flush(current, currentWidth, false);
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
            flush(current, currentWidth, true);
    }
    if (!result.lines.empty())
        result.totalHeight += ink - lineHeight;
    return result;
}

uint64_t TextLayout::TextHash(const std::string& text) {
    uint64_t hash = 14695981039346656037ull;   // FNV-1a, 64 bit
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<int> TextLayout::FlowCharacterStarts(const std::string& text,
                                                 const TextStyle& style,
                                                 const FontManager& fonts) {
    // The shared table IS the answer when this text is the one it was cut from.
    if (style.forcedBreaks && !style.forcedBreaks->starts.empty()
            && style.forcedBreaks->textHash == TextHash(text))
        return style.forcedBreaks->starts;
    const TextLayoutResult layout = Measure(text, style, fonts);
    const std::string foldedText = FoldToAtlasAscii(text);
    // The engine lays out folded bytes, while the shared scroll position is in
    // Qt's UTF-16 units. Keep a reverse map so both sides address the same word.
    std::vector<int> sourceCharAtFoldedOffset(foldedText.size() + 1, 0);

    const bool asciiOnly = std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch < 0x80;
    });
    if (asciiOnly) {
        for (size_t i = 0; i <= text.size(); ++i)
            sourceCharAtFoldedOffset[i] = static_cast<int>(i);
    } else {
        size_t sourceByte = 0;
        size_t foldedByte = 0;
        int sourceChar = 0;
        while (sourceByte < text.size()) {
            const unsigned char lead = static_cast<unsigned char>(text[sourceByte]);
            size_t sequenceLength = 1;
            if ((lead & 0xE0) == 0xC0 && sourceByte + 1 < text.size()
                && (static_cast<unsigned char>(text[sourceByte + 1]) & 0xC0) == 0x80) {
                sequenceLength = 2;
            } else if ((lead & 0xF0) == 0xE0 && sourceByte + 2 < text.size()
                       && (static_cast<unsigned char>(text[sourceByte + 1]) & 0xC0) == 0x80
                       && (static_cast<unsigned char>(text[sourceByte + 2]) & 0xC0) == 0x80) {
                sequenceLength = 3;
            } else if ((lead & 0xF8) == 0xF0 && sourceByte + 3 < text.size()
                       && (static_cast<unsigned char>(text[sourceByte + 1]) & 0xC0) == 0x80
                       && (static_cast<unsigned char>(text[sourceByte + 2]) & 0xC0) == 0x80
                       && (static_cast<unsigned char>(text[sourceByte + 3]) & 0xC0) == 0x80) {
                sequenceLength = 4;
            }

            const std::string foldedUnit =
                FoldToAtlasAscii(text.substr(sourceByte, sequenceLength));
            for (size_t i = 0; i < foldedUnit.size(); ++i)
                sourceCharAtFoldedOffset[foldedByte + i] = sourceChar;
            foldedByte += foldedUnit.size();
            // A four-byte UTF-8 code point occupies a UTF-16 surrogate pair.
            sourceChar += sequenceLength == 4 ? 2 : 1;
            sourceByte += sequenceLength;
            sourceCharAtFoldedOffset[foldedByte] = sourceChar;
        }
    }

    std::vector<int> starts;
    starts.reserve(layout.lines.size());
    size_t cursor = 0;
    size_t previousStart = 0;
    for (const auto& line : layout.lines) {
        size_t start = previousStart;
        if (!line.text.empty()) {
            const size_t found = foldedText.find(line.text, cursor);
            if (found != std::string::npos) {
                start = found;
                cursor = found + line.text.size();
            }
        }
        start = std::min(start, foldedText.size());
        starts.push_back(sourceCharAtFoldedOffset[start]);
        previousStart = start;
    }
    return starts;
}
} // namespace bps::rendering