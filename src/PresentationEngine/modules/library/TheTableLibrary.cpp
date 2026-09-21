#include "modules/library/TheTableLibrary.hpp"

#include "core/config/Json.hpp"
#include "core/logging/Logger.hpp"
#include "modules/content/AssetCompressor.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace bps::library {

namespace {

constexpr const char* kModule = "TheTableLibrary";
constexpr ErrorCode kTableErr = 2650;   // the Library band's Table slot

using J = json::Value;

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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

    // Year: a 4-digit token at the start OR in the parent path
    // ("downloads/1953/53_0217_X.pdf"). Scan path segments.
    std::string path = fileName;
    size_t seg = 0;
    while ((seg = path.find_first_not_of("/\\", seg)) != std::string::npos) {
        const size_t end = path.find_first_of("/\\", seg);
        const std::string part = path.substr(seg, end == std::string::npos ? end : end - seg);
        if (part.size() == 4 && std::all_of(part.begin(), part.end(), ::isdigit)) {
            n.year = part;
            break;
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
    // The numeric code before the title ("0217") is useful in the reference —
    // re-attach it so chapter titles sort naturally: "0217 Only Believe".
    const size_t codeEnd = base.find('_', t == 0 ? base.size() : 0);
    (void)codeEnd;
    return n;
}

// The leading numeric code ("0217") kept for the chapter title.
std::string CodeOf(const std::string& fileName) {
    std::string base = fileName;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t b = base.find_first_not_of(" \t");
    std::string code;
    size_t i = b;
    while (i < base.size() && std::isdigit(static_cast<unsigned char>(base[i]))) ++i;
    if (i > b && i < base.size() && base[i] == '_') code = base.substr(b, i - b);
    return code;
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

// Finds the CONTENTS stream of a page object and appends its decoded text ops.
// Simple parser: for every "stream ... endstream" whose dict says FlateDecode,
// inflate and scan; PDFs in the wild (and these sermons) put text ops only in
// page content streams, so scanning every flate stream for text operators is
// both simpler and more robust than object-graph walking.
bool LooksLikeTextOps(const std::string& s) {
    return s.find("Tj") != std::string::npos || s.find("TJ") != std::string::npos;
}

std::string TextFromPdf(const std::string& data) {
    std::string ops;
    const std::string kStream = "stream";
    const std::string kEnd = "endstream";
    size_t pos = 0;
    while ((pos = data.find(kStream, pos)) != std::string::npos) {
        // The dict just before "stream" decides the filter.
        const size_t dictStart = data.rfind("<<", pos);
        const std::string dict =
            dictStart == std::string::npos ? "" : data.substr(dictStart, pos - dictStart);
        pos += kStream.size();
        if (pos < data.size() && data[pos] == '\r') ++pos;
        if (pos < data.size() && data[pos] == '\n') ++pos;
        const size_t end = data.find(kEnd, pos);
        if (end == std::string::npos) break;
        const std::string raw = data.substr(pos, end - pos);
        pos = end + kEnd.size();

        std::string body;
        if (dict.find("FlateDecode") != std::string::npos) {
            content::DeflateCompressor deflate;
            auto out = deflate.Decompress(
                reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
            if (!out.ok()) continue;
            body.assign(out.value().begin(), out.value().end());
        } else if (dict.find("Filter") == std::string::npos) {
            body = raw;   // uncompressed stream
        } else {
            continue;     // encoded with something we do not decode (images etc.)
        }
        if (LooksLikeTextOps(body)) ops += body;
    }
    if (ops.empty()) return {};

    // Harvest the strings of text-showing operators. A '(' ... ') Tj' run is a
    // text run; newlines in the source (Td/TD/T*) become paragraph breaks.
    std::string text;
    bool lineStart = true;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] == '(') {
            // String literal (balanced, escapes).
            std::string raw;
            int depth = 1;
            ++i;
            for (; i < ops.size() && depth > 0; ++i) {
                if (ops[i] == '\\' && i + 1 < ops.size()) {
                    raw.push_back(ops[i]);
                    raw.push_back(ops[++i]);
                    continue;
                }
                if (ops[i] == '(') ++depth;
                else if (ops[i] == ')') {
                    --depth;
                    if (depth == 0) break;
                }
                raw.push_back(ops[i]);
            }
            text += UnescapePdfString(raw);
            lineStart = false;
        } else if (ops.compare(i, 2, "Td") == 0 || ops.compare(i, 2, "TD") == 0 ||
                   ops.compare(i, 2, "T*") == 0 || ops.compare(i, 2, "ET") == 0) {
            if (!lineStart) text += "\n";
            lineStart = true;
        }
    }
    return text;
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

} // namespace

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
    std::lock_guard<std::mutex> lock(mutex_);
    J::Object root;
    root["schema"] = J::Number(1);
    J::Array booksArr;
    for (const TheTableBook& book : books_) {
        J::Object b;
        b["id"] = J::String(book.id);
        b["name"] = J::String(book.name);
        b["order"] = J::Number(book.order);
        J::Array chs;
        for (const TheTableChapter& ch : book.chapters) {
            J::Object c;
            c["number"] = J::Number(ch.number);
            c["title"] = J::String(ch.title);
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

    auto& platform = platform::PlatformAccessor::Get();
    auto dir = platform.Paths().UserDataDir();
    // Ensure the user dir exists (Write writes files; parents must already be there).
    (void)platform.Filesystem().CreateDirectories(dir);
    return platform.Filesystem().Write(filePath_, J(std::move(root)).ToString());
}

Result<void> TheTableLibrary::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    books_.clear();

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
    return Ok();
}

// ---------------------------------------------------------------------------
// Browsing
// ---------------------------------------------------------------------------
std::vector<TheTableBook> TheTableLibrary::Books() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return books_;
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
    // Term extraction: lowercase words >= 3 chars.
    std::vector<std::string> terms;
    std::string cur;
    for (char c : Lower(std::string(query))) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            if (cur.size() >= 3) terms.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty() && cur.size() >= 3) terms.push_back(cur);
    if (terms.empty())
        return Error::Make(Err::InvalidArgument, kModule, "search needs a word");
    if (limit == 0) limit = 1;

    struct Acc { const TheTableVerse* v; const TheTableBook* b; int chapter; int matches; };
    std::vector<Acc> hits;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const TheTableBook& book : books_) {
            for (const TheTableChapter& ch : book.chapters) {
                for (const TheTableVerse& v : ch.verses) {
                    const std::string hay = Lower(v.text);
                    int matches = 0;
                    for (const std::string& t : terms)
                        if (hay.find(t) != std::string::npos) ++matches;
                    if (matches == static_cast<int>(terms.size()))
                        hits.push_back({&v, &book, ch.number, matches});
                }
            }
        }
    }
    // Best-first: more coverage wins (== all terms here), longer paragraphs tie-break.
    std::sort(hits.begin(), hits.end(), [](const Acc& a, const Acc& b) {
        if (a.matches != b.matches) return a.matches > b.matches;
        return a.v->text.size() < b.v->text.size();
    });
    if (hits.size() > limit) hits.resize(limit);

    std::vector<TheTableSearchHit> out;
    out.reserve(hits.size());
    for (const Acc& h : hits) {
        TheTableSearchHit s;
        s.reference = h.b->name + " " + std::to_string(h.chapter) + ":" +
                      std::to_string(h.v->number);
        s.bookId = h.b->id;
        s.chapter = h.chapter;
        s.verse = h.v->number;
        s.snippet = h.v->text.substr(0, 220);
        s.score = static_cast<double>(h.matches);
        out.push_back(std::move(s));
    }
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

Result<std::string> TheTableLibrary::ImportSermon(std::string_view fileName,
                                                std::string_view content) {
    const std::string data(content);
    const std::string name(fileName);

    // .pdf -> extract text ops; anything else parses as plain text.
    std::string lower = Lower(name);
    std::string text;
    if (lower.ends_with(".pdf")) {
        if (data.size() < 8 || data.substr(0, 5) != "%PDF-")
            return Error::Make(kTableErr, kModule, "not a PDF file");
        text = TextFromPdf(data);
        if (Trim(text).empty())
            return Error::Make(kTableErr, kModule,
                               "no text could be read from the PDF (a scanned image has no text layer)");
    } else {
        text = data;
    }

    auto paragraphs = ParagraphsFromText(text);
    if (paragraphs.empty())
        return Error::Make(kTableErr, kModule, "no paragraphs found in the sermon");

    SermonName parsed = NameFromFileName(name);
    if (parsed.year.empty()) parsed.year = "Unfiled";
    std::string code = CodeOf(name);
    std::string title = parsed.title;
    if (!code.empty() && !title.starts_with(code)) title = code + " " + title;

    std::lock_guard<std::mutex> lock(mutex_);
    TheTableBook& book = BookForYear(parsed.year);
    TheTableChapter ch;
    ch.number = static_cast<int>(book.chapters.size()) + 1;
    ch.title = title;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        TheTableVerse v;
        v.number = static_cast<int>(i) + 1;
        v.text = paragraphs[i];
        if (i == 0) v.heading = title;
        ch.verses.push_back(std::move(v));
    }
    book.chapters.push_back(std::move(ch));

    auto saved = const_cast<TheTableLibrary*>(this)->Save();
    if (!saved.ok())
        Logger::Instance().Warning("TheTableLibrary save failed: " + saved.error().message, kModule);
    return book.name + " " + std::to_string(book.chapters.size()) + ":1";
}

} // namespace bps::library
