#include "modules/library/TheTableLibrary.hpp"

#include "core/config/Json.hpp"
#include "core/logging/Logger.hpp"
#include "core/recovery/DocumentRecovery.hpp"
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
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
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

template <size_t N>
void AddWordBloom(std::array<uint64_t, N>& bloom, std::string_view word) {
    const uint64_t h1 = static_cast<uint64_t>(std::hash<std::string_view>{}(word));
    uint64_t h2 = (h1 >> 29) ^ (h1 << 17) ^ 0x9e3779b97f4a7c15ull;
    h2 |= 1ull;
    constexpr uint64_t bitCount = N * 64;
    for (uint64_t i = 0; i < 4; ++i) {
        const uint64_t bit = (h1 + i * h2) % bitCount;
        bloom[bit / 64] |= uint64_t{1} << (bit % 64);
    }
}

template <size_t N>
bool MightHaveWord(const std::array<uint64_t, N>& bloom, std::string_view word) {
    const uint64_t h1 = static_cast<uint64_t>(std::hash<std::string_view>{}(word));
    uint64_t h2 = (h1 >> 29) ^ (h1 << 17) ^ 0x9e3779b97f4a7c15ull;
    h2 |= 1ull;
    constexpr uint64_t bitCount = N * 64;
    for (uint64_t i = 0; i < 4; ++i) {
        const uint64_t bit = (h1 + i * h2) % bitCount;
        if ((bloom[bit / 64] & (uint64_t{1} << (bit % 64))) == 0)
            return false;
    }
    return true;
}

// HOW TIGHTLY A PARAGRAPH SAYS WHAT WAS TYPED. The length, in WORDS, of the shortest stretch of `words` that holds the
// query's terms IN THE ORDER TYPED (other words may sit between them) — "i preach what paul preach" in "…if I preach to you
// just what Paul preached…" is 8 words. When no stretch has them in order, the shortest stretch holding every term in any
// order, plus a penalty. 0 when a term is missing. Every paragraph that holds all the words used to score the same, so a
// sentence that really says it tied with paragraphs where the words were a page apart and the winner was whichever sermon
// came first.
size_t TightSpan(const std::vector<std::string_view>& words, const std::vector<std::string>& terms, bool whole) {
    if (terms.size() < 2 || words.empty()) return 0;
    const auto matches = [whole](std::string_view w, const std::string& t) {
        return whole ? w == t : (w.size() >= t.size() && w.compare(0, t.size(), t) == 0);
    };
    size_t best = 0;
    for (size_t s = 0; s < words.size(); ++s) {   // in the order typed: the earliest next word each time is the tightest
        if (!matches(words[s], terms[0])) continue;
        size_t last = s, k = 1;
        for (size_t w = s + 1; w < words.size() && k < terms.size(); ++w)
            if (matches(words[w], terms[k])) { ++k; last = w; }
        if (k == terms.size()) {
            const size_t span = last - s + 1;
            if (best == 0 || span < best) best = span;
        }
    }
    if (best != 0) return best;
    std::vector<const std::string*> distinct;   // any order: a sliding window over the distinct terms
    for (const std::string& t : terms)
        if (std::none_of(distinct.begin(), distinct.end(), [&](const std::string* d) { return *d == t; }))
            distinct.push_back(&t);
    std::vector<int> have(distinct.size(), 0);
    size_t covered = 0, left = 0;
    for (size_t right = 0; right < words.size(); ++right) {
        for (size_t d = 0; d < distinct.size(); ++d)
            if (matches(words[right], *distinct[d]) && have[d]++ == 0) ++covered;
        while (covered == distinct.size()) {
            const size_t span = right - left + 1;
            if (best == 0 || span < best) best = span;
            for (size_t d = 0; d < distinct.size(); ++d)
                if (matches(words[left], *distinct[d]) && --have[d] == 0) --covered;
            ++left;
        }
    }
    constexpr size_t kOutOfOrder = 8;
    return best == 0 ? 0 : best + kOutOfOrder;
}
// Score points for a span: 800 for the terms side by side, falling by 8 for every extra word between them.
int TightBonus(size_t span, size_t termCount) {
    if (span == 0 || span < termCount) return 0;
    return static_cast<int>(std::max<long>(0, 800L - 8L * static_cast<long>(span - termCount)));
}

size_t NearPhrasePosition(std::string_view text, const std::vector<std::string>& terms,
                          size_t maxGapWords) {
    if (terms.empty()) return std::string_view::npos;
    std::vector<std::string_view> words;
    std::vector<size_t> starts;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isalnum(c) && c < 0x80) { ++i; continue; }
        const size_t start = i++;
        while (i < text.size()) {
            const unsigned char next = static_cast<unsigned char>(text[i]);
            if (!std::isalnum(next) && next < 0x80) break;
            ++i;
        }
        words.emplace_back(text.substr(start, i - start));
        starts.push_back(start);
    }
    for (size_t start = 0; start < words.size(); ++start) {
        if (words[start] != terms.front()) continue;
        size_t pos = start;
        bool matched = true;
        for (size_t t = 1; t < terms.size(); ++t) {
            size_t next = pos + 1;
            while (next < words.size() && next <= pos + maxGapWords + 1
                   && words[next] != terms[t]) ++next;
            if (next >= words.size() || next > pos + maxGapWords + 1) {
                matched = false;
                break;
            }
            pos = next;
        }
        if (matched) return starts[start];
    }
    return std::string_view::npos;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

using textmatch::Lower;   // shared with the Search Engine (see TextMatching.hpp)
using textmatch::ContainsWord;
using textmatch::ContainsPhrase;
using textmatch::ContainsNearPhrase;
using textmatch::StartsWord;

int ParagraphDesignator(std::string_view query) {
    std::string s = Trim(std::string(query));
    if (s.empty()) return 0;
    const std::string lower = Lower(s);
    // Longest marker first, so "paragraph" is not eaten by "para" or "p".
    static const char* const kMarkers[] = { "paragraph", "para.", "para", "¶", "p.", "p", "#" };
    for (const char* marker : kMarkers) {
        const size_t len = std::char_traits<char>::length(marker);
        if (lower.rfind(marker, 0) != 0) continue;
        s = Trim(s.substr(len));
        break;
    }
    // A separator a writer may put between the marker and the number ("p.50",
    // "¶ 50", "p:50").
    while (!s.empty() && (s.front() == '.' || s.front() == ':' || s.front() == '#'))
        s = Trim(s.substr(1));
    // Up to three digits: a paragraph is numbered in the hundreds at most, while a
    // YEAR ("1959") must keep searching the text even inside a locked search.
    if (s.empty() || s.size() > 3) return 0;
    if (!std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return 0;
    const int n = std::atoi(s.c_str());
    return n > 0 ? n : 0;
}

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

std::string CodeFromTitle(const std::string& title) {
    size_t sp = title.find(' ');
    std::string head = sp == std::string::npos ? title : title.substr(0, sp);
    if (!head.empty()) {
        size_t digits = 0;
        while (digits < head.size() && std::isdigit(static_cast<unsigned char>(head[digits])))
            ++digits;
        const bool numeric = digits == head.size();
        const bool suffixed = digits + 1 == head.size()
            && std::isalpha(static_cast<unsigned char>(head.back()));
        if (digits > 0 && (numeric || suffixed))
            return head;
    }
    return {};
}

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
    std::string display = clean;
    if (!code.empty() && !display.empty()) {
        const size_t sp = display.find(' ');
        if (sp != std::string::npos && sp == code.size()) {
            bool same = true;
            for (size_t i = 0; i < sp && same; ++i)
                same = std::tolower(static_cast<unsigned char>(display[i]))
                       == std::tolower(static_cast<unsigned char>(code[i]));
            if (same) display = display.substr(sp + 1);
        }
    }
    if (display.empty()) display = clean;
    std::string out = yy;
    if (!code.empty()) out += "-" + code;
    if (!display.empty()) out += " - " + display;
    if (verse > 0) out += " " + std::to_string(verse);
    return out;
}

std::string CitationNeedleText(std::string_view handle)
{
    const std::string in = Trim(std::string(handle));
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) ++j;
            const bool beforeDash = j < in.size() && in[j] == '-';
            const bool afterDash = !out.empty() && out.back() == '-';
            if (beforeDash || afterDash)
                continue;   // the run beside the dash: dropped whole
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return Trim(out);
}

constexpr const char* kUnfiledYear = "Unfiled";

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

struct NumberedParagraph {
    int number = 0;
    std::string text;
};

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
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!VerseKnownLocked(bookId, chapter, verse))
            return Error::Make(Err::NotFound, kModule, "no such paragraph");

        TableNote note;
        note.id = std::format("note-{}", NowMs());
        note.ref = CitationLocked(bookId, chapter, verse);
        note.bookId = std::string(bookId);
        note.chapter = chapter;
        note.verse = verse;
        note.text = text;
        note.createdMs = note.modifiedMs = std::max(NowMs(), lastNoteMs_ + 1);
        lastNoteMs_ = note.modifiedMs;
        notes_[NoteKey(bookId, chapter, verse)] = std::move(note);
    }
    return Save();
}

Result<void> TheTableLibrary::RemoveNote(std::string_view bookId, int chapter, int verse) {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (notes_.erase(NoteKey(bookId, chapter, verse)) == 0)
            return Ok();   // nothing stored: already gone, nothing to write
    }
    return Save();
}

Result<std::vector<TableNote>> TheTableLibrary::Notes(std::string_view bookId, int chapter,
                                                      int verse) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = notes_.find(NoteKey(bookId, chapter, verse));
    if (it == notes_.end())
        return std::vector<TableNote>{};
    return std::vector<TableNote>{ it->second };
}

Result<std::vector<TableNote>> TheTableLibrary::Notes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!VerseKnownLocked(bookId, chapter, verse))
            return Error::Make(Err::NotFound, kModule, "no such paragraph");
        highlights_[NoteKey(bookId, chapter, verse)] = true;
    } else {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        highlights_.erase(NoteKey(bookId, chapter, verse));
    }
    return Save();
}

Result<std::vector<std::string>> TheTableLibrary::Highlights() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(highlights_.size());
    for (const auto& [key, on] : highlights_) {
        (void)on;
        out.push_back(key);
    }
    return out;
}

Result<bool> TheTableLibrary::IsHighlighted(std::string_view bookId, int chapter, int verse) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
        std::shared_lock<std::shared_mutex> lock(mutex_);
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
                vo["text"] = J::String(v.text.str());
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
    return platform.Filesystem().WriteAtomic(filePath_, J(std::move(root)).ToString());
}

namespace {
// A forward scanner over JSON text that hands out each member / element as a SLICE of the text — no tree for the whole
// document. The library file is ~100 MB: a tree of it (hundreds of thousands of small nodes) peaked at several hundred MB
// and the process never gave that memory back. Parsing one sermon at a time keeps the peak near the final size.
struct JsonSlicer {
    std::string_view s;
    size_t p = 0;
    bool bad = false;
    void Ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }
    void SkipString() {   // at the opening quote
        ++p;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\') ++p;
            ++p;
        }
        if (p >= s.size()) { bad = true; return; }
        ++p;
    }
    // The text of the next value (a string, a number/literal, or a whole object/array), advancing past it.
    std::string_view Take() {
        Ws();
        if (p >= s.size()) { bad = true; return {}; }
        const size_t start = p;
        const char c = s[p];
        if (c == '"') {
            SkipString();
        } else if (c == '{' || c == '[') {
            int depth = 0;
            while (p < s.size()) {
                const char d = s[p];
                if (d == '"') { SkipString(); if (bad) return {}; continue; }
                if (d == '{' || d == '[') ++depth;
                else if (d == '}' || d == ']') --depth;
                ++p;
                if (depth == 0) break;
            }
            if (depth != 0) { bad = true; return {}; }
        } else {
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']'
                   && s[p] != ' ' && s[p] != '\n' && s[p] != '\r' && s[p] != '\t')
                ++p;
        }
        if (bad) return {};
        return s.substr(start, p - start);
    }
};

// f(key, valueText) for every member of the object text `obj`; false when it is not a well-formed object (or f says stop).
template <class F>
bool EachMember(std::string_view obj, F&& f) {
    JsonSlicer c{obj};
    c.Ws();
    if (c.p >= obj.size() || obj[c.p] != '{') return false;
    ++c.p;
    c.Ws();
    if (c.p < obj.size() && obj[c.p] == '}') return true;
    for (;;) {
        const std::string_view keyText = c.Take();
        if (c.bad || keyText.size() < 2 || keyText.front() != '"') return false;
        c.Ws();
        if (c.p >= obj.size() || obj[c.p] != ':') return false;
        ++c.p;
        const std::string_view value = c.Take();
        if (c.bad || value.empty()) return false;
        std::string key;
        if (keyText.find('\\') == std::string_view::npos) {
            key.assign(keyText.substr(1, keyText.size() - 2));
        } else {
            auto k = json::Parse(keyText);
            if (!k.ok()) return false;
            key = std::string(k.value().asString());
        }
        if (!f(std::string_view(key), value)) return false;
        c.Ws();
        if (c.p < obj.size() && obj[c.p] == ',') { ++c.p; continue; }
        return c.p < obj.size() && obj[c.p] == '}';
    }
}

// f(elementText) for every element of the array text `arr`.
template <class F>
bool EachElement(std::string_view arr, F&& f) {
    JsonSlicer c{arr};
    c.Ws();
    if (c.p >= arr.size() || arr[c.p] != '[') return false;
    ++c.p;
    c.Ws();
    if (c.p < arr.size() && arr[c.p] == ']') return true;
    for (;;) {
        const std::string_view value = c.Take();
        if (c.bad || value.empty()) return false;
        if (!f(value)) return false;
        c.Ws();
        if (c.p < arr.size() && arr[c.p] == ',') { ++c.p; continue; }
        return c.p < arr.size() && arr[c.p] == ']';
    }
}

std::string TextOf(std::string_view valueText) {
    auto v = json::Parse(valueText);
    return v.ok() ? std::string(v.value().asString()) : std::string();
}
long long IntOf(std::string_view valueText) {
    auto v = json::Parse(valueText);
    return v.ok() ? v.value().asInt() : 0;
}

// ---- The sermon text as a memory-mapped file -----------------------------------------------------------------------
// ~95 MB of paragraphs used to live as 225,000 private heap strings. Load writes them to one file beside the library
// and maps it read-only: the operating system pages it in for a search or a read and out again under memory pressure,
// and it is not private memory of the process. THE FILE IS A SWAP, NOT A SECOND COPY OF THE LIBRARY: the library file
// stays the one source of truth, this one is rebuilt on every Load and removed with the last verse that uses it (and a
// crash's leftovers are removed by the next Load).
std::atomic<size_t> gMapThresholdBytes{4u << 20};   // a library file smaller than this keeps its text on the heap
std::atomic<uint64_t> gBlobSeq{0};

struct TextBlob {
    std::shared_ptr<const platform::IMappedFile> map;
    std::string path;
    ~TextBlob() {
        map.reset();   // the file cannot be removed while it is mapped
        // (std::filesystem, not the platform accessor: this can run while the process is shutting down)
        std::error_code ec;
        const std::u8string p(reinterpret_cast<const char8_t*>(path.data()), path.size());
        std::filesystem::remove(std::filesystem::path(p), ec);
    }
};

// One sermon's tree -> the library's own structure. The paragraph text MOVES out of the tree (no copy).
TheTableChapter ChapterFromJson(J& cv) {
    const auto text = [](J* v, std::string_view key) {
        J* f = v ? v->FindMut(key) : nullptr;
        return f ? f->takeString() : std::string();
    };
    const auto number = [](const J* v, std::string_view key) {
        const J* f = v ? v->Find(key) : nullptr;
        return static_cast<int>(f ? f->asInt() : 0);
    };
    TheTableChapter ch;
    ch.number = number(&cv, "number");
    ch.title = text(&cv, "title");
    // The code joined this schema later: older files carry none, and the title always opens with it when there is one.
    ch.code = text(&cv, "code");
    if (ch.code.empty()) ch.code = CodeFromTitle(ch.title);
    if (J* vs = cv.FindMut("verses"); vs && vs->asArrayMut()) {
        ch.verses.reserve(vs->asArrayMut()->size());
        for (J& vv : *vs->asArrayMut()) {
            TheTableVerse v;
            v.number = number(&vv, "number");
            v.text = text(&vv, "text");
            v.heading = text(&vv, "heading");
            // The heading is the sermon's title and only the first paragraph shows it (nothing reads it on the others):
            // an older file repeats it on every paragraph, which was a string per paragraph (~17 MB for the library).
            if (!ch.verses.empty() && v.heading == ch.title) v.heading.clear();
            if (!v.text.empty()) ch.verses.push_back(std::move(v));
        }
        ch.verses.shrink_to_fit();
    }
    return ch;
}
} // namespace

void TheTableLibrary::SetMapThresholdBytes(size_t bytes) { gMapThresholdBytes.store(bytes); }

Result<void> TheTableLibrary::Load() {
    auto& platform = platform::PlatformAccessor::Get();
    auto body = platform.Filesystem().ReadText(filePath_);

    bool damaged = false;
    std::string damage;
    const bool bodyOk = body.ok();
    std::vector<TheTableBook> loaded;
    std::map<std::string, TableNote> loadedNotes;
    std::map<std::string, bool> loadedHighlights;

    // The mapped text file (see TextBlob). `spans` has one (offset, length) per paragraph moved into it, in load order.
    std::unique_ptr<platform::IFileWriteStream> blobOut;
    std::string blobPath;
    uint64_t blobWritten = 0;
    bool blobFailed = false;
    std::vector<std::pair<uint64_t, uint32_t>> spans;
    if (bodyOk && body.value().size() >= gMapThresholdBytes.load()) {
        const size_t cut = filePath_.find_last_of("/\\");
        const std::string dir = cut == std::string::npos ? std::string(".") : filePath_.substr(0, cut);
        const std::string stem = (cut == std::string::npos ? filePath_ : filePath_.substr(cut + 1)) + ".text.";
        // A crash or a kill leaves its file behind: remove those (one still mapped by another process cannot be, and stays).
        if (auto entries = platform.Filesystem().Enumerate(dir); entries.ok()) {
            for (const std::string& entry : entries.value()) {
                const size_t slash = entry.find_last_of("/\\");
                const std::string name = slash == std::string::npos ? entry : entry.substr(slash + 1);
                if (name.rfind(stem, 0) == 0) (void)platform.Filesystem().Remove(entry);
            }
        }
        blobPath = filePath_ + ".text." + std::to_string(++gBlobSeq) + "-"
                   + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (auto opened = platform.Filesystem().OpenWriteStream(blobPath); opened.ok())
            blobOut = std::move(opened.value());
        else
            blobFailed = true;
    }

    if (bodyOk) {
        const std::string_view text = body.value();
        std::string_view booksText, notesText, highlightsText;
        const bool rootOk = EachMember(text, [&](std::string_view key, std::string_view value) {
            if (key == "books") booksText = value;
            else if (key == "notes") notesText = value;
            else if (key == "highlights") highlightsText = value;
            return true;
        });
        if (!rootOk) {
            damaged = true;
            damage = "the library file is not a well-formed JSON object";
        } else if (booksText.empty() || booksText.front() != '[') {
            damaged = true;
            damage = "\"books\" is missing or is not an array";
        } else {
            // THE LIBRARY IS READ ONE SERMON AT A TIME. The whole file used to be parsed into one tree and the text copied
            // out of it: a 107 MB library peaked at several hundred MB of small allocations and the idle app kept ~450 MB
            // of it. Now a sermon's own text is parsed, its paragraphs MOVE into the library, and its tree is gone before
            // the next sermon starts.
            const bool booksOk = EachElement(booksText, [&](std::string_view bookText) {
                TheTableBook book;
                std::string_view chaptersText;
                const bool memberOk = EachMember(bookText, [&](std::string_view key, std::string_view value) {
                    if (key == "id") book.id = TextOf(value);
                    else if (key == "name") book.name = TextOf(value);
                    else if (key == "order") book.order = static_cast<int>(IntOf(value));
                    else if (key == "chapters") chaptersText = value;
                    return true;
                });
                if (!memberOk) return false;
                if (book.id.empty()) return true;   // a book without an id is skipped, as before
                if (!chaptersText.empty() && chaptersText.front() == '[') {
                    const bool chaptersOk = EachElement(chaptersText, [&](std::string_view chapterText) {
                        auto parsed = json::Parse(chapterText);
                        if (!parsed.ok()) {
                            damage = parsed.error().message;
                            return false;
                        }
                        TheTableChapter ch = ChapterFromJson(parsed.value());
                        if (ch.verses.empty()) return true;
                        if (blobOut && !blobFailed) {
                            // This sermon's paragraphs go to the mapped file and their strings are let go right here,
                            // so the heap never holds more than one sermon's text at a time.
                            std::string chunk;
                            size_t need = 0;
                            for (const TheTableVerse& v : ch.verses) need += v.text.size();
                            chunk.reserve(need);
                            for (const TheTableVerse& v : ch.verses) chunk.append(v.text.view());
                            const auto w = blobOut->Write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
                            if (w.ok() && w.value() == chunk.size()) {
                                uint64_t at = blobWritten;
                                for (TheTableVerse& v : ch.verses) {
                                    spans.push_back({at, static_cast<uint32_t>(v.text.size())});
                                    at += v.text.size();
                                    v.text = VerseText();
                                }
                                blobWritten += chunk.size();
                            } else {
                                blobFailed = true;   // (the sermons already moved are read back from the file below)
                            }
                        }
                        book.chapters.push_back(std::move(ch));
                        return true;
                    });
                    if (!chaptersOk) return false;
                }
                book.chapters.shrink_to_fit();
                loaded.push_back(std::move(book));
                return true;
            });
            if (!booksOk) {
                damaged = true;
                if (damage.empty()) damage = "a book or sermon in the library file is malformed";
            }
        }
        // User data (older files carry none — both sections stay empty then).
        if (!damaged && !notesText.empty() && notesText.front() == '[') {
            (void)EachElement(notesText, [&](std::string_view noteText) {
                auto parsed = json::Parse(noteText);
                if (!parsed.ok()) return true;   // skip a damaged entry, keep the rest
                const J& nv = parsed.value();
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
                    return true;
                loadedNotes[NoteKey(note.bookId, note.chapter, note.verse)] = std::move(note);
                return true;
            });
        }
        if (!damaged && !highlightsText.empty() && highlightsText.front() == '[') {
            (void)EachElement(highlightsText, [&](std::string_view hlText) {
                const std::string key = TextOf(hlText);
                if (!key.empty()) loadedHighlights[key] = true;
                return true;
            });
        }
    }
    if (damaged) {
        loaded.clear();
        loaded.shrink_to_fit();
        (void)recovery::SetAsideDamagedDocument(filePath_, kModule, damage);
    }

    // Seal the mapped text file and point the paragraphs at it.
    if (blobOut) {
        (void)blobOut->Close();
        blobOut.reset();
        const auto eachVerse = [&](const auto& fn) {
            size_t i = 0;
            for (TheTableBook& b : loaded)
                for (TheTableChapter& c : b.chapters)
                    for (TheTableVerse& v : c.verses) {
                        if (i >= spans.size()) return;   // the rest still own their strings
                        fn(v, spans[i++]);
                    }
        };
        std::shared_ptr<const platform::IMappedFile> map;
        if (!damaged && !spans.empty() && !blobFailed) {
            if (auto m = platform.Filesystem().MapReadOnly(blobPath); m.ok() && m.value()->Size() == blobWritten)
                map = m.value();
        }
        if (map) {
            auto blob = std::make_shared<TextBlob>();
            blob->map = map;
            blob->path = blobPath;
            const std::shared_ptr<const void> keep = blob;
            eachVerse([&](TheTableVerse& v, const std::pair<uint64_t, uint32_t>& s) {
                v.text = VerseText::Borrowed(std::string_view(map->Data() + s.first, s.second), keep);
            });
            Logger::Instance().Info(std::format("The Table text kept in a mapped file ({} paragraphs, {} MB)",
                                                spans.size(), blobWritten >> 20), kModule);
        } else {
            if (!damaged && !spans.empty()) {
                // Mapping (or a write) failed after some sermons had already moved out: give them their strings back.
                auto bytes = platform.Filesystem().ReadBinary(blobPath);
                if (!bytes.ok() || bytes.value().size() < blobWritten)
                    return Error::Make(Err::IoError, kModule, "the library's text could not be kept in memory or in a file");
                const std::vector<uint8_t>& all = bytes.value();
                eachVerse([&](TheTableVerse& v, const std::pair<uint64_t, uint32_t>& s) {
                    v.text = std::string(reinterpret_cast<const char*>(all.data()) + s.first, s.second);
                });
            }
            std::error_code ec;
            const std::u8string p(reinterpret_cast<const char8_t*>(blobPath.data()), blobPath.size());
            std::filesystem::remove(std::filesystem::path(p), ec);
        }
    }

    // The raw ~100 MB text is one big block: freeing it hands it straight back to the system.
    { auto release = std::move(body); (void)release; }

    std::lock_guard<std::mutex> scanLock(scanMutex_);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    books_.clear();
    exactWordBlooms_.clear();
    exactWordBloomsReady_ = false;
    ++libraryRevision_;

    if (!bodyOk || damaged) return Ok();   // first run, or a store just set aside
    books_ = std::move(loaded);
    std::sort(books_.begin(), books_.end(),
              [](const TheTableBook& a, const TheTableBook& b) { return a.order < b.order; });
    notes_ = std::move(loadedNotes);
    highlights_ = std::move(loadedHighlights);
    return Ok();
}

// ---------------------------------------------------------------------------
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

std::string TheTableDocId(std::string_view bookId, int chapter) {
    return std::format("table:{}:{}", bookId, chapter);
}

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

    // Only ONE book is copied at a time, under the lock; indexing (an upsert per
    // sermon) runs on that copy so browsing is never held up behind the engine's
    // work. Copying the whole ~100 MB library up front doubled the process's memory
    // for the whole (minutes-long) pass. A library that changes mid-pass (revision
    // moves) ends this pass — the reload that moved it schedules its own.
    uint64_t snapshotRevision = 0;
    size_t bookCount = 0;
    size_t total = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        snapshotRevision = libraryRevision_;
        bookCount = books_.size();
        for (const TheTableBook& book : books_)
            total += book.chapters.size();
    }
    Logger::Instance().Info(std::format("The Table search indexing started ({} sermon documents)", total),
                            kModule);

    size_t indexed = 0;
    std::unordered_map<std::string, WordBloom> exactWordBlooms;
    exactWordBlooms.reserve(total);
    auto publishBloomBatch = [&] {
        if (exactWordBlooms.empty()) return;
        std::lock_guard<std::mutex> scanLock(scanMutex_);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (snapshotRevision != libraryRevision_) {
            exactWordBlooms.clear();
            return;
        }
        for (auto& [id, bloom] : exactWordBlooms)
            exactWordBlooms_.insert_or_assign(std::move(id), bloom);
        exactWordBlooms.clear();
        // Missing entries are deliberately scanned normally while indexing is
        // still in progress; only present summaries are allowed to skip work.
        exactWordBloomsReady_ = true;
    };
    // One cache invalidation for the whole batch, not one per sermon.
    engine.SuspendCacheInvalidation();
    for (size_t bookIndex = 0; bookIndex < bookCount; ++bookIndex) {
        TheTableBook book;
        bool moved = false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            moved = snapshotRevision != libraryRevision_ || bookIndex >= books_.size();
            if (!moved)
                book = books_[bookIndex];
        }
        if (moved)
            break;
        for (const TheTableChapter& ch : book.chapters) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                engine.ResumeCacheInvalidation();   // (end the batch scope even when cancelled)
                return indexed;
            }
            search::SearchDocument doc;
            doc.id = TheTableDocId(book.id, ch.number);
            WordBloom bloom{};
            for (const TheTableVerse& verse : ch.verses)
                for (const std::string& word : textmatch::TokenizeWords(verse.text))
                    AddWordBloom(bloom, word);
            exactWordBlooms.emplace(doc.id, bloom);
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
            if (indexed % 25 == 0)
                publishBloomBatch();
            if (indexed % 25 == 0 || indexed == total)
                Logger::Instance().Info(std::format("The Table search indexing progress: {}/{}",
                                                    indexed, total), kModule);
        }
    }
    engine.ResumeCacheInvalidation();
    // A concurrent import/reload invalidates this snapshot. Never publish
    // summaries from an older library: a false negative would hide a hit.
    publishBloomBatch();
    return indexed;
}

namespace {
// The index's own tokenizer (IndexStorage): lowercase letters/digits, UTF-8 multibyte runs kept whole.
struct TermHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
void LowerTerms(std::string_view text, std::string& lowered, std::vector<std::string_view>& terms) {
    lowered.clear();
    lowered.reserve(text.size());
    for (unsigned char c : text)
        lowered.push_back((std::isalnum(c) || c >= 0x80) ? static_cast<char>(std::tolower(c)) : ' ');
    terms.clear();
    size_t i = 0;
    while (i < lowered.size()) {
        while (i < lowered.size() && lowered[i] == ' ') ++i;
        const size_t start = i;
        while (i < lowered.size() && lowered[i] != ' ') ++i;
        if (i > start) terms.emplace_back(lowered.data() + start, i - start);
    }
}
} // namespace

Result<size_t> TheTableLibrary::IndexVocabularyWithSearchEngine(const std::atomic<bool>* cancelled) {
    search::SearchEngine& engine = search::SearchEngine::Instance();

    // WORDS, NOT DOCUMENTS. IndexWithSearchEngine() stores every sermon in the engine (its text three more times, plus a
    // posting per word per sermon): ~370 MB on top of the library's own ~100 MB, for an index that the sermon search
    // never reads — it scans the library itself. What the engine needs from the sermons is the WORD LIST (typo
    // correction, run-together words, completions) and how many sermons use each word: a few MB. The same pass builds
    // the per-sermon word summaries (blooms) that let a locked search skip sermons.
    uint64_t snapshotRevision = 0;
    size_t bookCount = 0;
    size_t total = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        snapshotRevision = libraryRevision_;
        bookCount = books_.size();
        for (const TheTableBook& book : books_)
            total += book.chapters.size();
    }
    Logger::Instance().Info(std::format("The Table word list started ({} sermons)", total), kModule);

    size_t indexed = 0;
    std::unordered_map<std::string, uint32_t, TermHash, std::equal_to<>> df;
    std::unordered_map<std::string, WordBloom> blooms;
    blooms.reserve(total);
    std::string lowered;
    std::vector<std::string_view> terms;
    std::unordered_set<std::string_view> seen;
    for (size_t bookIndex = 0; bookIndex < bookCount; ++bookIndex) {
        TheTableBook book;
        bool moved = false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            moved = snapshotRevision != libraryRevision_ || bookIndex >= books_.size();
            if (!moved)
                book = books_[bookIndex];   // one year at a time, never the whole library
        }
        if (moved)
            break;
        for (const TheTableChapter& ch : book.chapters) {
            if (cancelled && cancelled->load(std::memory_order_relaxed))
                return indexed;
            WordBloom bloom{};
            for (const TheTableVerse& verse : ch.verses)
                for (const std::string& word : textmatch::TokenizeWords(verse.text))
                    AddWordBloom(bloom, word);
            blooms.emplace(TheTableDocId(book.id, ch.number), bloom);
            // This sermon's distinct words (its title and every paragraph), each counted once toward the word's
            // document count — the same words the document index would have posted it under.
            LowerTerms(TheTableContent(ch), lowered, terms);
            seen.clear();
            seen.insert(terms.begin(), terms.end());
            for (std::string_view t : seen) {
                auto it = df.find(t);
                if (it == df.end()) df.emplace(std::string(t), 1u);
                else ++it->second;
            }
            ++indexed;
            if (indexed % 100 == 0 || indexed == total)
                Logger::Instance().Info(std::format("The Table word list progress: {}/{}", indexed, total), kModule);
        }
    }
    // A library that changed mid-pass (an import landed) ends this pass; the reload that moved it schedules its own.
    {
        std::lock_guard<std::mutex> scanLock(scanMutex_);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (snapshotRevision != libraryRevision_)
            return indexed;
        for (auto& [id, bloom] : blooms)
            exactWordBlooms_.insert_or_assign(id, bloom);
        exactWordBloomsReady_ = true;
    }
    std::vector<std::pair<std::string, uint32_t>> list;
    list.reserve(df.size());
    for (auto& [word, n] : df) list.emplace_back(word, n);
    df = {};
    engine.SetVocabulary("table", std::move(list));
    return indexed;
}

Result<void> TheTableLibrary::UnindexFromSearchEngine() {
    search::SearchEngine& engine = search::SearchEngine::Instance();
    engine.ClearVocabulary("table");
    std::vector<TheTableBook> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
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
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return books_;
}

std::vector<TheTableLibrary::BookInfo> TheTableLibrary::BookIndex() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const TheTableBook& book : books_) {
        if (book.id != bookId) continue;
        for (const TheTableChapter& ch : book.chapters)
            if (ch.number == chapter) return ch;
        break;
    }
        return Error::Make(Err::NotFound, kModule, "no such chapter");
}

std::string TheTableLibrary::Citation(std::string_view bookId, int chapter, int verse) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return CitationLocked(bookId, chapter, verse);
}

std::string TheTableLibrary::CitationNeedle(std::string_view handle)
{
    return CitationNeedleText(handle);
}

std::string TheTableLibrary::CitationCode(std::string_view bookId, int chapter) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const TheTableBook& book : books_) {
        if (book.id != bookId) continue;
        for (const TheTableChapter& ch : book.chapters)
            if (ch.number == chapter)
                return SermonCitation(book.name, ch.code.empty() ? CodeFromTitle(ch.title) : ch.code,
                                      std::string(), 0);
        break;
    }
    return {};
}

SermonParts TheTableLibrary::Parts(std::string_view bookId, int chapter) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    SermonParts out;
    for (const TheTableBook& book : books_) {
        if (book.id != bookId) continue;
        for (const TheTableChapter& ch : book.chapters) {
            if (ch.number != chapter) continue;
            const std::string code = ch.code.empty() ? CodeFromTitle(ch.title) : ch.code;
            out.year = book.name;
            out.code = SermonCitation(book.name, code, std::string(), 0);
            out.citation = SermonCitation(book.name, code, ch.title, 0);
            const size_t dash = out.citation.find(" - ");
            if (dash != std::string::npos)
                out.title = out.citation.substr(dash + 3);
            return out;
        }
        break;
    }
    return out;
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

bool TheTableLibrary::HasVerses() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const TheTableBook& book : books_)
        for (const TheTableChapter& ch : book.chapters)
            if (!ch.verses.empty())
                return true;
    return false;
}

size_t TheTableLibrary::VerseCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
                                                          size_t limit,
                                                          bool exactWords,
                                                          std::string_view scopeBookId,
                                                          int scopeChapter,
                                                          const std::function<bool()>& cancelled) const {
    if (cancelled && cancelled())
        return std::vector<TheTableSearchHit>{};
    const int wantedParagraph = ParagraphDesignator(query);
    const bool numberedLookup = wantedParagraph > 0 && !scopeBookId.empty();

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
    if (terms.empty() && !numberedLookup)
        return Error::Make(Err::InvalidArgument, kModule, "search needs a word");
    if (limit == 0) limit = 1;
    const auto inScope = [&](const TheTableBook& book, const TheTableChapter& ch) {
        if (!scopeBookId.empty() && book.id != scopeBookId) return false;
        if (scopeChapter > 0 && ch.number != scopeChapter) return false;
        return true;
    };
    {
        std::vector<std::string> ordered;
        for (const std::string& t : terms)
            if (std::find(ordered.begin(), ordered.end(), t) == ordered.end()) ordered.push_back(t);
        terms = std::move(ordered);
    }

    std::vector<std::string> bonusWords;
    auto resolved = search::SearchEngine::Instance().ResolveTermsWithSplits(terms, !exactWords);
    if (cancelled && cancelled())
        return std::vector<TheTableSearchHit>{};
    std::vector<std::string> tailCandidates;
    if (resolved.ok() && !exactWords && resolved.value().size() == terms.size()
        && !resolved.value().empty())
        tailCandidates = resolved.value().back().candidates;
    if (resolved.ok() && resolved.value().size() == terms.size()) {
        std::vector<std::string> fixed;
        fixed.reserve(terms.size() + 1);
        for (size_t i = 0; i < terms.size(); ++i) {
            const std::string& r = resolved.value()[i].term;
            const bool applyCorrection = !exactWords || resolved.value()[i].weight < 1.0;
            const std::string& chosen = (applyCorrection && !r.empty()) ? r : terms[i];
            std::istringstream ins(chosen);
            std::string part;
            while (ins >> part) fixed.push_back(part);
            if (!exactWords && !r.empty() && r != terms[i] &&
                std::find(bonusWords.begin(), bonusWords.end(), terms[i]) == bonusWords.end())
                bonusWords.push_back(terms[i]);
        }
        terms = std::move(fixed);
    }

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
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    // Shared: the scan below is long, and the library is read-only for its whole duration. A reader (the reference
    // label, the book index, a chapter) must not wait for it.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (codeShaped) {
        std::vector<std::string> rawTokens;
        {
            const std::string rawQuery(query);   // (not `istringstream in(std::string(query))` — that is a function declaration, not a stream)
            std::istringstream in(rawQuery);
            std::string t;
            while (in >> t) rawTokens.push_back(t);
        }
        int wantPara = 0;
        if (rawTokens.size() >= 2 && !rawTokens.back().empty() &&
            std::all_of(rawTokens.back().begin(), rawTokens.back().end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            wantPara = std::atoi(rawTokens.back().c_str());
        std::string norm;   // "47 - 0412" and "47-0412" are the same code
        for (size_t i = 0; i < rawTokens.size(); ++i) {
            if (wantPara > 0 && i + 1 == rawTokens.size())
                break;                      // the paragraph number is not part of the code
            norm += Lower(rawTokens[i]);
        }
        // A bare "47" or "7" stays a verse/year query (Genesis 47, Luke 1:47);
        // the CODE shape needs a dash ("47-") or a full date-code ("0412").
        const bool dash = norm.find('-') != std::string::npos;
        const size_t digitCount = std::count_if(norm.begin(), norm.end(),
                                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (!norm.empty() && (dash || digitCount >= 3)) {
            for (const TheTableBook& book : books_) {
                if (cancelled && cancelled())
                    return std::vector<TheTableSearchHit>{};
                for (const TheTableChapter& ch : book.chapters) {
                    if (cancelled && cancelled())
                        return std::vector<TheTableSearchHit>{};
                    if (!inScope(book, ch)) continue;   // the chapter lock scopes the code tier too
                    const std::string bare = Lower(ch.code.empty() ? CodeFromTitle(ch.title) : ch.code);
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

            std::stable_sort(codeHits.begin(), codeHits.end(),
                             [](const CodeCand& a, const CodeCand& b) {
                                 return a.score > b.score;
                             });
            if (wantPara > 0) {
                for (const CodeCand& c : codeHits) {
                    if (c.score != 2000)
                        continue;                       // exact citation only
                    if (wantPara > static_cast<int>(c.ch->verses.size()))
                        break;                          // no such paragraph
                    TheTableSearchHit hit;
                    hit.reference = SermonCitation(c.b->name,
                        c.ch->code.empty() ? CodeFromTitle(c.ch->title) : c.ch->code,
                        c.ch->title, wantPara);
                    hit.bookId = c.b->id;
                    hit.chapter = c.ch->number;
                    hit.verse = wantPara;
                    hit.snippet = c.ch->verses[static_cast<size_t>(wantPara) - 1].text.substr(0, 220);
                    hit.score = c.score + 1000;   // a fully-specified reference leads
                    std::vector<TheTableSearchHit> out;
                    out.push_back(std::move(hit));
                    return out;
                }
            }
            std::vector<TheTableSearchHit> out;
            out.reserve(std::min(limit, codeHits.size()));
            for (const CodeCand& c : codeHits) {
                if (c.ch->verses.empty()) continue;
                TheTableSearchHit hit;
                hit.reference = SermonCitation(c.b->name,
                    c.ch->code.empty() ? CodeFromTitle(c.ch->title) : c.ch->code,
                    c.ch->title, 0);
                hit.bookId = c.b->id;
                hit.chapter = c.ch->number;
                hit.verse = 0;
                hit.snippet = c.ch->verses.front().text.substr(0, 220);
                hit.score = c.score;
                out.push_back(std::move(hit));
                if (out.size() >= limit) break;
            }
            return out;
        }
    }

    // ---- a BARE PARAGRAPH NUMBER inside a LOCKED search -------------------------
    if (numberedLookup) {
        std::vector<TheTableSearchHit> numbered;
        for (const TheTableBook& book : books_) {
            for (const TheTableChapter& ch : book.chapters) {
                if (!inScope(book, ch)) continue;
                if (wantedParagraph > static_cast<int>(ch.verses.size())) continue;
                const TheTableVerse& verse = ch.verses[static_cast<size_t>(wantedParagraph) - 1];
                TheTableSearchHit hit;
                hit.reference = SermonCitation(book.name,
                    ch.code.empty() ? CodeFromTitle(ch.title) : ch.code, ch.title, wantedParagraph);
                hit.bookId = book.id;
                hit.chapter = ch.number;
                hit.verse = wantedParagraph;
                hit.snippet = verse.text.substr(0, 220);
                hit.fullText = verse.text;
                // A direct reference is not a relevance guess: it leads anything the
                // word scan could find (the exact citation tier scores 2000).
                hit.score = 2500.0;
                numbered.push_back(std::move(hit));
                if (numbered.size() >= limit) break;
            }
            if (numbered.size() >= limit) break;
        }
        if (!numbered.empty()) return numbered;
        if (terms.empty())
            return Error::Make(Err::InvalidArgument, kModule, "search needs a word");
    }

    // One pass over EVERY sermon (reading order = the sermonRank tiebreak below).

    struct Acc { const TheTableVerse* v; const TheTableBook* b; const TheTableChapter* ch; int score; bool spanned; bool phrase; int run; int sermonRank; };
    std::vector<Acc> hits;
    auto scanChapter = [&](const TheTableBook& book, const TheTableChapter& ch, int sermonRank) {
        // The lowercase text of THIS sermon only, dropped when the scan of it ends. It used to be kept for every
        // sermon after the first search (a second copy of the whole library: ~100 MB resident for good); lowercasing
        // a sermon costs microseconds next to scanning it, so nothing is kept.
        std::vector<std::string> hay;
        hay.reserve(ch.verses.size());
        for (size_t i = 0; i < ch.verses.size(); ++i) {
            if ((i & 0x3Fu) == 0 && cancelled && cancelled())
                return;
            hay.push_back(Lower(ch.verses[i].text));
        }
        const size_t n = ch.verses.size();
        std::vector<std::string_view> spans;
        std::string_view spanSegs[2]{};
        int spansKey = -1;   // paragraph i, or -(i+1) for the pair (i, i+1)
        const auto fillSpans = [&](size_t i, bool pair) {
            const int key = pair ? -int(i + 1) : int(i);
            if (spansKey == key)
                return;   // this text's spans are already in `spans`
            spans.clear();
            spanSegs[0] = hay[i];
            spanSegs[1] = (pair && i + 1 < n) ? hay[i + 1] : std::string_view{};
            textmatch::AppendWordSpans(spanSegs, pair ? 2 : 1, spans);
            spansKey = key;
        };
        std::vector<unsigned> bm(n, 0u);   // per-paragraph term-hit bitmask (substring)
        std::vector<unsigned> wm(n, 0u);   // of those, whole-word hits
        for (size_t i = 0; i < n; ++i) {
            if ((i & 0x3Fu) == 0 && cancelled && cancelled())
                return;
            unsigned m = 0, w = 0;
            for (size_t t = 0; t < terms.size(); ++t) {
                const textmatch::TermPresence seen = textmatch::PresenceOf(hay[i], terms[t]);
                if (!seen.found) continue;
                if (exactWords && !seen.whole) continue;
                m |= (1u << t);
                if (seen.whole || seen.start) w |= (1u << t);
            }
            bm[i] = m;
            wm[i] = w;
        }
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
        std::vector<int> ladder(n, 0);
        if (!tailCandidates.empty()) {
            for (size_t i = 0; i < n; ++i) {
                if (bm[i] == 0u) continue;
                for (size_t c = 0; c < tailCandidates.size(); ++c) {
                    if (ContainsWord(hay[i], tailCandidates[c])) {
                        ladder[i] = int(tailCandidates.size() - c);
                        break;
                    }
                }
            }
        }
        const auto ladderOf = [&](size_t i) { return ladder.empty() ? 0 : ladder[i]; };
        auto quality = [&](unsigned m, unsigned w) {
            int q = 0;
            for (size_t t = 0; t < terms.size() && t < 32; ++t) {
                if (m & (1u << t)) ++q;
                if (w & (1u << t)) ++q;   // a whole word counts twice
            }
            return q;
        };
        const unsigned all = terms.size() >= 32 ? 0xFFFFFFFFu : ((1u << terms.size()) - 1u);
        // Locked, the sentence must be there as typed — the phrase test closes its
        // last-word tolerance too ("my wife" no longer matches "my wives").
        const bool phraseWhole = exactWords;
        const auto bitsOf = [](unsigned m) { int c = 0; for (; m; m &= m - 1) ++c; return c; };
        if (exactWords) {
            const size_t floorRun = terms.size() > 1 ? 2 : 1;
            constexpr size_t kMaxGapWords = 2;
            const auto longestRun = [&](const std::vector<std::string_view>& words, int& outRun,
                                        size_t minK = 1, unsigned present = 0xFFFFFFFFu) {
                outRun = 0;
                if (minK > terms.size()) return;
                for (size_t k = terms.size(); k >= std::max(floorRun, minK); --k)
                    for (size_t s = 0; s + k <= terms.size(); ++s) {
                        if (s < 32) {   // (a window past the 32-term bitmask is tested unmasked)
                            const unsigned need =
                                (k >= 32 ? 0xFFFFFFFFu : ((1u << k) - 1u)) << s;
                            if ((present & need) != need) continue;   // a term of this window is absent
                        }
                        if (textmatch::ContainsTermRunWords(words, terms, s, k, kMaxGapWords)) {
                            outRun = int(k);
                            return;
                        }
                    }
            };
            std::vector<int> runOf(n, 0);
            for (size_t i = 0; i < n; ++i) {
                if (size_t(bitsOf(bm[i])) < floorRun)
                    continue;
                if (terms.size() == 1) {
                    runOf[i] = 1;
                } else {
                    fillSpans(i, false);
                    longestRun(spans, runOf[i], 1, bm[i]);
                }
                if (size_t(runOf[i]) < floorRun)
                    continue;
                const bool exactPhrase = ContainsPhrase(hay[i], terms, nullptr, true);
                // (`spans` still holds this paragraph's words from the run test above)
                const int tight = terms.size() > 1 ? TightBonus(TightSpan(spans, terms, true), terms.size()) : 0;
                hits.push_back({&ch.verses[i], &book, &ch,
                                1000 + quality(bm[i], wm[i]) + tight
                                    + (size_t(runOf[i]) == terms.size() ? 500 : 0)
                                    + 20 * bonusOf(i, n),
                                false, exactPhrase, runOf[i], sermonRank});
            }
            const bool pairPhase = terms.size() > 1;
            for (size_t i = 0; pairPhase && i + 1 < n; ++i) {
                const unsigned pair = bm[i] | bm[i + 1];
                if (size_t(bitsOf(pair)) < floorRun)
                    continue;
                // (a pair whose halves already qualify alone was pushed above —
                // only a spread-out match that BEATS both halves lands here)
                if (bm[i] == all && bm[i + 1] == all)
                    continue;
                bool crosses = false;
                for (size_t t = 0; t + 1 < terms.size() && !crosses; ++t)
                    crosses = (bm[i] & (1u << t)) && (bm[i + 1] & (1u << (t + 1)));
                if (!crosses)
                    continue;
                int run = 0;
                const size_t need = size_t(std::max(runOf[i], runOf[i + 1])) + 1;
                if (need > terms.size())
                    continue;
                fillSpans(i, true);   // the pair's words, in one reused scratch
                longestRun(spans, run, need, pair);
                if (size_t(run) < floorRun)
                    continue;
                if (run < runOf[i] || run < runOf[i + 1])
                    continue;
                if (run == runOf[i] || run == runOf[i + 1])
                    continue;   // the same combination, already listed from one paragraph
                const std::string joined = hay[i] + ' ' + hay[i + 1];
                const bool exactPhrase = ContainsPhrase(joined, terms, nullptr, true);
                hits.push_back({&ch.verses[i], &book, &ch,
                                quality(pair, wm[i] | wm[i + 1])
                                    + (size_t(run) == terms.size() ? 500 : 0)
                                    + 20 * bonusOf(i, i + 1),
                                true, exactPhrase, run, sermonRank});
            }
        } else {
            for (size_t i = 0; i < n; ++i)
                if (bm[i] == all) {
                    if (terms.size() > 1)
                        fillSpans(i, false);
                    const bool phrase = ContainsPhrase(hay[i], terms, nullptr, phraseWhole);
                    const bool near = terms.size() > 1
                        ? textmatch::ContainsNearPhraseWords(spans, terms, 2)
                        : phrase;
                    const int tight = terms.size() > 1 ? TightBonus(TightSpan(spans, terms, false), terms.size()) : 0;
                    hits.push_back({&ch.verses[i], &book, &ch,
                                    1000 + quality(bm[i], wm[i]) + tight
                                        + (phrase ? 500 : (near ? 300 : 0))
                                        + 20 * bonusOf(i, n)
                                        + 8 * ladderOf(i),
                                    false, phrase, int(terms.size()), sermonRank});
                }
            for (size_t i = 0; i + 1 < n; ++i) {
                const unsigned pair = bm[i] | bm[i + 1];
                // (a pair whose halves already qualify alone was pushed above — only
                // genuinely spread-out matches land here)
                if (pair == all && bm[i] != all && bm[i + 1] != all) {
                    const std::string joined = hay[i] + ' ' + hay[i + 1];
                    const bool phrase = ContainsPhrase(joined, terms, nullptr, phraseWhole);
                    const bool near = ContainsNearPhrase(joined, terms, 2);
                    hits.push_back({&ch.verses[i], &book, &ch,
                                    quality(pair, wm[i] | wm[i + 1])
                                        + (phrase ? 500 : (near ? 300 : 0))
                                        + 20 * bonusOf(i, i + 1)
                                        + 8 * std::max(ladderOf(i), ladderOf(i + 1)),
                                    true, phrase, 0, sermonRank});
                }
            }
        }
    };
    int rank = 0;
    for (const TheTableBook& book : books_) {
        for (const TheTableChapter& ch : book.chapters) {
            if (cancelled && cancelled())
                return std::vector<TheTableSearchHit>{};
            bool skip = !inScope(book, ch);
            if (skip) {
                ++rank;   // the sermon-level tiebreak counts every sermon, scoped or not
                continue;
            }
            if (exactWords && exactWordBloomsReady_) {
                const auto bloom = exactWordBlooms_.find(TheTableDocId(book.id, ch.number));
                if (bloom != exactWordBlooms_.end()) {
                    skip = std::any_of(terms.begin(), terms.end(), [&](const std::string& term) {
                        return !MightHaveWord(bloom->second, term);
                    });
                }
            }
            if (!skip) scanChapter(book, ch, rank);
            ++rank;
        }
    }
    if (cancelled && cancelled())
        return std::vector<TheTableSearchHit>{};
    std::stable_sort(hits.begin(), hits.end(), [](const Acc& a, const Acc& b) {
        if (a.phrase != b.phrase) return a.phrase;
        if (a.run != b.run) return a.run > b.run;    // THE SENTENCE, then the next combination
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
        s.fullText = h.v->text;
        if (h.spanned) {
            const auto verse = std::find_if(h.ch->verses.begin(), h.ch->verses.end(),
                                            [&](const TheTableVerse& v) { return &v == h.v; });
            if (verse != h.ch->verses.end() && std::next(verse) != h.ch->verses.end())
                s.fullText += "\n\n" + std::next(verse)->text;
        }
        {
            const std::string low = Lower(h.v->text);
            size_t anchor = std::string::npos;
            if (!(ContainsPhrase(low, terms, &anchor) && anchor != std::string::npos))
                anchor = NearPhrasePosition(low, terms, 2);
            if (anchor == std::string::npos) {
                anchor = std::string::npos;
                for (const auto& t : terms) {
                    const size_t at = NearPhrasePosition(low, std::vector<std::string>{t}, 0);
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

namespace {
// The file name without its directories, for a message that names the file.
std::string BaseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* BinaryKindOf(const std::string& data) {
    struct Signature { const char* magic; size_t length; const char* kind; };
    static const Signature kSignatures[] = {
        { "PK\x03\x04", 4, "a ZIP or Office document (.docx/.xlsx/.pptx)" },
        { "PK\x05\x06", 4, "a ZIP file" },
        { "\x89PNG\r\n\x1a\n", 8, "a PNG image" },
        { "\xFF\xD8\xFF", 3, "a JPEG image" },
        { "GIF87a", 6, "a GIF image" },
        { "GIF89a", 6, "a GIF image" },
        { "\x1F\x8B", 2, "a gzip archive" },
        { "Rar!\x1A\x07", 6, "a RAR archive" },
        { "7z\xBC\xAF\x27\x1C", 6, "a 7-Zip archive" },
        { "\xD0\xCF\x11\xE0", 4, "an older Office document (.doc/.xls)" },
        { "\x7F\x45\x4C\x46", 4, "a Linux program" },
        { "%PDF-", 5, "a PDF" },
        { "ID3", 3, "an MP3 audio file" },
        { "OggS", 4, "an Ogg media file" },
        { "RIFF", 4, "a WAV or AVI file" },
        { "\x00\x00\x01\xBA", 4, "an MPEG video" },
    };
    for (const Signature& s : kSignatures) {
        if (data.size() >= s.length && data.compare(0, s.length, s.magic, s.length) == 0)
            return s.kind;
    }
    return nullptr;
}

// The extension as it is written in the name, lower-cased, INCLUDING the dot -
// or "" when the name has none. (A name ending in a dot counts as none.)
std::string ExtensionOf(const std::string& name) {
    const std::string file = BaseNameOf(name);
    const size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= file.size())
        return {};
    return Lower(file.substr(dot));
}
} // namespace

bool TheTableLibrary::IsSermonFile(std::string_view fileName, std::string_view content, std::string& whyNot) {
    const std::string name(fileName);
    const std::string ext = ExtensionOf(name);

    // 1. The format is decided by the NAME: the library reads .txt and .pdf, and
    //    anything else is refused before a single byte is parsed. (The dialog's
    //    "All files (*.*)" option can hand over anything at all.)
    if (ext != ".txt" && ext != ".pdf") {
        const std::string shown = BaseNameOf(name);
        whyNot = ext.empty()
            ? "\"" + shown + "\" has no file extension - a sermon is a .txt or a .pdf"
            : "\"" + shown + "\" is " + ext + " - a sermon is a .txt or a .pdf";
        return false;
    }

    // 2. The name's claim is then checked against the file's own first bytes -
    //    the PDF magic is what the .pdf reader itself demands, so a .pdf that
    //    really is one passes here and every other signature is a lie.
    const std::string data(content);
    const bool startsPdf = data.size() >= 5 && data.compare(0, 5, "%PDF-") == 0;
    if (ext == ".pdf") {
        if (startsPdf)
            return true;   // the real thing: the PDF reader takes it from here
        if (const char* kind = BinaryKindOf(data); kind != nullptr)
            whyNot = "\"" + BaseNameOf(name) + "\" is " + kind + ", not a PDF";
        else
            whyNot = "\"" + BaseNameOf(name) + "\" is not a PDF file";
        return false;
    }
    if (const char* kind = BinaryKindOf(data); kind != nullptr) {
        whyNot = startsPdf
            ? "\"" + BaseNameOf(name) + "\" is a PDF saved as .txt - rename it .pdf and it will be read as one"
            : "\"" + BaseNameOf(name) + "\" is " + kind + ", not a sermon (.txt or .pdf)";
        return false;
    }
    return true;
}

Result<std::string> TheTableLibrary::ConvertToCleanText(std::string_view fileName, std::string_view content) {
    const std::string data(content);
    if (std::string whyNot; !IsSermonFile(fileName, content, whyNot))
        return Error::Make(kTableErr, kModule, whyNot);
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
    std::unique_lock<std::shared_mutex> lock(mutex_);
    exactWordBlooms_.clear();
    exactWordBloomsReady_ = false;
    ++libraryRevision_;
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
    ImportReport report;
    for (const std::string& path : found.value()) {
        const std::string lower = Lower(path);
        if (lower.ends_with(".pdf") || lower.ends_with(".txt")) {
            files.push_back(path);
            continue;
        }
        ++report.unsupported;
        report.rejected.push_back(BaseNameOf(path) + ": not a sermon file (the library reads .txt and .pdf)");
    }
    std::sort(files.begin(), files.end());   // deterministic order (year, then code)

    int done = 0;
    const int total = static_cast<int>(files.size());
    for (const std::string& path : files) {
        ++done;
        if (progress) progress(done, total, path);

        // Duplicate guard: the same year + sermon title is already in.
        const Placement place = PlacementOf(path);
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
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
                report.rejected.push_back(BaseNameOf(path) + ": could not be read (" + bytes.error().message + ")");
                Logger::Instance().Info("Table import: could not read " + path + ": " + bytes.error().message, kModule);
                continue;
            }
            auto converted = ConvertToCleanText(path, std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
            if (!converted.ok()) {
                // The reason travels with the count now: "3 failed" said nothing
                // about a folder whose .txt files are really renamed spreadsheets.
                ++report.failed;
                report.rejected.push_back(BaseNameOf(path) + ": " + converted.error().message);
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
            report.rejected.push_back(BaseNameOf(path) + ": " + r.error().message);
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