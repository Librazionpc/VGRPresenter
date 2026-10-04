#include "modules/library/TheTableLibrary.hpp"

#include "core/config/Json.hpp"
#include "core/logging/Logger.hpp"
#include "modules/content/AssetCompressor.hpp"
#include "modules/search/SearchEngine.hpp"
#include "modules/search/TextMatching.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bps::library {

namespace {
namespace textmatch = bps::search::textmatch;

constexpr const char* kModule = "TheTableLibrary";
constexpr ErrorCode kTableErr = 2650;   // the Library band's Table slot
constexpr const char* kConvertedVersion = "v1";   // bump when the conversion changes: text kept by an older one is then made again

using J = json::Value;

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

using textmatch::Lower;   // shared with the Search Engine (see TextMatching.hpp)
using textmatch::ContainsWord;
using textmatch::ContainsPhrase;
using textmatch::StartsWord;

std::string Collapse(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastSpace = true;   // leading whitespace is dropped
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!lastSpace) { out.push_back(' '); lastSpace = true; }
        } else {
            out.push_back(c);
            lastSpace = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// "53_0217_Only_Believe.pdf" -> { year "1953", title "0217 Only Believe" }.
// The filename encodes yy_mmdd-style prefixes (53_0217); a 19xx/20xx folder
// name is authoritative for the year, the leading number becomes the title.
struct SermonName {
    std::string year;
    std::string title;
};

SermonName NameFromFileName(const std::string& fileName) {
    SermonName n;
    std::string base = fileName;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    base = Trim(base);

    // Year: a 4-digit folder in the path ("downloads/1953/53_0217_X.pdf"). The one nearest the file wins, so a folder far above the
    // sermons ("D:/2024/Sermons/...") cannot override it. Scan path segments.
    std::string path = fileName;
    size_t seg = 0;
    while ((seg = path.find_first_not_of("/\\", seg)) != std::string::npos) {
        const size_t end = path.find_first_of("/\\", seg);
        const std::string part = path.substr(seg, end == std::string::npos ? end : end - seg);
        if (part.size() == 4 && std::all_of(part.begin(), part.end(), ::isdigit)) {
            n.year = part;   // keep scanning: a nearer folder replaces it
        }
        seg = end;
    }
    if (n.year.empty()) {
        const size_t us = base.find('_');
        if (us != std::string::npos && us == 2) {
            const std::string yy = base.substr(0, 2);
            if (std::all_of(yy.begin(), yy.end(), ::isdigit)) n.year = "19" + yy;
        }
    }

    // Title: strip a leading "53_0217_"-style code, keep the readable tail.
    std::string title = base;
    static const std::string digits = "0123456789";
    size_t t = 0;
    while (t < title.size() && (digits.find(title[t]) != std::string::npos || title[t] == '_'))
        ++t;
    title = title.substr(t);
    std::replace(title.begin(), title.end(), '_', ' ');
    n.title = Trim(Collapse(title));
    if (n.title.empty()) n.title = base;
    return n;
}

// The leading numeric code ("0217") kept for the chapter title. A leading
// 2-digit group ("53_") is the year prefix, not the code — skip it.
std::string CodeOf(const std::string& fileName) {
    std::string base = fileName;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t b = base.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t i = b;
    while (i < base.size() && std::isdigit(static_cast<unsigned char>(base[i]))) ++i;
    if (i - b == 2 && i < base.size() && base[i] == '_') {
        b = i + 1;                       // "53_" — the yy year prefix; the code follows
        i = b;
        while (i < base.size() && std::isdigit(static_cast<unsigned char>(base[i]))) ++i;
    }
    if (i > b && i < base.size() && base[i] == '_') return base.substr(b, i - b);
    return {};
}

// The date-code inside a stored TITLE ("0217 Only Believe" -> "0217"): the
// leading numeric run before the first space. Empty when the title opens with
// words (sermons imported without a code in their file name).
std::string CodeFromTitle(const std::string& title) {
    size_t sp = title.find(' ');
    std::string head = sp == std::string::npos ? title : title.substr(0, sp);
    if (!head.empty() && head.find_first_not_of("0123456789") == std::string::npos)
        return head;
    return {};
}

// A sermon's citation line, the way these sermons are cited: the year without
// its "19" prefix, a dash, the sermon's own date-code, another dash, the
// title with every non-alphanumeric character dropped, then the paragraph
// number — "1947" + "0412 Faith Is The Substance" + 3 becomes
//   47-0412 - Faith Is The Substance 3
// The stored parts (year / code / title) stay as imported; only the display
// cleans them up. An empty `code` keeps its slot out of the line; a zero
// `verse` drops the trailing paragraph number (the sermon-level citation).
std::string SermonCitation(const std::string& year, const std::string& code,
                           const std::string& title, int verse) {
    // The book name ("1947") loses a leading "19" when what is left is still
    // a plausible yy form (2 digits) — "1947" -> "47"; anything else as-is.
    std::string yy = year;
    if (yy.size() == 4 && yy.substr(0, 2) == "19") yy = yy.substr(2);
    std::string clean;
    clean.reserve(title.size());
    bool pendingSpace = false;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (pendingSpace && !clean.empty()) clean.push_back(' ');
            pendingSpace = false;
            clean.push_back(c);
        } else if (!clean.empty()) {
            pendingSpace = true;   // runs of punctuation/spaces become one space
        }
    }
    // The stored title opens with the code when there is one ("0217 Only
    // Believe"); the code stands on its own in the citation, so the leading
    // numeric run comes out of the title (kept when the title is ONLY a code).
    std::string display = clean;
    if (!code.empty()) {
        size_t d = 0;
        while (d < display.size() && std::isdigit(static_cast<unsigned char>(display[d]))) ++d;
        if (d > 0 && d < display.size() && display[d] == ' ') display = display.substr(d + 1);
    }
    if (display.empty()) display = clean;
    std::string out = yy;
    if (!code.empty()) out += "-" + code;
    if (!display.empty()) out += " - " + display;
    if (verse > 0) out += " " + std::to_string(verse);
    return out;
}

constexpr const char* kUnfiledYear = "Unfiled";

// Where a sermon file goes: its year book and its chapter title ("0217 Only Believe" - the numeric code before the title is kept so
// chapters sort naturally). ImportSermon and the folder import's duplicate check both ask this, so they cannot disagree about what
// "the same sermon" is.
struct Placement {
    std::string year;
    std::string title;
};

Placement PlacementOf(const std::string& fileName) {
    const SermonName parsed = NameFromFileName(fileName);
    Placement p;
    p.year = parsed.year.empty() ? std::string(kUnfiledYear) : parsed.year;
    const std::string code = CodeOf(fileName);
    p.title = (!code.empty() && !parsed.title.starts_with(code)) ? code + " " + parsed.title : parsed.title;
    return p;
}

// ---------------------------------------------------------------------------
// PDF text extraction (built-in, dependency-free): walks the objects, inflates
// FlateDecode content streams through the engine's zlib DeflateCompressor and
// collects the strings of text-showing operators (Tj, TJ, ', ").
// ---------------------------------------------------------------------------
std::string UnescapePdfString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c != '\\') { out.push_back(c); continue; }
        if (++i >= raw.size()) break;
        switch (raw[i]) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': case 'f': break;
            case '(': out.push_back('('); break;
            case ')': out.push_back(')'); break;
            case '\\': out.push_back('\\'); break;
            default:
                if (raw[i] >= '0' && raw[i] <= '7' && i + 2 < raw.size() &&
                    raw[i + 1] >= '0' && raw[i + 1] <= '7' && raw[i + 2] >= '0' && raw[i + 2] <= '7') {
                    out.push_back(static_cast<char>(
                        std::stoi(raw.substr(i, 3), nullptr, 8)));
                    i += 2;
                } else {
                    out.push_back(raw[i]);
                }
        }
    }
    return out;
}

// ---- PDF text, the way a reader gets it -------------------------------------------------------------------------------------------
// A page's text is not in its strings as they are: a font with an Identity-H encoding writes glyph numbers (often as hex, <0012003A>),
// and the font's /ToUnicode table says which character each number is. So: read the objects, find the pages in order, load the fonts
// each page uses (and their ToUnicode tables), then read the page's content stream with those fonts. Line breaks and paragraph breaks
// come from where the lines sit on the page.

bool LooksLikeTextOps(const std::string& s) {
    return s.find("Tj") != std::string::npos || s.find("TJ") != std::string::npos;
}

bool IsPdfSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0'; }
bool IsPdfDelim(char c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

void AppendCodepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// Windows-1252 (what a font without a ToUnicode table means by its one-byte codes) -> UTF-8.
std::string Cp1252ToUtf8(const std::string& bytes) {
    static const uint32_t high[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                       0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
    std::string out;
    for (unsigned char c : bytes) {
        if (c < 0x80) { if (c >= 0x20 || c == '\n' || c == '\t') out += static_cast<char>(c); }
        else AppendCodepoint(out, c < 0xA0 ? high[c - 0x80] : c);
    }
    return out;
}

// --- objects: "N G obj <<dict>> stream ... endstream endobj" ---
struct PdfObject {
    std::string dict;      // the text of the object (up to its stream)
    std::string stream;    // the stream, still filtered
    bool hasStream = false;
};
using PdfObjects = std::map<int, PdfObject>;

// The whole number that ends just before `end` (spaces skipped); false when there is none.
bool NumberBefore(const std::string& d, size_t end, int& value, size_t& start) {
    size_t e = end;
    while (e > 0 && IsPdfSpace(d[e - 1])) --e;
    size_t s = e;
    while (s > 0 && std::isdigit(static_cast<unsigned char>(d[s - 1]))) --s;
    if (s == e || e - s > 9) return false;
    value = std::stoi(d.substr(s, e - s));
    start = s;
    return true;
}

std::string DecodeStream(const PdfObject& o);   // (defined below; ReadObjects needs it to unpack compressed object streams)
size_t FindKey(const std::string& dict, const std::string& key, size_t from = 0);   // (defined below too)

// "N" at a key ("/N 53"); `fallback` when the key is missing or not a plain number.
int IntAfter(const std::string& dict, const char* key, int fallback) {
    const size_t k = FindKey(dict, key);
    if (k == std::string::npos) return fallback;
    const char* p = dict.c_str() + k + std::strlen(key);
    char* end = nullptr;
    const long v = std::strtol(p, &end, 10);
    return end != p ? static_cast<int>(v) : fallback;
}

// Compressed object streams (PDF 1.5+, "/Type /ObjStm"): many of a PDF's smaller objects - font dictionaries and their ToUnicode
// tables among them - packed into one compressed stream instead of each appearing as its own "N 0 obj", which the scan above cannot
// see at all. Its own header (right after decompressing) is N pairs of "object-number offset-from-First"; each object's dictionary runs
// from its offset to the next one's (or the stream's end). None of them carries a stream of its own - the format does not allow it.
void ReadObjectStreams(PdfObjects& out) {
    std::vector<int> containers;
    for (const auto& [num, obj] : out)
        if (obj.hasStream && obj.dict.find("/ObjStm") != std::string::npos) containers.push_back(num);
    for (const int num : containers) {
        const std::string data = DecodeStream(out.at(num));
        if (data.empty()) continue;
        const std::string& dict = out.at(num).dict;
        const int n = IntAfter(dict, "/N", 0);
        const int first = IntAfter(dict, "/First", 0);
        if (n <= 0 || first <= 0 || static_cast<size_t>(first) > data.size()) continue;
        std::vector<std::pair<int, int>> header;   // (object number, offset)
        size_t p = 0;
        for (int i = 0; i < n; ++i) {
            while (p < data.size() && IsPdfSpace(data[p])) ++p;
            const size_t b1 = p;
            while (p < data.size() && std::isdigit(static_cast<unsigned char>(data[p]))) ++p;
            if (p == b1) break;
            const int objNum = std::stoi(data.substr(b1, p - b1));
            while (p < data.size() && IsPdfSpace(data[p])) ++p;
            const size_t b2 = p;
            while (p < data.size() && std::isdigit(static_cast<unsigned char>(data[p]))) ++p;
            if (p == b2) break;
            header.push_back({ objNum, std::stoi(data.substr(b2, p - b2)) });
        }
        for (size_t i = 0; i < header.size(); ++i) {
            const size_t from = static_cast<size_t>(first) + header[i].second;
            const size_t to = i + 1 < header.size() ? static_cast<size_t>(first) + header[i + 1].second : data.size();
            if (from >= data.size() || to < from) continue;
            // An object already found directly (an updated later revision) wins over the packed copy.
            if (out.count(header[i].first)) continue;
            PdfObject obj;
            obj.dict = data.substr(from, std::min(to, data.size()) - from);
            out[header[i].first] = std::move(obj);
        }
    }
}

PdfObjects ReadObjects(const std::string& d) {
    PdfObjects out;
    size_t pos = 0;
    while ((pos = d.find("obj", pos)) != std::string::npos) {
        const size_t at = pos;
        pos += 3;
        if (at == 0 || !IsPdfSpace(d[at - 1])) continue;              // "endobj" and other words that end in obj
        int gen = 0, num = 0;
        size_t genStart = 0, numStart = 0;
        if (!NumberBefore(d, at, gen, genStart) || !NumberBefore(d, genStart, num, numStart)) continue;
        const size_t bodyStart = at + 3;
        size_t endobj = d.find("endobj", bodyStart);
        if (endobj == std::string::npos) endobj = d.size();
        const std::string body = d.substr(bodyStart, endobj - bodyStart);
        pos = endobj + 6;

        PdfObject obj;
        size_t sp = body.find("stream");
        while (sp != std::string::npos) {                             // the "stream" keyword follows the dictionary's ">>"
            size_t k = sp;
            while (k > 0 && IsPdfSpace(body[k - 1])) --k;
            if (k >= 2 && body[k - 1] == '>' && body[k - 2] == '>') break;
            sp = body.find("stream", sp + 6);
        }
        if (sp != std::string::npos) {
            obj.dict = body.substr(0, sp);
            size_t from = sp + 6;
            if (from < body.size() && body[from] == '\r') ++from;
            if (from < body.size() && body[from] == '\n') ++from;
            size_t to = body.rfind("endstream");
            if (to == std::string::npos || to < from) to = body.size();
            obj.stream = body.substr(from, to - from);
            obj.hasStream = true;
        } else {
            obj.dict = body;
        }
        out[num] = std::move(obj);
    }
    ReadObjectStreams(out);
    return out;
}

std::string DecodeStream(const PdfObject& o) {
    if (!o.hasStream) return {};
    if (o.dict.find("FlateDecode") != std::string::npos) {
        content::DeflateCompressor deflate;
        auto out = deflate.Decompress(reinterpret_cast<const uint8_t*>(o.stream.data()), o.stream.size());
        if (!out.ok()) return {};
        return std::string(out.value().begin(), out.value().end());
    }
    if (o.dict.find("Filter") == std::string::npos) return o.stream;
    return {};                                                        // a filter we do not read (images, LZW...)
}

// --- dictionary text helpers ---
size_t FindKey(const std::string& dict, const std::string& key, size_t from) {
    size_t k = from;
    while ((k = dict.find(key, k)) != std::string::npos) {
        const size_t after = k + key.size();
        if (after >= dict.size() || !std::isalnum(static_cast<unsigned char>(dict[after]))) return k;   // "/Page" is not "/Pages"
        k = after;
    }
    return std::string::npos;
}

// Reads "N G R" at `i`; returns N (0 when it is not a reference) and moves `i` past it.
int ReadRef(const std::string& s, size_t& i) {
    size_t j = i;
    auto skip = [&] { while (j < s.size() && IsPdfSpace(s[j])) ++j; };
    auto number = [&](int& v) {
        skip();
        const size_t b = j;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == b || j - b > 9) return false;
        v = std::stoi(s.substr(b, j - b));
        return true;
    };
    int n = 0, g = 0;
    if (!number(n) || !number(g)) return 0;
    skip();
    if (j < s.size() && s[j] == 'R') { i = j + 1; return n; }
    return 0;
}

std::vector<int> RefsAfter(const std::string& dict, const std::string& key) {
    std::vector<int> out;
    const size_t k = FindKey(dict, key);
    if (k == std::string::npos) return out;
    size_t i = k + key.size();
    while (i < dict.size() && IsPdfSpace(dict[i])) ++i;
    if (i < dict.size() && dict[i] == '[') {
        ++i;
        while (i < dict.size() && dict[i] != ']') {
            const int r = ReadRef(dict, i);
            if (r) out.push_back(r);
            else ++i;
        }
    } else if (const int r = ReadRef(dict, i)) {
        out.push_back(r);
    }
    return out;
}

// The "<< ... >>" that starts at `from` (nested ones included).
std::string BalancedDict(const std::string& s, size_t from) {
    int depth = 0;
    for (size_t i = from; i + 1 < s.size(); ++i) {
        if (s[i] == '<' && s[i + 1] == '<') { ++depth; ++i; }
        else if (s[i] == '>' && s[i + 1] == '>') { --depth; ++i; if (depth == 0) return s.substr(from, i + 1 - from); }
    }
    return {};
}

// What a key holds when it holds a dictionary - inline, or an object it points to.
std::string DictAfter(const std::string& dict, const std::string& key, const PdfObjects& objs) {
    const size_t k = FindKey(dict, key);
    if (k == std::string::npos) return {};
    size_t i = k + key.size();
    while (i < dict.size() && IsPdfSpace(dict[i])) ++i;
    if (i + 1 < dict.size() && dict[i] == '<' && dict[i + 1] == '<') return BalancedDict(dict, i);
    if (const int r = ReadRef(dict, i)) {
        auto it = objs.find(r);
        if (it != objs.end()) return it->second.dict;
    }
    return {};
}

// --- fonts and their ToUnicode tables ---
struct PdfFont {
    bool twoByte = false;                                   // a Type0 (composite) font: its codes are glyph numbers
    int codeBytes = 0;                                      // from the ToUnicode table's code space (0 = not stated)
    bool hasMap = false;
    bool hasSpace = false;                                  // the table maps some code to a space: word spaces are real glyphs
    std::unordered_map<uint32_t, std::string> toUnicode;   // code -> UTF-8
    // Some PDFs, with no ToUnicode table at all, print correctly (the font's own glyphs are just not at their normal codes) but every
    // raw code is off by the same constant from the real character - see DetectShift. 0 means: no shift found, read the bytes as they are.
    int shift = 0;
};

std::string Utf16ToUtf8(const std::vector<uint32_t>& units) {
    std::string out;
    for (size_t k = 0; k < units.size(); ++k) {
        uint32_t u = units[k];
        if (u >= 0xD800 && u < 0xDC00 && k + 1 < units.size() && units[k + 1] >= 0xDC00 && units[k + 1] < 0xE000) {
            u = 0x10000 + ((u - 0xD800) << 10) + (units[k + 1] - 0xDC00);
            ++k;
        }
        AppendCodepoint(out, u);
    }
    return out;
}

std::vector<uint32_t> HexUnits(const std::string& hex) {
    std::vector<uint32_t> units;
    for (size_t i = 0; i + 4 <= hex.size(); i += 4) units.push_back(static_cast<uint32_t>(std::stoul(hex.substr(i, 4), nullptr, 16)));
    return units;
}

void ParseToUnicode(const std::string& cmap, PdfFont& font) {
    struct Tok { enum Kind { Hex, Word, Open, Close } kind; std::string text; };
    std::vector<Tok> toks;
    for (size_t i = 0; i < cmap.size();) {
        const char c = cmap[i];
        if (IsPdfSpace(c)) { ++i; continue; }
        if (c == '<' && i + 1 < cmap.size() && cmap[i + 1] != '<') {
            std::string hex;
            for (++i; i < cmap.size() && cmap[i] != '>'; ++i)
                if (std::isxdigit(static_cast<unsigned char>(cmap[i]))) hex += cmap[i];
            ++i;
            toks.push_back({ Tok::Hex, hex });
        } else if (c == '[') { toks.push_back({ Tok::Open, "" }); ++i; }
        else if (c == ']') { toks.push_back({ Tok::Close, "" }); ++i; }
        else if (c == '%') { while (i < cmap.size() && cmap[i] != '\n') ++i; }
        else if (c == '<' || c == '>' || c == '(' || c == ')' || c == '/') {   // a dictionary, string or name: not part of the mapping
            if (c == '/') { ++i; while (i < cmap.size() && !IsPdfSpace(cmap[i]) && !IsPdfDelim(cmap[i])) ++i; }
            else ++i;
        } else {
            const size_t b = i;
            while (i < cmap.size() && !IsPdfSpace(cmap[i]) && !IsPdfDelim(cmap[i])) ++i;
            toks.push_back({ Tok::Word, cmap.substr(b, i - b) });
        }
    }
    auto code = [](const std::string& hex) { return static_cast<uint32_t>(std::stoul(hex.empty() ? "0" : hex.substr(0, 8), nullptr, 16)); };
    enum { None, Space, Char, Range } mode = None;
    for (size_t t = 0; t < toks.size(); ++t) {
        const Tok& tk = toks[t];
        if (tk.kind == Tok::Word) {
            if (tk.text == "begincodespacerange") mode = Space;
            else if (tk.text == "beginbfchar") mode = Char;
            else if (tk.text == "beginbfrange") mode = Range;
            else if (tk.text.rfind("end", 0) == 0) mode = None;
            continue;
        }
        if (tk.kind != Tok::Hex) continue;
        if (mode == Space) {
            font.codeBytes = std::max(font.codeBytes, static_cast<int>(tk.text.size() / 2));
            ++t;   // (the range's other end)
        } else if (mode == Char && t + 1 < toks.size() && toks[t + 1].kind == Tok::Hex) {
            font.toUnicode[code(tk.text)] = Utf16ToUtf8(HexUnits(toks[t + 1].text));
            ++t;
        } else if (mode == Range && t + 2 < toks.size() && toks[t + 1].kind == Tok::Hex) {
            const uint32_t lo = code(tk.text), hi = code(toks[t + 1].text);
            if (toks[t + 2].kind == Tok::Open) {              // [<d1> <d2> ...]: one destination for each code
                size_t k = t + 3;
                uint32_t c = lo;
                for (; k < toks.size() && toks[k].kind == Tok::Hex; ++k, ++c)
                    if (c <= hi) font.toUnicode[c] = Utf16ToUtf8(HexUnits(toks[k].text));
                t = k;                                       // (on the closing bracket)
            } else if (toks[t + 2].kind == Tok::Hex && hi >= lo && hi - lo < 65536) {
                std::vector<uint32_t> units = HexUnits(toks[t + 2].text);
                if (!units.empty())
                    for (uint32_t c = lo; c <= hi; ++c) {
                        font.toUnicode[c] = Utf16ToUtf8(units);
                        ++units.back();
                    }
                t += 2;
            }
        }
    }
    font.hasMap = !font.toUnicode.empty();
    for (const auto& kv : font.toUnicode)
        if (kv.second == " ") { font.hasSpace = true; break; }
}

std::string DecodeShown(const PdfFont* font, const std::string& bytes) {
    if (!font) return Cp1252ToUtf8(bytes);
    if (font->hasMap) {
        const int width = font->codeBytes > 0 ? font->codeBytes : (font->twoByte ? 2 : 1);
        std::string out;
        for (size_t i = 0; i + width <= bytes.size(); i += width) {
            uint32_t c = 0;
            for (int k = 0; k < width; ++k) c = (c << 8) | static_cast<unsigned char>(bytes[i + k]);
            auto it = font->toUnicode.find(c);
            if (it != font->toUnicode.end()) out += it->second;
            else if (width == 1 && c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
        }
        return out;
    }
    if (font->twoByte) return {};                            // glyph numbers with no table: nothing to read
    if (font->shift == 0) return Cp1252ToUtf8(bytes);
    std::string shown(bytes.size(), '\0');
    for (size_t i = 0; i < bytes.size(); ++i)
        shown[i] = static_cast<char>((static_cast<unsigned char>(bytes[i]) + font->shift + 256) % 256);
    return Cp1252ToUtf8(shown);
}

// --- a page's content stream ---
struct PdfLine {
    double y = 0;
    std::string text;
};
using FontTable = std::unordered_map<std::string, PdfFont*>;

// Some sermon PDFs display correctly but carry no ToUnicode table at all: their font's own codes are the real character's ASCII code
// plus one constant that stays the same for every letter, space and mark that font shows anywhere in the document - a copy-paste
// deterrent, not a real encoding. It is found by trying every possible shift (0..255) and keeping the one whose result looks the most
// like English prose - mostly lower-case letters and spaces, since those two dominate ordinary text far more than any other bytes do -
// rather than assuming which raw byte is the space (the space is not always this font's single commonest byte). Too little text, or no
// shift that reads convincingly as prose, is not trusted (0: read the bytes as they already are).
int DetectShift(const std::array<int, 256>& hist) {
    int total = 0;
    for (int b = 0; b < 256; ++b) total += hist[b];
    if (total < 200) return 0;
    int bestShift = 0, bestScore = -1;
    for (int shift = 1; shift < 256; ++shift) {
        int score = 0;
        for (int b = 0; b < 256; ++b) {
            if (hist[b] == 0) continue;
            const int shown = (b + shift) % 256;
            if (shown == ' ' || (shown >= 'a' && shown <= 'z')) score += hist[b];
        }
        if (score > bestScore) { bestScore = score; bestShift = shift; }
    }
    return bestScore * 100 >= total * 55 ? bestShift : 0;
}

// A pass over a content stream that reads nothing but which font is showing which raw bytes (Tf switches the current font; a string
// operand's bytes, still undecoded, are tallied against it) - everything else (positioning, other operators) is skipped over. Used
// before the real read, to find each no-ToUnicode font's shift (see DetectShift) from its own text across the whole document.
void CollectFontBytes(const std::string& ops, const FontTable& fonts, std::unordered_map<PdfFont*, std::array<int, 256>>& hist) {
    PdfFont* font = nullptr;
    std::string pendingName;
    for (size_t i = 0; i < ops.size();) {
        const char c = ops[i];
        if (IsPdfSpace(c)) { ++i; continue; }
        if (c == '%') { while (i < ops.size() && ops[i] != '\n' && ops[i] != '\r') ++i; continue; }
        if (c == '(') {
            int depth = 1;
            const bool tally = font && !font->hasMap && !font->twoByte;
            for (++i; i < ops.size(); ++i) {
                if (ops[i] == '\\' && i + 1 < ops.size()) { ++i; continue; }
                if (ops[i] == '(') ++depth;
                else if (ops[i] == ')' && --depth == 0) { ++i; break; }
                else if (tally) ++hist[font][static_cast<unsigned char>(ops[i])];
            }
            continue;
        }
        if (c == '<' && i + 1 < ops.size() && ops[i + 1] == '<') { i += 2; continue; }
        if (c == '<') {
            std::string hex;
            for (++i; i < ops.size() && ops[i] != '>'; ++i)
                if (std::isxdigit(static_cast<unsigned char>(ops[i]))) hex += ops[i];
            if (i < ops.size()) ++i;
            if (hex.size() % 2) hex += '0';
            if (font && !font->hasMap && !font->twoByte)
                for (size_t k = 0; k + 2 <= hex.size(); k += 2)
                    ++hist[font][static_cast<unsigned char>(std::stoi(hex.substr(k, 2), nullptr, 16))];
            continue;
        }
        if (c == '>' || c == '[' || c == ']') { ++i; continue; }
        if (c == '/') {
            const size_t b = ++i;
            while (i < ops.size() && !IsPdfSpace(ops[i]) && !IsPdfDelim(ops[i])) ++i;
            pendingName = ops.substr(b, i - b);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            while (i < ops.size() && (std::isdigit(static_cast<unsigned char>(ops[i])) || ops[i] == '-' || ops[i] == '+' || ops[i] == '.')) ++i;
            continue;
        }
        const size_t b = i;
        if (c == '\'' || c == '"') ++i;
        else while (i < ops.size() && !IsPdfSpace(ops[i]) && !IsPdfDelim(ops[i])) ++i;
        const std::string op = ops.substr(b, i - b);
        if (op == "Tf" && !pendingName.empty()) {
            auto it = fonts.find(pendingName);
            font = it != fonts.end() ? it->second : nullptr;
        }
    }
}

struct Operand {
    enum Kind { Num, Str, Name, Arr } kind = Num;
    double num = 0;
    std::string str;
    std::vector<Operand> items;
};

void ReadContent(const std::string& ops, const FontTable& fonts, std::vector<PdfLine>& lines) {
    const PdfFont* font = nullptr;
    double ly = 0, leading = 0, curY = 0;
    bool haveLine = false;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) lines.push_back({ curY, cur }); cur.clear(); };
    auto moveTo = [&](double y) {
        if (!haveLine || std::fabs(y - curY) > 0.5) { flush(); curY = y; haveLine = true; }
    };
    auto show = [&](const std::string& bytes) { moveTo(ly); cur += DecodeShown(font, bytes); };

    std::vector<Operand> stack;
    std::vector<size_t> arrays;   // where each open [ started in the stack
    for (size_t i = 0; i < ops.size();) {
        const char c = ops[i];
        if (IsPdfSpace(c)) { ++i; continue; }
        if (c == '%') { while (i < ops.size() && ops[i] != '\n' && ops[i] != '\r') ++i; continue; }
        if (c == '(') {
            std::string raw;
            int depth = 1;
            for (++i; i < ops.size(); ++i) {
                if (ops[i] == '\\' && i + 1 < ops.size()) { raw += ops[i]; raw += ops[++i]; continue; }
                if (ops[i] == '(') ++depth;
                else if (ops[i] == ')' && --depth == 0) break;
                raw += ops[i];
            }
            ++i;
            Operand o; o.kind = Operand::Str; o.str = UnescapePdfString(raw);
            stack.push_back(std::move(o));
            continue;
        }
        if (c == '<') {
            if (i + 1 < ops.size() && ops[i + 1] == '<') { i += 2; continue; }
            std::string hex;
            for (++i; i < ops.size() && ops[i] != '>'; ++i)
                if (std::isxdigit(static_cast<unsigned char>(ops[i]))) hex += ops[i];
            ++i;
            if (hex.size() % 2) hex += '0';
            Operand o; o.kind = Operand::Str;
            for (size_t k = 0; k + 2 <= hex.size(); k += 2) o.str += static_cast<char>(std::stoi(hex.substr(k, 2), nullptr, 16));
            stack.push_back(std::move(o));
            continue;
        }
        if (c == '>') { ++i; continue; }
        if (c == '[') { arrays.push_back(stack.size()); ++i; continue; }
        if (c == ']') {
            Operand arr; arr.kind = Operand::Arr;
            const size_t from = arrays.empty() ? stack.size() : arrays.back();
            if (!arrays.empty()) arrays.pop_back();
            for (size_t k = from; k < stack.size(); ++k) arr.items.push_back(std::move(stack[k]));
            stack.resize(std::min(from, stack.size()));
            stack.push_back(std::move(arr));
            ++i;
            continue;
        }
        if (c == '/') {
            const size_t b = ++i;
            while (i < ops.size() && !IsPdfSpace(ops[i]) && !IsPdfDelim(ops[i])) ++i;
            Operand o; o.kind = Operand::Name; o.str = ops.substr(b, i - b);
            stack.push_back(std::move(o));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            const size_t b = i;
            while (i < ops.size() && (std::isdigit(static_cast<unsigned char>(ops[i])) || ops[i] == '-' || ops[i] == '+' || ops[i] == '.')) ++i;
            Operand o; o.num = std::atof(ops.substr(b, i - b).c_str());
            stack.push_back(std::move(o));
            continue;
        }
        // an operator
        size_t b = i;
        if (c == '\'' || c == '"') ++i;
        else while (i < ops.size() && !IsPdfSpace(ops[i]) && !IsPdfDelim(ops[i])) ++i;
        const std::string op = ops.substr(b, i - b);
        auto num = [&](size_t fromEnd) { return stack.size() > fromEnd ? stack[stack.size() - 1 - fromEnd].num : 0.0; };
        auto str = [&]() -> const std::string* { return !stack.empty() && stack.back().kind == Operand::Str ? &stack.back().str : nullptr; };

        if (op == "BT") { ly = 0; }
        else if (op == "Tf") {
            if (stack.size() >= 2 && stack[stack.size() - 2].kind == Operand::Name) {
                auto it = fonts.find(stack[stack.size() - 2].str);
                font = it != fonts.end() ? it->second : nullptr;
            }
        }
        else if (op == "TL") { leading = num(0); }
        else if (op == "Td") { ly += num(0); moveTo(ly); }
        else if (op == "TD") { leading = -num(0); ly += num(0); moveTo(ly); }
        else if (op == "Tm") { ly = num(0); moveTo(ly); }
        else if (op == "T*") { ly -= leading; moveTo(ly); }
        else if (op == "Tj") { if (const std::string* s = str()) show(*s); }
        else if (op == "'" || op == "\"") { ly -= leading; moveTo(ly); if (const std::string* s = str()) show(*s); }
        else if (op == "TJ") {
            if (!stack.empty() && stack.back().kind == Operand::Arr)
                for (const Operand& e : stack.back().items) {
                    if (e.kind == Operand::Str) show(e.str);
                    else if (e.kind == Operand::Num && e.num < -200.0 && !cur.empty() && cur.back() != ' ' && !(font && font->hasMap && font->hasSpace))
                        cur += ' ';   // a gap the size of a word space (when spaces are not real glyphs; otherwise it is tracking)
                }
        }
        stack.clear();
        arrays.clear();
    }
    flush();
}

// The pages' lines -> text, paragraphs separated by a blank line. A paragraph starts where the line spacing opens up, or at a line that
// begins with a paragraph number ("75 Now, I want you to notice...") after a line that ended a sentence.
bool StartsWithParagraphNumber(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    return i >= 1 && i <= 3 && i + 1 < t.size() && t[i] == ' ' && !std::isdigit(static_cast<unsigned char>(t[i + 1]));
}

bool EndsSentence(const std::string& t) {
    if (t.empty()) return true;
    const char c = t.back();
    if (c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == '"' || c == ')') return true;
    return t.size() >= 3 && (t.compare(t.size() - 3, 3, "\xE2\x80\x9D") == 0 || t.compare(t.size() - 3, 3, "\xE2\x80\x99") == 0);   // ” ’
}

// The page's visible box (CropBox, else MediaBox) as { x0, y0, x1, y1 }; false when the page does not say.
bool PageBox(const std::string& dict, double box[4]) {
    for (const char* key : { "/CropBox", "/MediaBox" }) {
        const size_t k = FindKey(dict, key);
        if (k == std::string::npos) continue;
        const size_t open = dict.find('[', k);
        if (open == std::string::npos) continue;
        const char* p = dict.c_str() + open + 1;
        char* end = nullptr;
        int n = 0;
        for (; n < 4; ++n) {
            box[n] = std::strtod(p, &end);
            if (end == p) break;
            p = end;
        }
        if (n == 4) return true;
    }
    return false;
}

// Em / en / thin / no-break spaces (a paragraph number is often followed by an em space) become plain spaces.
std::string NormalizeSpaces(std::string s) {
    for (const char* sp : { "\xE2\x80\x83", "\xE2\x80\x82", "\xE2\x80\x89", "\xC2\xA0" })   // em, en, thin, no-break
        for (size_t at = 0; (at = s.find(sp, at)) != std::string::npos;) s.replace(at, std::strlen(sp), " ");
    return s;
}

bool StartsLowercase(const std::string& t) { return !t.empty() && t[0] >= 'a' && t[0] <= 'z'; }

// Removes what is printed on the page but is not the sermon: a bare page number, and a running head or footer - a line that opens or
// closes many pages with the same words ("The Spoken Word", "Adoption 1 60-0515E"). Digits and spacing do not count in the comparison,
// so "Page 3" and "Page 4" are the same line.
void DropRunningLines(std::vector<std::vector<PdfLine>>& pages) {
    constexpr size_t kEdge = 2;   // lines from the top and from the bottom of a page that can be a head or foot
    auto key = [](const std::string& raw) {
        std::string k;
        for (const unsigned char c : NormalizeSpaces(raw))
            if (std::isalpha(c) || c >= 0x80) k += static_cast<char>(std::tolower(c));
        return k;
    };
    // A bare number that is the page's own number (give or take a cover page or two). A paragraph number standing alone at the top of a
    // page is a different number, so it stays.
    auto pageNumber = [](const std::string& raw, size_t pageIndex) {
        const std::string t = Trim(NormalizeSpaces(raw));
        if (t.empty() || t.size() > 8) return false;
        int value = 0;
        bool digit = false;
        for (const char c : t) {
            if (std::isdigit(static_cast<unsigned char>(c))) { digit = true; value = value * 10 + (c - '0'); }
            else if (c != '-' && c != ' ' && c != '[' && c != ']' && c != '(' && c != ')') return false;
        }
        return digit && std::abs(value - static_cast<int>(pageIndex + 1)) <= 2;
    };
    auto atEdge = [&](const std::vector<PdfLine>& lines, size_t i) { return i < kEdge || i + kEdge >= lines.size(); };

    std::unordered_map<std::string, size_t> seen;   // on how many pages each edge line turns up
    for (const std::vector<PdfLine>& lines : pages) {
        std::vector<std::string> onThisPage;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!atEdge(lines, i)) continue;
            const std::string k = key(lines[i].text);
            if (k.size() < 3 || std::find(onThisPage.begin(), onThisPage.end(), k) != onThisPage.end()) continue;
            onThisPage.push_back(k);
            ++seen[k];
        }
    }
    const size_t needed = std::max<size_t>(3, pages.size() * 3 / 10);
    for (size_t p = 0; p < pages.size(); ++p) {
        std::vector<PdfLine>& lines = pages[p];
        std::vector<PdfLine> kept;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (atEdge(lines, i)) {
                if (pageNumber(lines[i].text, p)) continue;
                if (pages.size() >= 4) {
                    const auto it = seen.find(key(lines[i].text));
                    if (it != seen.end() && it->second >= needed) continue;
                }
            }
            kept.push_back(std::move(lines[i]));
        }
        lines = std::move(kept);
    }
    pages.erase(std::remove_if(pages.begin(), pages.end(), [](const std::vector<PdfLine>& l) { return l.empty(); }), pages.end());
}

std::string PagesToText(std::vector<std::vector<PdfLine>> pages) {
    DropRunningLines(pages);
    std::string out;
    std::string prev;
    std::string carry;   // a paragraph number seen on its own line, waiting for the line it belongs to
    for (const std::vector<PdfLine>& lines : pages) {
        std::vector<double> gaps;
        for (size_t i = 1; i < lines.size(); ++i) {
            const double g = std::fabs(lines[i - 1].y - lines[i].y);
            if (g > 0.5) gaps.push_back(g);
        }
        std::sort(gaps.begin(), gaps.end());
        const double normal = gaps.empty() ? 0.0 : gaps[gaps.size() / 2];
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string text = Collapse(Trim(NormalizeSpaces(lines[i].text)));
            if (!text.empty() && text[0] == '`') text = Trim(text.substr(1));   // a stray paragraph mark
            if (text.empty()) continue;
            // A paragraph number on a line of its own belongs to the line after it: in the middle of a sentence it is dropped, after a
            // finished one it opens the next paragraph.
            if (text.size() <= 3 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
                if (out.empty() || EndsSentence(prev)) carry = text;
                continue;
            }
            const double gap = i > 0 ? std::fabs(lines[i - 1].y - lines[i].y) : 0.0;
            const bool byGap = !out.empty() && normal > 0 && gap > normal * 1.35;
            bool byNumber = !out.empty() && StartsWithParagraphNumber(text) && EndsSentence(prev);
            if (!carry.empty()) {
                text = carry + " " + text;
                carry.clear();
                byNumber = !out.empty();
            }
            bool newParagraph = out.empty() || byGap || byNumber;
            // A wider gap in the middle of a sentence is a change of font or size, not a paragraph. (A paragraph number that landed at the
            // start of such a line - "2 from Georgia" - is not part of the sentence either.)
            if (newParagraph && !byNumber && !out.empty() && !EndsSentence(prev)) {
                size_t k = 0;
                while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
                const bool numbered = k >= 1 && k <= 3 && k + 1 < text.size() && text[k] == ' ';
                const std::string body = numbered ? text.substr(k + 1) : text;
                if (StartsLowercase(body)) { newParagraph = false; text = body; }
            }
            if (newParagraph) { if (!out.empty()) out += "\n\n"; }
            else if (!out.empty() && out.back() != ' ') out += ' ';
            out += text;
            prev = text;
        }
    }
    return out;
}

std::string TextFromPdf(const std::string& data) {
    const PdfObjects objs = ReadObjects(data);
    if (objs.empty()) return {};

    // fonts, built once each
    std::map<int, PdfFont> fontCache;
    auto fontOf = [&](int num) -> PdfFont* {
        auto cached = fontCache.find(num);
        if (cached != fontCache.end()) return &cached->second;
        auto it = objs.find(num);
        if (it == objs.end()) return nullptr;
        PdfFont f;
        f.twoByte = it->second.dict.find("/Type0") != std::string::npos;
        for (const int u : RefsAfter(it->second.dict, "/ToUnicode")) {
            auto tu = objs.find(u);
            if (tu != objs.end()) ParseToUnicode(DecodeStream(tu->second), f);
        }
        return &(fontCache[num] = std::move(f));
    };

    // The pages in their order: down the page tree from the root, or (when there is none) the page objects as they come.
    std::vector<int> pageNums;
    std::function<void(int, int)> walk = [&](int num, int depth) {
        auto it = objs.find(num);
        if (it == objs.end() || depth > 40) return;
        const std::string& d = it->second.dict;
        if (FindKey(d, "/Kids") != std::string::npos && d.find("/Pages") != std::string::npos) {
            for (const int kid : RefsAfter(d, "/Kids")) walk(kid, depth + 1);
        } else {
            pageNums.push_back(num);
        }
    };
    for (const auto& [num, obj] : objs)
        if (FindKey(obj.dict, "/Kids") != std::string::npos && obj.dict.find("/Pages") != std::string::npos && FindKey(obj.dict, "/Parent") == std::string::npos)
            walk(num, 0);
    if (pageNums.empty())
        for (const auto& [num, obj] : objs)
            if (obj.dict.find("/Type") != std::string::npos && obj.dict.find("/Page") != std::string::npos && obj.dict.find("/Pages") == std::string::npos && FindKey(obj.dict, "/Contents") != std::string::npos)
                pageNums.push_back(num);

    // Pass one: every page's fonts and content stream, gathered once (so a font used on many pages is seen whole before anything is
    // decoded), and - for a font with no ToUnicode table - a tally of the raw bytes it shows, to find its shift (see DetectShift).
    struct PageWork {
        FontTable fonts;
        std::string ops;
        int pageNum = 0;
    };
    std::vector<PageWork> work;
    std::unordered_map<PdfFont*, std::array<int, 256>> hist;
    auto gather = [&](int pageNum, const PdfObject& page) {
        // Resources may be inherited from a parent
        std::string resources = DictAfter(page.dict, "/Resources", objs);
        for (int hop = 0, at = pageNum; resources.empty() && hop < 20; ++hop) {
            const std::vector<int> parent = RefsAfter(objs.at(at).dict, "/Parent");
            if (parent.empty() || !objs.count(parent.front())) break;
            at = parent.front();
            resources = DictAfter(objs.at(at).dict, "/Resources", objs);
        }
        PageWork pw;
        pw.pageNum = pageNum;
        const std::string fontDict = DictAfter(resources, "/Font", objs);
        for (size_t i = 0; i < fontDict.size(); ++i) {
            if (fontDict[i] != '/') continue;
            const size_t b = ++i;
            while (i < fontDict.size() && !IsPdfSpace(fontDict[i]) && !IsPdfDelim(fontDict[i])) ++i;
            const std::string name = fontDict.substr(b, i - b);
            size_t j = i;
            if (const int ref = ReadRef(fontDict, j)) { pw.fonts[name] = fontOf(ref); i = j - 1; }   // (the loop's ++i lands on the next "/")
        }
        for (const int c : RefsAfter(page.dict, "/Contents")) {
            auto it = objs.find(c);
            if (it != objs.end()) { pw.ops += DecodeStream(it->second); pw.ops += '\n'; }
        }
        CollectFontBytes(pw.ops, pw.fonts, hist);
        work.push_back(std::move(pw));
    };
    for (const int pageNum : pageNums) gather(pageNum, objs.at(pageNum));
    for (auto& [font, tally] : hist) font->shift = DetectShift(tally);

    // Pass two: the real read, with every font's shift (if any) already known.
    std::vector<std::vector<PdfLine>> pages;
    for (const PageWork& pw : work) {
        const PdfObject& page = objs.at(pw.pageNum);
        std::vector<PdfLine> lines;
        ReadContent(pw.ops, pw.fonts, lines);
        double box[4] = { 0, 0, 0, 0 };
        if (PageBox(page.dict, box))
            lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const PdfLine& l) { return l.y < box[1] - 2 || l.y > box[3] + 2; }), lines.end());
        if (!lines.empty()) pages.push_back(std::move(lines));
    }

    // No page tree to follow (a stripped-down PDF): read every stream that carries text operators, in simple encoding.
    if (pages.empty()) {
        for (const auto& [num, obj] : objs) {
            const std::string body = DecodeStream(obj);
            if (!body.empty() && LooksLikeTextOps(body)) {
                std::vector<PdfLine> lines;
                ReadContent(body, {}, lines);
                if (!lines.empty()) pages.push_back(std::move(lines));
            }
        }
    }
    return PagesToText(pages);
}

// Plain-text / extracted-PDF text -> paragraphs (blank-line separated).
std::vector<std::string> ParagraphsFromText(const std::string& text) {
    std::vector<std::string> out;
    std::string buf;
    auto flush = [&]() {
        std::string para = Collapse(Trim(buf));
        buf.clear();
        if (para.size() >= 40) out.push_back(std::move(para));   // page artifacts are short
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = Trim(line);
        if (t.empty()) {
            if (!buf.empty()) flush();
        } else {
            buf += t;
            buf += ' ';
        }
    }
    if (!buf.empty()) flush();
    // Degenerate case: everything landed in one bucket below the threshold
    // (a stub file) — keep what is there.
    if (out.empty() && !Trim(text).empty()) {
        const std::string para = Collapse(Trim(text));
        if (!para.empty()) out.push_back(para);
    }
    return out;
}

// The sermon PDFs' own stand-ins for punctuation, put right: "_" is a dash ("after_after", "words._Ed."), "^" is a sentence broken off
// ("we will^Some of them"). Applied to text read from a PDF only.
std::string TidyMarkers(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            out += "\xE2\x80\x94";
        } else if (s[i] == '^') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += "\xE2\x80\xA6";
            if (i + 1 < s.size() && (std::isalnum(static_cast<unsigned char>(s[i + 1])) || static_cast<unsigned char>(s[i + 1]) >= 0x80)) out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// "The Spoken Word" - the running head printed on nearly every page of these sermons (the series' own name, not something anyone
// said) - some pages' header font has no space glyph at all, so it (and a glued page number, and sometimes a glued "IS") comes out as
// one run of capitals with no spaces: "2THESPOKENWORD", "THESPOKENWORDIS". Struck wherever that exact run of capitals appears - never
// the ordinary, mixed-case "the spoken Word" the sermons themselves say constantly, which this never matches.
std::string StripSpokenWordBanner(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 3 <= s.size() && s.compare(i, 3, "THE") == 0) {
            size_t j = i + 3;
            while (j < s.size() && s[j] == ' ') ++j;
            if (j + 6 <= s.size() && s.compare(j, 6, "SPOKEN") == 0) {
                size_t k = j + 6;
                while (k < s.size() && s[k] == ' ') ++k;
                if (k + 4 <= s.size() && s.compare(k, 4, "WORD") == 0) {
                    size_t end = k + 4;
                    if (end + 2 <= s.size() && s.compare(end, 2, "IS") == 0) end += 2;
                    // a page number can land on either side, glued the same way
                    while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
                    while (!out.empty() && std::isdigit(static_cast<unsigned char>(out.back()))) out.pop_back();
                    i = end;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// The early sermons label their paragraphs inline - "E-1", "E-2", ... - with no gap between them: a label after a finished sentence
// starts a paragraph.
std::string SplitCodedParagraphs(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 64);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == 'E' && i + 2 < s.size() && s[i + 1] == '-' && std::isdigit(static_cast<unsigned char>(s[i + 2])) && (i == 0 || s[i - 1] == ' ')) {
            size_t j = i + 2;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            size_t end = out.size();
            while (end > 0 && out[end - 1] == ' ') --end;
            if (j - (i + 2) <= 3 && j < s.size() && s[j] == ' ' && end > 0 && out[end - 1] != '\n' &&
                EndsSentence(out.substr(end > 3 ? end - 3 : 0, end > 3 ? 3 : end))) {
                out.erase(end);
                out += "\n\n";
            }
        }
        out += s[i];
    }
    return out;
}

bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        const size_t extra = (c & 0xE0) == 0xC0 && c >= 0xC2 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 3 : 0;
        if (extra == 0 || i + extra >= s.size()) return false;
        for (size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

// The clean-up stage: any sermon text (extracted from a PDF, or a .txt of unknown make) as plain UTF-8 with one paragraph per block,
// blocks separated by one blank line. Nothing is left that is not text: no byte-order mark, control characters, soft hyphens,
// zero-width marks, odd spaces or runs of spaces and blank lines.
std::string CleanSermonText(const std::string& raw) {
    std::string text = raw;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (!ValidUtf8(text)) text = Cp1252ToUtf8(text);   // a .txt saved by an old editor

    // characters that carry nothing: soft hyphen, zero-width space / joiners, byte-order mark
    for (const char* mark : { "\xC2\xAD", "\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", "\xEF\xBB\xBF" })
        for (size_t at = 0; (at = text.find(mark, at)) != std::string::npos;) text.erase(at, std::strlen(mark));
    text = NormalizeSpaces(std::move(text));

    std::string out;
    bool blank = true;   // the last thing written was a blank line (or nothing yet)
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        for (char& c : line) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F) c = ' ';   // tabs, form feeds, stray codes, carriage returns
        }
        line = Collapse(line);
        if (line.empty()) {
            if (!blank) { out += '\n'; blank = true; }
            continue;
        }
        out += line;
        out += '\n';
        blank = false;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// Many of these sermons print their own paragraph number ("2 And now we have a very wonderful pastor...") - the number a citation of
// the sermon names ("53-0325 12"). Where a paragraph carries one, it becomes the verse's number (and the printed digits come out of its
// text), and that is where a new verse begins; that is what makes the library's chapter:verse match the source.
//
// A sermon like this prints a number on SOME of its paragraph breaks, not all of them: the opening is one long unlabeled paragraph
// (verse 1) that our own blank-line splitting may still have cut into several pieces (a typographic gap, not a new verse), before the
// first number appears. So once a sermon shows it numbers its own paragraphs at all, every blank-line break with no number on it is
// folded into the verse before it, rather than becoming a wrongly-numbered verse of its own; a sermon that never prints a number keeps
// the plain shape (one verse per blank-line paragraph, numbered in order) it always had.
struct NumberedParagraph {
    int number = 0;
    std::string text;
};

// A leading "N " on `para` that reads as a real label (a capital letter, a quote mark, or another script right after it - never a
// digit or a lower-case letter, so "3 million people..." mid-paragraph is never mistaken for one) and goes up from `after` - a real
// paragraph number only ever increases. 0 when there is none.
int ParagraphLabel(const std::string& para, int after) {
    size_t k = 0;
    while (k < para.size() && std::isdigit(static_cast<unsigned char>(para[k]))) ++k;
    if (k < 1 || k > 3 || k + 1 >= para.size() || para[k] != ' ') return 0;
    const unsigned char next = static_cast<unsigned char>(para[k + 1]);
    if (!(std::isupper(next) || next == '"' || next >= 0x80)) return 0;
    const int n = std::atoi(para.c_str());
    return n > after ? n : 0;
}

std::vector<NumberedParagraph> NumberParagraphs(const std::vector<std::string>& paragraphs) {
    // Does the sermon label its own paragraphs anywhere? (The first paragraph is never a label - it is what comes before the first one.)
    bool numbered = false;
    for (size_t i = 1, prev = 0; i < paragraphs.size(); ++i)
        if (const int n = ParagraphLabel(paragraphs[i], static_cast<int>(prev)); n > 0) { numbered = true; prev = n; }

    std::vector<NumberedParagraph> out;
    int prev = 0;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        const int n = i > 0 ? ParagraphLabel(paragraphs[i], prev) : 0;
        if (n > 0) {
            const size_t sp = paragraphs[i].find(' ');
            out.push_back({ n, Trim(paragraphs[i].substr(sp + 1)) });
            prev = n;
        } else if (numbered && !out.empty()) {
            out.back().text += "\n\n" + paragraphs[i];
        } else {
            out.push_back({ static_cast<int>(out.size()) + 1, paragraphs[i] });
        }
    }
    return out;
}

// Whether text reads as text. An earlier PDF reader stored the raw glyph codes of PDFs with embedded fonts: control characters and
// bytes that are not UTF-8. Such a sermon is replaced when it is imported again.
bool ReadableText(const std::string& s) {
    size_t letters = 0, junk = 0, total = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\n' || c == '\t') { ++i; continue; }
        ++total;
        if (c < 0x20 || c == 0x7F) { ++junk; ++i; continue; }
        if (c < 0x80) { if (std::isalpha(c)) ++letters; ++i; continue; }
        const size_t extra = (c & 0xE0) == 0xC0 && c >= 0xC2 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        bool valid = extra > 0 && i + extra < s.size();
        for (size_t k = 1; valid && k <= extra; ++k) valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (valid) { ++letters; i += extra + 1; } else { ++junk; ++i; }
    }
    return total > 0 && letters * 100 >= total * 55 && junk * 100 <= total * 2;
}

// (ContainsWord / ContainsPhrase live in modules/search/TextMatching.hpp —
// shared with the Search Engine so both search surfaces can never disagree
// again about what counts as a word or a phrase.)

bool ChapterReadable(const TheTableChapter& ch) {
    std::string sample;
    for (const TheTableVerse& v : ch.verses) {
        if (sample.size() > 4000) break;
        sample += v.text;
        sample += ' ';
    }
    return ReadableText(sample);
}

} // namespace

bool TheTableLibrary::VerseKnownLocked(std::string_view bookId, int chapter, int verse) const {
    for (const TheTableBook& b : books_) {
        if (b.id != bookId) continue;
        for (const TheTableChapter& c : b.chapters) {
            if (c.number != chapter) continue;
            for (const TheTableVerse& v : c.verses)
                if (v.number == verse) return true;
            return false;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// User data (notes and highlights) — the Bible module's user-data section,
// keyed the table way: "bookId:chapter:verse". Persisted with the library
// JSON (Persist writes it, Load reads it back), never inside the sermon text.
// ---------------------------------------------------------------------------
namespace {
std::string NoteKey(std::string_view bookId, int chapter, int verse) {
    return std::format("{}:{}:{}", bookId, chapter, verse);
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

Result<void> TheTableLibrary::AddNote(std::string_view bookId, int chapter, int verse,
                                      const std::string& text) {
    if (text.empty())
        return Error::Make(Err::InvalidArgument, kModule, "note text is empty");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!VerseKnownLocked(bookId, chapter, verse))
            return Error::Make(Err::NotFound, kModule, "no such paragraph");

        TableNote note;
        note.id = std::format("note-{}", NowMs());
        note.ref = CitationLocked(bookId, chapter, verse);
        note.bookId = std::string(bookId);
        note.chapter = chapter;
        note.verse = verse;
        note.text = text;
        // Monotonic stamp: two notes in the same wall-clock millisecond must
        // not tie (the drawer sorts most-recent-first, and a tie makes the
        // order ambiguous — the round-trip test caught exactly that).
        note.createdMs = note.modifiedMs = std::max(NowMs(), lastNoteMs_ + 1);
        lastNoteMs_ = note.modifiedMs;
        notes_[NoteKey(bookId, chapter, verse)] = std::move(note);
    }
    return Save();
}

Result<void> TheTableLibrary::RemoveNote(std::string_view bookId, int chapter, int verse) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (notes_.erase(NoteKey(bookId, chapter, verse)) == 0)
            return Ok();   // nothing stored: already gone, nothing to write
    }
    return Save();
}

Result<std::vector<TableNote>> TheTableLibrary::Notes(std::string_view bookId, int chapter,
                                                      int verse) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = notes_.find(NoteKey(bookId, chapter, verse));
    if (it == notes_.end())
        return std::vector<TableNote>{};
    return std::vector<TableNote>{ it->second };
}

Result<std::vector<TableNote>> TheTableLibrary::Notes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TableNote> out;
    out.reserve(notes_.size());
    for (const auto& [key, note] : notes_) {
        (void)key;
        out.push_back(note);
    }
    std::sort(out.begin(), out.end(),
              [](const TableNote& a, const TableNote& b) { return a.modifiedMs > b.modifiedMs; });
    return out;
}

Result<void> TheTableLibrary::SetHighlight(std::string_view bookId, int chapter, int verse, bool on) {
    if (on) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!VerseKnownLocked(bookId, chapter, verse))
            return Error::Make(Err::NotFound, kModule, "no such paragraph");
        highlights_[NoteKey(bookId, chapter, verse)] = true;
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        highlights_.erase(NoteKey(bookId, chapter, verse));
    }
    return Save();
}

Result<std::vector<std::string>> TheTableLibrary::Highlights() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(highlights_.size());
    for (const auto& [key, on] : highlights_) {
        (void)on;
        out.push_back(key);
    }
    return out;
}

Result<bool> TheTableLibrary::IsHighlighted(std::string_view bookId, int chapter, int verse) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = highlights_.find(NoteKey(bookId, chapter, verse));
    return it != highlights_.end() && it->second;
}

// ---------------------------------------------------------------------------
// Construction / persistence
// ---------------------------------------------------------------------------
TheTableLibrary::TheTableLibrary(std::string filePath) : filePath_(std::move(filePath)) {}

std::shared_ptr<TheTableLibrary> TheTableLibrary::Open(const std::string& filePath) {
    auto lib = std::shared_ptr<TheTableLibrary>(new TheTableLibrary(filePath));
    (void)lib->Load();   // a missing file is an empty library, not an error
    return lib;
}

Result<void> TheTableLibrary::Save() const {
    // Only the copy happens under the lock; serialising and writing a big library take seconds and must not hold up browsing.
    std::vector<TheTableBook> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = books_;
    }
    return Persist(snapshot);
}

Result<void> TheTableLibrary::Persist(const std::vector<TheTableBook>& books) const {
    J::Object root;
    root["schema"] = J::Number(1);
    J::Array booksArr;
    for (const TheTableBook& book : books) {
        J::Object b;
        b["id"] = J::String(book.id);
        b["name"] = J::String(book.name);
        b["order"] = J::Number(book.order);
        J::Array chs;
        for (const TheTableChapter& ch : book.chapters) {
            J::Object c;
            c["number"] = J::Number(ch.number);
            c["title"] = J::String(ch.title);
            if (!ch.code.empty()) c["code"] = J::String(ch.code);
            J::Array vs;
            for (const TheTableVerse& v : ch.verses) {
                J::Object vo;
                vo["number"] = J::Number(v.number);
                vo["text"] = J::String(v.text);
                if (!v.heading.empty()) vo["heading"] = J::String(v.heading);
                vs.push_back(J(std::move(vo)));
            }
            c["verses"] = J(std::move(vs));
            chs.push_back(J(std::move(c)));
        }
        b["chapters"] = J(std::move(chs));
        booksArr.push_back(J(std::move(b)));
    }
    root["books"] = J(std::move(booksArr));

    // User notes and highlights ride the same document: a note survives an
    // import, a re-save, everything short of deleting the file.
    J::Array notesArr;
    for (const auto& [key, note] : notes_) {
        (void)key;
        J::Object n;
        n["id"] = J::String(note.id);
        n["ref"] = J::String(note.ref);
        n["bookId"] = J::String(note.bookId);
        n["chapter"] = J::Number(note.chapter);
        n["verse"] = J::Number(note.verse);
        n["text"] = J::String(note.text);
        n["createdMs"] = J::Number(static_cast<double>(note.createdMs));
        n["modifiedMs"] = J::Number(static_cast<double>(note.modifiedMs));
        notesArr.push_back(J(std::move(n)));
    }
    root["notes"] = J(std::move(notesArr));
    J::Array hlArr;
    for (const auto& [key, on] : highlights_) {
        (void)on;
        hlArr.push_back(J::String(key));
    }
    root["highlights"] = J(std::move(hlArr));

    auto& platform = platform::PlatformAccessor::Get();
    // Ensure the file's own parent exists (the library may live anywhere —
    // tests use a temp dir; production uses the user data dir).
    (void)platform.Filesystem().CreateDirectories(
        std::filesystem::path(filePath_).parent_path().generic_string());
    return platform.Filesystem().Write(filePath_, J(std::move(root)).ToString());
}

Result<void> TheTableLibrary::Load() {
    // scanMutex_ first: a concurrent Search()'s scan must never see the
    // between-clears state (books_ empty, cache alive → stale pointers) nor
    // interleave with the reload.
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    books_.clear();
    lowerParagraphs_.clear();

    auto& platform = platform::PlatformAccessor::Get();
    auto body = platform.Filesystem().ReadText(filePath_);
    if (!body.ok()) return Ok();   // first run: no file yet
    auto parsed = json::Parse(body.value());
    if (!parsed.ok())
        return Error::Make(kTableErr, kModule, "corrupt library JSON: " + parsed.error().message);
    const J& root = parsed.value();
    const J* booksArr = root.Find("books");
    if (!booksArr || !booksArr->asArray()) return Ok();

    for (const J& bv : *booksArr->asArray()) {
        TheTableBook book;
        book.id = std::string(bv.Find("id") ? bv.Find("id")->asString() : "");
        book.name = std::string(bv.Find("name") ? bv.Find("name")->asString() : "");
        book.order = static_cast<int>(bv.Find("order") ? bv.Find("order")->asInt() : 0);
        if (book.id.empty()) continue;
        if (const J* chs = bv.Find("chapters"); chs && chs->asArray()) {
            for (const J& cv : *chs->asArray()) {
                TheTableChapter ch;
                ch.number = static_cast<int>(cv.Find("number") ? cv.Find("number")->asInt() : 0);
                ch.title = std::string(cv.Find("title") ? cv.Find("title")->asString() : "");
                // The code joined this schema later: older files carry none, and
                // the title always opens with it when there is one.
                ch.code = std::string(cv.Find("code") ? cv.Find("code")->asString() : "");
                if (ch.code.empty()) ch.code = CodeFromTitle(ch.title);
                if (const J* vs = cv.Find("verses"); vs && vs->asArray()) {
                    for (const J& vv : *vs->asArray()) {
                        TheTableVerse v;
                        v.number = static_cast<int>(vv.Find("number") ? vv.Find("number")->asInt() : 0);
                        v.text = std::string(vv.Find("text") ? vv.Find("text")->asString() : "");
                        v.heading = std::string(vv.Find("heading") ? vv.Find("heading")->asString() : "");
                        if (!v.text.empty()) ch.verses.push_back(std::move(v));
                    }
                }
                if (!ch.verses.empty()) book.chapters.push_back(std::move(ch));
            }
        }
        books_.push_back(std::move(book));
    }
    std::sort(books_.begin(), books_.end(),
              [](const TheTableBook& a, const TheTableBook& b) { return a.order < b.order; });

    // User data (older files carry none — both sections stay empty then).
    notes_.clear();
    highlights_.clear();
    if (const J* notesArr = root.Find("notes"); notesArr && notesArr->asArray()) {
        for (const J& nv : *notesArr->asArray()) {
            TableNote note;
            note.id = std::string(nv.Find("id") ? nv.Find("id")->asString() : "");
            note.ref = std::string(nv.Find("ref") ? nv.Find("ref")->asString() : "");
            note.bookId = std::string(nv.Find("bookId") ? nv.Find("bookId")->asString() : "");
            note.chapter = static_cast<int>(nv.Find("chapter") ? nv.Find("chapter")->asInt() : 0);
            note.verse = static_cast<int>(nv.Find("verse") ? nv.Find("verse")->asInt() : 0);
            note.text = std::string(nv.Find("text") ? nv.Find("text")->asString() : "");
            note.createdMs = static_cast<int64_t>(nv.Find("createdMs") ? nv.Find("createdMs")->asNumber() : 0.0);
            note.modifiedMs = static_cast<int64_t>(nv.Find("modifiedMs") ? nv.Find("modifiedMs")->asNumber() : 0.0);
            if (note.bookId.empty() || note.text.empty())
                continue;   // skip a damaged entry, keep the rest
            notes_[NoteKey(note.bookId, note.chapter, note.verse)] = std::move(note);
        }
    }
    if (const J* hlArr = root.Find("highlights"); hlArr && hlArr->asArray()) {
        for (const J& hv : *hlArr->asArray()) {
            std::string key(hv.asString());
            if (!key.empty()) highlights_[key] = true;
        }
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// Search Engine integration (docs/specs/20 §DocumentAdapterRegistry): makes the
// platform Search Engine content-aware for type "table". Verse documents would
// carry their paragraph text as content; this adapter extracts it verbatim.
// ---------------------------------------------------------------------------
namespace {
class TheTableIndexAdapter final : public search::IIndexAdapter {
public:
    const char* Type() const noexcept override { return "table"; }
    bool CanIndex(const search::SearchDocument& doc) const override {
        return doc.type == "table";
    }
    Result<std::string> ExtractContent(const search::SearchDocument& doc) const override {
        return doc.content;
    }
};
} // namespace

// The document id of one sermon in the platform Search Engine. `chapter` is the
// 1-based sermon index within its year book — the same shape the Bible module
// uses ("bible:<bibleId>:<book>:<chapter>:<verse>", one level shorter).
std::string TheTableDocId(std::string_view bookId, int chapter) {
    return std::format("table:{}:{}", bookId, chapter);
}

// One document per SERMON: title + every paragraph's text. A paragraph-per-
// document split (the Bible's granularity) would put 300k+ documents into the
// engine for this library alone — the engine re-copies every candidate document
// per query — so the sermon stays one document and paragraph-level hits keep
// coming from this library's own Search().
std::string TheTableContent(const TheTableChapter& ch) {
    std::string content = ch.title;
    for (const TheTableVerse& v : ch.verses) {
        content += '\n';
        content += v.text;
    }
    return content;
}

Result<size_t> TheTableLibrary::IndexWithSearchEngine(const std::atomic<bool>* cancelled) {
    search::SearchEngine& engine = search::SearchEngine::Instance();

    // The adapter registers once per process; a re-register is a benign no-op
    // failure here (the engine rejects the duplicate).
    static std::once_flag adapterOnce;
    std::call_once(adapterOnce, [&engine] {
        (void)engine.RegisterAdapter(std::make_shared<TheTableIndexAdapter>());
    });

    // The library copy happens under the lock; indexing (an upsert per sermon)
    // runs on it so browsing is never held up behind the engine's work.
    std::vector<TheTableBook> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = books_;
    }

    size_t total = 0;
    for (const TheTableBook& book : snapshot)
        total += book.chapters.size();
    Logger::Instance().Info(std::format("The Table search indexing started ({} sermon documents)", total),
                            kModule);

    size_t indexed = 0;
    // One cache invalidation for the whole batch, not one per sermon.
    engine.SuspendCacheInvalidation();
    for (const TheTableBook& book : snapshot) {
        for (const TheTableChapter& ch : book.chapters) {
            // A set flag ends the walk BEFORE the next upsert (see the header):
            // this is what keeps a shutdown's join short — the pass stops within
            // one document instead of walking the whole library into an engine
            // that is being torn down underneath it.
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                engine.ResumeCacheInvalidation();   // (end the batch scope even when cancelled)
                return indexed;
            }
            search::SearchDocument doc;
            doc.id = TheTableDocId(book.id, ch.number);
            doc.type = "table";
            doc.title = ch.title;
            doc.content = TheTableContent(ch);
            // The author field carries the year book id (the Bible precedent:
            // filters type + author scope a search to one book).
            doc.author = book.id;
            doc.source = "table";
            doc.metadata["book"] = book.id;
            doc.metadata["year"] = book.name;
            doc.metadata["chapter"] = std::to_string(ch.number);
            // The citation line ("47-0412 - Faith Is The Substance") travels
            // with the document so search surfaces can quote it verbatim.
            doc.metadata["reference"] = SermonCitation(book.name, ch.code.empty() ? CodeFromTitle(ch.title) : ch.code, ch.title, 0);
            doc.tags = {"sermon", "table"};
            auto r = engine.IndexDocument(doc);
            if (!r.ok()) {
                engine.ResumeCacheInvalidation();   // (error path: end the batch scope)
                return r.error();
            }
            ++indexed;
            if (indexed % 25 == 0 || indexed == total)
                Logger::Instance().Info(std::format("The Table search indexing progress: {}/{}",
                                                    indexed, total), kModule);
        }
    }
    engine.ResumeCacheInvalidation();
    return indexed;
}

Result<void> TheTableLibrary::UnindexFromSearchEngine() {
    search::SearchEngine& engine = search::SearchEngine::Instance();
    std::vector<TheTableBook> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = books_;
    }
    for (const TheTableBook& book : snapshot)
        for (const TheTableChapter& ch : book.chapters)
            (void)engine.RemoveDocument(TheTableDocId(book.id, ch.number));
    return Ok();
}

// ---------------------------------------------------------------------------
// Browsing
// ---------------------------------------------------------------------------
std::vector<TheTableBook> TheTableLibrary::Books() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return books_;
}

std::vector<TheTableLibrary::BookInfo> TheTableLibrary::BookIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BookInfo> out;
    out.reserve(books_.size());
    for (const TheTableBook& b : books_) {
        BookInfo info;
        info.id = b.id;
        info.name = b.name;
        info.order = b.order;
        for (const TheTableChapter& ch : b.chapters) info.chapters.push_back({ ch.number, ch.title, ch.verses.size() });
        out.push_back(std::move(info));
    }
    return out;
}

Result<TheTableChapter> TheTableLibrary::GetChapter(std::string_view bookId, int chapter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const TheTableBook& book : books_) {
        if (book.id != bookId) continue;
        for (const TheTableChapter& ch : book.chapters)
            if (ch.number == chapter) return ch;
        break;
    }
        return Error::Make(Err::NotFound, kModule, "no such chapter");
}

std::string TheTableLibrary::Citation(std::string_view bookId, int chapter, int verse) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CitationLocked(bookId, chapter, verse);
}

std::string TheTableLibrary::CitationLocked(std::string_view bookId, int chapter, int verse) const {
    for (const TheTableBook& book : books_) {
        if (book.id != bookId) continue;
        for (const TheTableChapter& ch : book.chapters)
            if (ch.number == chapter)
                return SermonCitation(book.name, ch.code.empty() ? CodeFromTitle(ch.title) : ch.code,
                                      ch.title, verse);
        break;
    }
    return {};
}

size_t TheTableLibrary::VerseCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const TheTableBook& book : books_)
        for (const TheTableChapter& ch : book.chapters)
            n += ch.verses.size();
    return n;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------
Result<std::vector<TheTableSearchHit>> TheTableLibrary::Search(std::string_view query,
                                                          size_t limit) const {
    // Term extraction: lowercase words >= 2 chars (user call: "my" must find the
    // sermons that have it — the old 3-char floor silently dropped half of English).
    std::vector<std::string> terms;
    std::string cur;
    for (char c : Lower(std::string(query))) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            if (cur.size() >= 2) terms.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty() && cur.size() >= 2) terms.push_back(cur);
    if (terms.empty())
        return Error::Make(Err::InvalidArgument, kModule, "search needs a word");
    if (limit == 0) limit = 1;
    // Deduplicated, keeping the USER'S word order — the verbatim-phrase test must
    // honor what was typed ("then, friends" looks for then→friends, not a sorted
    // friends→then). Bitmask positions below come from this same vector, so the
    // dedupe keeps them collision-free.
    {
        std::vector<std::string> ordered;
        for (const std::string& t : terms)
            if (std::find(ordered.begin(), ordered.end(), t) == ordered.end()) ordered.push_back(t);
        terms = std::move(ordered);
    }

    // The SAME word resolution every other search surface uses (the engine's
    // ResolveTerms): incomplete words completed ("friend" -> "friends") and
    // typos corrected ("thn" -> "then", "frend" -> "friend") against the
    // sermon vocabulary. The RESOLVED words are what the paragraph scan below
    // hunts (they are what actually exists in the text); each raw spelling that
    // differs from its resolution rides along as an optional bonus word — a
    // paragraph containing BOTH the correction and the raw misspelling (a
    // verbatim quote of the typo) outranks one with the correction alone.
    std::vector<std::string> bonusWords;
    {
        auto resolved = search::SearchEngine::Instance().ResolveTermsWithSplits(terms);
        if (resolved.ok() && resolved.value().size() == terms.size()) {
            std::vector<std::string> fixed;
            fixed.reserve(terms.size() + 1);
            for (size_t i = 0; i < terms.size(); ++i) {
                const std::string& r = resolved.value()[i].term;
                // A jam-split entry ("holy spirit") becomes two real terms so the
                // paragraph bitmask can match them independently.
                std::istringstream ins(r.empty() ? terms[i] : r);
                std::string part;
                while (ins >> part) fixed.push_back(part);
                if (!r.empty() && r != terms[i] &&
                    std::find(bonusWords.begin(), bonusWords.end(), terms[i]) == bonusWords.end())
                    bonusWords.push_back(terms[i]);
            }
            terms = std::move(fixed);
        }
    }

    // WORD QUERIES SCAN EVERY SERMON (the b887811 behavior the user verified):
    // paragraph gems are invisible at sermon granularity. Engine-side candidate
    // picking — either Search()'s 48-doc fetch slice or a BM25 ranking of whole
    // sermons — drops the long, word-rich sermon whose ¶7 opens with the verbatim
    // query (47-0412 "Then, friends," went missing twice this way), and the
    // paragraph scan's +500 phrase bonus can never rescue a sermon that was never
    // scanned. The per-query lag that once forced candidate picking is gone: the
    // lowered paragraphs are CACHED per sermon (lowerParagraphs_), so the expensive
    // part (Lower() of ~100MB) happens once ever, not per query.

    // A citation-CODE query ("47-", "47-0412", "63-0628e") is naming a SERMON,
    // not searching paragraph words — the codes live in the chapter metadata and
    // never in the paragraph text, so the word scan below can never answer them
    // (Quick search's "47-1100X" showed only Bible rows). Strictly code-shaped
    // queries (digits/dash/letters only, at least one digit) ALSO scan the codes
    // themselves; anything with other punctuation ("revelation 1:1") is not.
    const bool codeShaped = [&query] {
        bool digits = false;
        for (char c : query) {
            if (std::isdigit(static_cast<unsigned char>(c))) digits = true;
            else if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ' '))
                return false;
        }
        return digits;
    }();
    struct CodeCand { const TheTableBook* b; const TheTableChapter* ch; int score; };
    std::vector<CodeCand> codeHits;
    // ONE pointer phase under ONE lock pair: the code gather, the paragraph
    // walk, the sort, the per-sermon cap and the materialization all store or
    // read &book/&ch/&verse into books_ — every one of those pointers is only
    // valid while nobody clears or grows books_, so the pair holds until the
    // last read (released just before return). Nothing in between blocks.
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (codeShaped) {
        std::string norm;   // "47 - 0412" and "47-0412" are the same code
        for (char c : Lower(std::string(query)))
            if (c != ' ') norm.push_back(c);
        // A bare "47" or "7" stays a verse/year query (Genesis 47, Luke 1:47);
        // the CODE shape needs a dash ("47-") or a full date-code ("0412").
        const bool dash = norm.find('-') != std::string::npos;
        const size_t digitCount = std::count_if(norm.begin(), norm.end(),
                                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (!norm.empty() && (dash || digitCount >= 3)) {
            for (const TheTableBook& book : books_)
                for (const TheTableChapter& ch : book.chapters) {
                    const std::string bare = Lower(ch.code.empty() ? CodeFromTitle(ch.title) : ch.code);
                    // The typed form is the CITATION shape — "47-0412" (yy-code),
                    // possibly with the spoken letter ("47-1100x"). A code may also
                    // live only in the TITLE ("1100x Fellowship" — no file code),
                    // so the leading code-like token of the title is a candidate
                    // too. The needle is compared to every candidate form.
                    std::string yy = book.name;
                    if (yy.size() == 4 && yy.compare(0, 2, "19") == 0) yy = yy.substr(2);
                    std::string head;   // leading [0-9]+[a-z]? of the stored title
                    for (char c : ch.title) {
                        if (std::isdigit(static_cast<unsigned char>(c))) { head.push_back(c); continue; }
                        if ((std::isalpha(static_cast<unsigned char>(c)) && !head.empty()
                             && std::all_of(head.begin(), head.end(),
                                            [](char h) { return std::isdigit(static_cast<unsigned char>(h)); })))
                            head.push_back(c);   // one trailing letter ("1100x")
                        break;
                    }
                    const std::string cands[4] = { yy + "-" + bare, bare, yy + "-" + head, head };
                    int score = 0;
                    for (const std::string& cand : cands) {
                        if (cand.empty()) continue;
                        if (cand == norm)
                            score = std::max(score, 2000);   // the exact sermon
                        else if (cand.rfind(norm, 0) == 0 || norm.rfind(cand, 0) == 0)
                            score = std::max(score, 1500);   // "47-" lists every 47-*; "47-1100x" = its "47-1100"
                        else if (cand.find(norm) != std::string::npos ||
                                 norm.find(cand) != std::string::npos)
                            score = std::max(score, 1000);   // "0412" in "47-0412"
                    }
                    if (score != 0) codeHits.push_back({&book, &ch, score});
                }
        }
    }

    // One pass over EVERY sermon (reading order = the sermonRank tiebreak below).

    // One sermon's paragraphs against the terms. TWO match shapes (user call: the
    // words need not sit in ONE paragraph — "it can be in a paragraph or max two"):
    //   exact     — every term in the one paragraph (the original rule, best rank);
    //   spanned   — every term across the paragraph and its NEXT one together
    //               (neither alone has all of them). The hit reports the FIRST of
    //               the pair, so picking it opens at where the passage starts.
    // Ranking (user calls: "words that fully match first before words that partly
    // match"; the para-7 probe): within a shape, VERBATIM phrase presence adds a
    // large bonus (the paragraph really says what was typed), then whole-word hits
    // outrank partial ones. `sermonRank` keeps the engine's candidate order as the
    // sermon-level tiebreak.
    struct Acc { const TheTableVerse* v; const TheTableBook* b; const TheTableChapter* ch; int score; bool spanned; int sermonRank; };
    std::vector<Acc> hits;
    auto scanChapter = [&](const TheTableBook& book, const TheTableChapter& ch, int sermonRank) {
        // The lowered paragraphs, computed once per sermon and kept (the fallback
        // scan re-lowercast the whole library per query — the lag this module's
        // rewrite removed, back for every short-word query without this cache).
        const std::string key = book.id + ":" + std::to_string(ch.number);
        auto cached = lowerParagraphs_.find(key);
        if (cached == lowerParagraphs_.end()) {
            std::vector<std::string> low;
            low.reserve(ch.verses.size());
            for (const TheTableVerse& v : ch.verses) low.push_back(Lower(v.text));
            cached = lowerParagraphs_.emplace(std::move(key), std::move(low)).first;
        }
        const std::vector<std::string>& hay = cached->second;
        const size_t n = ch.verses.size();
        std::vector<unsigned> bm(n, 0u);   // per-paragraph term-hit bitmask (substring)
        std::vector<unsigned> wm(n, 0u);   // of those, whole-word hits
        for (size_t i = 0; i < n; ++i) {
            unsigned m = 0, w = 0;
            for (size_t t = 0; t < terms.size(); ++t) {
                if (hay[i].find(terms[t]) == std::string::npos) continue;
                m |= (1u << t);
                // Whole word AND word-start extension count FULL ("friend" in
                // "friends" is the completion shape — the user's word plus an
                // ending, exactly what they meant); only a mid-word substring
                // ("friend" in "boyfriend") is the weak partial.
                if (ContainsWord(hay[i], terms[t]) || StartsWord(hay[i], terms[t])) w |= (1u << t);
            }
            bm[i] = m;
            wm[i] = w;
        }
        // Optional bonus words (the user's raw spellings when they differed from
        // the resolution): a paragraph that verbatim-quotes the misspelling —
        // the user was quoting something — outranks one that only has the fix.
        std::vector<unsigned> bx(n, 0u);
        for (size_t i = 0; i < n && !bonusWords.empty(); ++i) {
            unsigned b = 0;
            for (size_t t = 0; t < bonusWords.size() && t < 32; ++t)
                if (ContainsWord(hay[i], bonusWords[t])) b |= (1u << t);
            bx[i] = b;
        }
        auto bonusOf = [&](size_t i, size_t j) {
            if (bonusWords.empty()) return 0;
            unsigned m = (j < n ? bx[i] | bx[j] : bx[i]);
            int c = 0;
            for (; m; m &= m - 1) ++c;
            return c;
        };
        auto quality = [&](unsigned m, unsigned w) {
            int q = 0;
            for (size_t t = 0; t < terms.size() && t < 32; ++t) {
                if (m & (1u << t)) ++q;
                if (w & (1u << t)) ++q;   // a whole word counts twice
            }
            return q;
        };
        const unsigned all = terms.size() >= 32 ? 0xFFFFFFFFu : ((1u << terms.size()) - 1u);
        for (size_t i = 0; i < n; ++i)
            if (bm[i] == all)
                hits.push_back({&ch.verses[i], &book, &ch,
                                1000 + quality(bm[i], wm[i])
                                    + (ContainsPhrase(hay[i], terms) ? 500 : 0)
                                    + 20 * bonusOf(i, n),
                                false, sermonRank});
        for (size_t i = 0; i + 1 < n; ++i) {
            const unsigned pair = bm[i] | bm[i + 1];
            // (a pair whose halves already qualify alone was pushed above — only
            // genuinely spread-out matches land here)
            if (pair == all && bm[i] != all && bm[i + 1] != all) {
                const std::string joined = hay[i] + ' ' + hay[i + 1];
                hits.push_back({&ch.verses[i], &book, &ch,
                                quality(pair, wm[i] | wm[i + 1])
                                    + (ContainsPhrase(joined, terms) ? 500 : 0)
                                    + 20 * bonusOf(i, i + 1),
                                true, sermonRank});
            }
        }
    };
    // (Still inside the pointer-phase pair opened above the code gather —
    // the walk's original separate scope is what left the sort/cap/materialize
    // reading dangling pointers after an import or Load slipped in between.)
    int rank = 0;
    for (const TheTableBook& book : books_)
        for (const TheTableChapter& ch : book.chapters)
            scanChapter(book, ch, rank++);
    // ONE order for both paths: score first (verbatim-phrase bonus > scattered
    // words), the engine's candidate order as the sermon-level tiebreak, then
    // reading order. Then the per-sermon variety cap — a long sermon matching a
    // common-word query hundreds of times must not flood the list and push other
    // sermons' (often better) matches past the display limit (the para-7 probe).
    std::stable_sort(hits.begin(), hits.end(), [](const Acc& a, const Acc& b) {
        if (a.spanned != b.spanned) return !a.spanned;
        if (a.score != b.score) return a.score > b.score;
        if (a.sermonRank != b.sermonRank) return a.sermonRank < b.sermonRank;
        return a.v->number < b.v->number;
    });
    {
        constexpr size_t kPerSermonCap = 3;
        // Code hits bypass the cap (one row per sermon — the sermon IS the answer;
        // a "47-" listing must show every 47 sermon, not 3).
        std::map<std::pair<const void*, int>, size_t> perSermon;
        for (const CodeCand& c : codeHits) ++perSermon[{static_cast<const void*>(c.b), c.ch->number}];
        std::vector<Acc> kept;
        kept.reserve(std::min(hits.size(), limit * 4));
        for (const Acc& h : hits) {
            if (codeShaped && perSermon.count({static_cast<const void*>(h.b), h.ch->number})) continue;
            size_t& n = perSermon[{static_cast<const void*>(h.b), h.ch->number}];
            if (n >= kPerSermonCap) continue;
            ++n;
            kept.push_back(h);
        }
        hits = std::move(kept);
    }
    if (hits.size() > limit) hits.resize(limit);

    std::vector<TheTableSearchHit> out;
    out.reserve(hits.size());
    for (const Acc& h : hits) {
        TheTableSearchHit s;
        // The citation line, the sermon-citation way ("47-0412 - Faith Is The
        // Substance 3"), not the scripture-shaped "1953 2:1".
        s.reference = SermonCitation(h.b->name,
                                     h.ch->code.empty() ? CodeFromTitle(h.ch->title) : h.ch->code,
                                     h.ch->title, h.v->number);
        s.bookId = h.b->id;
        s.chapter = h.ch->number;
        s.verse = h.v->number;
        // THE GLOW RULE (The Table's side): the snippet is anchored where the
        // words actually sit, not at the paragraph head — a 400-word paragraph
        // whose match is at word 300 put the match past the visible two lines
        // and the yellow highlight was swallowed. Anchor = the EARLIEST query
        // word in the paragraph (the passage's start; a couple of bounded finds
        // per SHOWN row, nothing per candidate).
        {
            const std::string low = Lower(h.v->text);
            // THE GLOW RULE (The Table's side), phrase first: when the paragraph
            // holds the query as a VERBATIM phrase ("Then, friends, isn't…"),
            // anchor THERE — the earliest-word rule picked a lone "And then you
            // watch It" 1,000 chars before the real match, so the row that the
            // +500 phrase bonus ranked FIRST showed a snippet with a single
            // glowable word while the very words that ranked it sat past the
            // clip. Non-phrase hits keep the earliest-word anchor (the passage's
            // start; a couple of bounded finds per SHOWN row, nothing per
            // candidate).
            size_t anchor = std::string::npos;
            if (!(ContainsPhrase(low, terms, &anchor) && anchor != std::string::npos)) {
                anchor = std::string::npos;
                for (const auto& t : terms) {
                    const size_t at = low.find(t);
                    if (at != std::string::npos && at < anchor) anchor = at;
                }
            }
            if (anchor == std::string::npos) anchor = 0;
            const size_t start = anchor > 60 ? anchor - 60 : 0;
            std::string snip = h.v->text.substr(start, 220);
            if (start > 0) {   // trim a leading partial word
                const size_t sp = snip.find(' ');
                if (sp != std::string::npos && sp < 40) snip = snip.substr(sp + 1);
            }
            s.snippet = std::move(snip);
        }
        s.score = static_cast<double>(h.score);
        s.spanned = h.spanned;
        out.push_back(std::move(s));
    }
    // Code matches lead: a sermon-CODE query ("47-", "47-0412") is naming the
    // sermon itself, so each matching sermon gets ONE sermon-level row (verse 0,
    // "chapter 1" opening) ahead of any paragraph word-hits. Sermon order = the
    // library's reading order; scores rank which rows lead within the code tiers.
    if (!codeHits.empty()) {
        std::stable_sort(codeHits.begin(), codeHits.end(),
                         [](const CodeCand& a, const CodeCand& b) { return a.score > b.score; });
        std::vector<TheTableSearchHit> leading;
        leading.reserve(codeHits.size());
        for (const CodeCand& c : codeHits) {
            if (c.ch->verses.empty()) continue;
            TheTableSearchHit s;
            s.reference = SermonCitation(c.b->name,
                                         c.ch->code.empty() ? CodeFromTitle(c.ch->title) : c.ch->code,
                                         c.ch->title, 0);
            s.bookId = c.b->id;
            s.chapter = c.ch->number;
            s.verse = 0;
            s.snippet = c.ch->verses.front().text.substr(0, 220);
            s.score = static_cast<double>(c.score);
            s.spanned = false;
            leading.push_back(std::move(s));
        }
        out.insert(out.begin(), leading.begin(), leading.end());
    }
    // The pointer-phase pair (declared above the code gather) releases here at
    // function exit — every Acc/CodeCand pointer is already materialized.
    return out;
}

// ---------------------------------------------------------------------------
// Import ("New sermon": .txt or .pdf)
// ---------------------------------------------------------------------------
TheTableBook& TheTableLibrary::BookForYear(std::string_view year) {
    for (TheTableBook& book : books_)
        if (book.name == year) return book;
    TheTableBook book;
    book.id = "Y" + std::string(year);
    book.name = std::string(year);
    int maxOrder = 0;
    for (const TheTableBook& b : books_) maxOrder = std::max(maxOrder, b.order);
    book.order = maxOrder + 1;
    books_.push_back(std::move(book));
    return books_.back();
}

// Step one of an import: whatever the file is (.pdf or .txt) becomes clean UTF-8 text. Nothing is stored here.
Result<std::string> TheTableLibrary::ConvertToCleanText(std::string_view fileName, std::string_view content) {
    const std::string data(content);
    std::string text;
    if (Lower(std::string(fileName)).ends_with(".pdf")) {
        if (data.size() < 8 || data.substr(0, 5) != "%PDF-")
            return Error::Make(kTableErr, kModule, "not a PDF file");
        text = CleanSermonText(SplitCodedParagraphs(StripSpokenWordBanner(TidyMarkers(TextFromPdf(data)))));
        if (text.empty())
            return Error::Make(kTableErr, kModule,
                               "no text could be read from the PDF (a scan with no text layer, or fonts this reader cannot decode)");
    } else {
        text = CleanSermonText(data);
        if (text.empty()) return Error::Make(kTableErr, kModule, "the file is empty");
    }
    return text;
}

Result<std::string> TheTableLibrary::ImportSermon(std::string_view fileName, std::string_view content) {
    auto clean = ConvertToCleanText(fileName, content);
    if (!clean.ok()) return clean.error();
    auto added = AddCleanText(fileName, clean.value());
    if (!added.ok()) return added;
    if (auto saved = Save(); !saved.ok())
        Logger::Instance().Warning("TheTableLibrary save failed: " + saved.error().message, kModule);
    return added;
}

// Step two: clean text -> paragraphs -> a chapter of the sermon's year.
Result<std::string> TheTableLibrary::AddCleanText(std::string_view fileName, const std::string& text, bool replaceUnreadable) {
    const std::string name(fileName);
    auto paragraphs = ParagraphsFromText(text);
    if (paragraphs.empty())
        return Error::Make(kTableErr, kModule, "no paragraphs found in the sermon");

    const Placement place = PlacementOf(name);
    const std::string& title = place.title;

    // Import mutates the topology the scan walks — same pair as Load.
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    TheTableBook& book = BookForYear(place.year);
    TheTableChapter ch;
    ch.number = static_cast<int>(book.chapters.size()) + 1;
    ch.title = title;
    ch.code = CodeOf(name);
    const auto numbered = NumberParagraphs(paragraphs);
    for (size_t i = 0; i < numbered.size(); ++i) {
        TheTableVerse v;
        v.number = numbered[i].number;
        v.text = numbered[i].text;
        if (i == 0) v.heading = title;
        ch.verses.push_back(std::move(v));
    }
    // The same sermon stored before by a reader that could not decode its fonts: put the readable text in its place.
    if (replaceUnreadable) {
        for (TheTableChapter& existing : book.chapters) {
            if (existing.title == title && !ChapterReadable(existing)) {
                existing.verses = std::move(ch.verses);
                lowerParagraphs_.erase(book.id + ":" + std::to_string(existing.number));
                return book.name + " " + std::to_string(existing.number) + ":1";
            }
        }
    }
    book.chapters.push_back(std::move(ch));

    return book.name + " " + std::to_string(book.chapters.size()) + ":1";
}

// ---------------------------------------------------------------------------
// Folder import: every .pdf/.txt under a root, same mapping as ImportSermon.
// ---------------------------------------------------------------------------
Result<TheTableLibrary::ImportReport> TheTableLibrary::ImportFolder(
    std::string_view folderPath,
    const std::function<void(int, int, std::string_view)>& progress,
    std::string_view convertedRoot) {
    auto& platform = platform::PlatformAccessor::Get();
    // One walk of the tree. The extension test ignores case ("SERMON.PDF" is a sermon too - FindFiles' own extension filter is
    // case-sensitive, so it is not used).
    auto found = platform.Filesystem().FindFiles(folderPath, "");
    if (!found.ok()) return Error::Make(kTableErr, kModule, found.error().message);
    std::vector<std::string> files;
    for (const std::string& path : found.value()) {
        const std::string lower = Lower(path);
        if (lower.ends_with(".pdf") || lower.ends_with(".txt")) files.push_back(path);
    }
    std::sort(files.begin(), files.end());   // deterministic order (year, then code)
    ImportReport report;

    int done = 0;
    const int total = static_cast<int>(files.size());
    for (const std::string& path : files) {
        ++done;
        if (progress) progress(done, total, path);

        // Duplicate guard: the same year + sermon title is already in.
        const Placement place = PlacementOf(path);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool dup = false;
            for (const TheTableBook& b : books_) {
                if (b.name != place.year) continue;
                // (a sermon stored as garbage by an older reader is not "already in": it is read again and replaced)
                dup = std::any_of(b.chapters.begin(), b.chapters.end(), [&](const TheTableChapter& c) { return c.title == place.title && ChapterReadable(c); });
                break;
            }
            if (dup) { ++report.skipped; continue; }
        }

        // Step one, outside the lock: the file as clean text. A PDF's converted text is kept as a .txt under `convertedRoot` (the same
        // name, in its year's folder) so it can be opened and checked, and is reused while it is newer than the PDF.
        auto& fs = platform.Filesystem();
        std::string clean;
        std::string keepAt;
        if (!convertedRoot.empty() && Lower(path).ends_with(".pdf")) {
            std::string stem = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
            stem = stem.substr(0, stem.find_last_of('.'));
            keepAt = fs.Join(fs.Join(fs.Join(convertedRoot, kConvertedVersion), place.year), stem + ".txt");
            auto kept = fs.Metadata(keepAt);
            auto source = fs.Metadata(path);
            if (kept.ok() && source.ok() && kept.value().modifiedEpochNs >= source.value().modifiedEpochNs) {
                if (auto text = fs.ReadText(keepAt); text.ok() && ReadableText(text.value())) clean = std::move(text.value());
            }
        }
        if (clean.empty()) {
            auto bytes = fs.ReadBinary(path);
            if (!bytes.ok()) {
                ++report.failed;
                Logger::Instance().Info("Table import: could not read " + path + ": " + bytes.error().message, kModule);
                continue;
            }
            auto converted = ConvertToCleanText(path, std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
            if (!converted.ok()) {
                ++report.failed;
                Logger::Instance().Info("Table import: " + path + ": " + converted.error().message, kModule);
                continue;
            }
            clean = std::move(converted.value());
            if (!keepAt.empty()) {
                const size_t cut = keepAt.find_last_of("/\\");
                (void)fs.CreateDirectories(keepAt.substr(0, cut));
                if (auto wrote = fs.Write(keepAt, clean + "\n"); !wrote.ok())
                    Logger::Instance().Info("Table import: could not keep the converted text " + keepAt + ": " + wrote.error().message, kModule);
            }
        }
        // Step two: the clean text into the library.
        auto r = AddCleanText(path, clean, /*replaceUnreadable=*/true);
        if (r.ok()) {
            ++report.imported;
        } else {
            ++report.failed;
            Logger::Instance().Info("Table import: " + path + ": " + r.error().message, kModule);
        }
        // Saved every so often and once at the end - not after every sermon. Rewriting a library of hundreds of sermons for each one
        // is what made a big folder crawl.
        if (r.ok() && report.imported % 250 == 0) (void)Save();
    }
    if (report.imported > 0) {
        if (auto saved = Save(); !saved.ok())
            Logger::Instance().Warning("TheTableLibrary save failed: " + saved.error().message, kModule);
    }
    return report;
}

} // namespace bps::library
