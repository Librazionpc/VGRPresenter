#include "modules/bible/IBibleProvider.hpp"

#include "core/config/Json.hpp"
#include "modules/bible/ReferenceResolver.hpp"
#include "modules/xml/Xml.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <sstream>

namespace bps::bible {

namespace {

using XmlNode = xml::Node;
using xml::Collapse;
using xml::Trim;

int ToInt(const std::string& s, int dflt = 0) {
    if (s.empty()) return dflt;
    int v = 0;
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    return i == 0 ? dflt : v;
}

// XML tags arrive in whatever case the source wrote them: Zefania writes every
// tag uppercase (<XMLBIBLE>/<BIBLEBOOK>/<CHAPTER>/<VERS>), other tools write
// lowercase. Compare case-insensitively so both shapes parse.
bool TagIs(const XmlNode& n, std::string_view name) {
    if (n.tag.size() != name.size()) return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(n.tag[i])) !=
            std::tolower(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

// Case-insensitive attribute lookup (Zefania: biblename / bnumber / cnumber /
// vnumber alongside the platform's lowercase name / num).
std::string AttrNoCase(const XmlNode& n, std::string_view name) {
    for (const auto& [key, value] : n.attrs) {
        if (key.size() != name.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < key.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(key[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                eq = false;
                break;
            }
        if (eq) return value;
    }
    return {};
}

// Default 66-book table when the source provides no book names (plain text,
// USFM without a mapping). Mirrors ReferenceResolver's canonical table.
std::vector<BibleBook> DefaultBooks() {
    struct B {
        const char* id; const char* name; const char* test;
    };
    static const B k[] = {
        {"GEN", "Genesis", "old"}, {"EXO", "Exodus", "old"},
        {"LEV", "Leviticus", "old"}, {"NUM", "Numbers", "old"},
        {"DEU", "Deuteronomy", "old"}, {"JOS", "Joshua", "old"},
        {"JDG", "Judges", "old"}, {"RUT", "Ruth", "old"},
        {"1SA", "1 Samuel", "old"}, {"2SA", "2 Samuel", "old"},
        {"1KI", "1 Kings", "old"}, {"2KI", "2 Kings", "old"},
        {"1CH", "1 Chronicles", "old"}, {"2CH", "2 Chronicles", "old"},
        {"EZR", "Ezra", "old"}, {"NEH", "Nehemiah", "old"},
        {"EST", "Esther", "old"}, {"JOB", "Job", "old"},
        {"PSA", "Psalms", "old"}, {"PRO", "Proverbs", "old"},
        {"ECC", "Ecclesiastes", "old"}, {"SNG", "Song of Solomon", "old"},
        {"ISA", "Isaiah", "old"}, {"JER", "Jeremiah", "old"},
        {"LAM", "Lamentations", "old"}, {"EZK", "Ezekiel", "old"},
        {"DAN", "Daniel", "old"}, {"HOS", "Hosea", "old"},
        {"JOL", "Joel", "old"}, {"AMO", "Amos", "old"},
        {"OBA", "Obadiah", "old"}, {"JON", "Jonah", "old"},
        {"MIC", "Micah", "old"}, {"NAM", "Nahum", "old"},
        {"HAB", "Habakkuk", "old"}, {"ZEP", "Zephaniah", "old"},
        {"HAG", "Haggai", "old"}, {"ZEC", "Zechariah", "old"},
        {"MAL", "Malachi", "old"},
        {"MAT", "Matthew", "new"}, {"MRK", "Mark", "new"},
        {"LUK", "Luke", "new"}, {"JHN", "John", "new"},
        {"ACT", "Acts", "new"}, {"ROM", "Romans", "new"},
        {"1CO", "1 Corinthians", "new"}, {"2CO", "2 Corinthians", "new"},
        {"GAL", "Galatians", "new"}, {"EPH", "Ephesians", "new"},
        {"PHP", "Philippians", "new"}, {"COL", "Colossians", "new"},
        {"1TH", "1 Thessalonians", "new"}, {"2TH", "2 Thessalonians", "new"},
        {"1TI", "1 Timothy", "new"}, {"2TI", "2 Timothy", "new"},
        {"TIT", "Titus", "new"}, {"PHM", "Philemon", "new"},
        {"HEB", "Hebrews", "new"}, {"JAS", "James", "new"},
        {"1PE", "1 Peter", "new"}, {"2PE", "2 Peter", "new"},
        {"1JN", "1 John", "new"}, {"2JN", "2 John", "new"},
        {"3JN", "3 John", "new"}, {"JUD", "Jude", "new"},
        {"REV", "Revelation", "new"},
    };
    std::vector<BibleBook> out;
    for (int n = 0; n < 66; ++n) {
        BibleBook b;
        b.id = k[n].id;
        b.name = k[n].name;
        b.testament = k[n].test;
        b.order = n + 1;
        out.push_back(std::move(b));
    }
    return out;
}

// Canonical 3-letter id for a Zefania book number (bnum 1..66).
std::string CanonicalIdFromNumber(int num) {
    for (const auto& b : DefaultBooks())
        if (b.order == num) return b.id;
    return {};
}

// Canonical 3-letter id for a full book name ("John" -> "JHN").
std::string CanonicalIdFromName(std::string_view name) {
    for (const auto& b : DefaultBooks())
        if (std::string(b.name) == name) return b.id;
    return {};
}

// Finds a book in `books` by id; appends if missing (with a canonical name).
void EnsureBook(std::vector<BibleBook>& books, const std::string& id) {
    for (const auto& b : books)
        if (b.id == id) return;
    for (const auto& d : DefaultBooks()) {
        if (d.id == id) {
            books.push_back(d);
            return;
        }
    }
    BibleBook b;
    b.id = id;
    b.name = id;
    b.order = static_cast<int>(books.size()) + 1;
    books.push_back(std::move(b));
}

// Builds chapters + flattened verses from per-book verse streams.
BibleVersion Finalize(BibleVersion out) {
    std::map<std::string, BibleChapter, std::less<>> chapters;
    for (auto& book : out.books) {
        std::map<int, BibleChapter> byNumber;
        for (const auto& v : out.verses) {
            if (v.bookId != book.id) continue;
            auto& ch = byNumber[v.chapter];
            ch.bookId = book.id;
            ch.number = v.chapter;
            ch.verses.push_back(v);
        }
        for (auto& [num, ch] : byNumber) chapters[std::format("{}.{}", book.id, num)] = std::move(ch);
    }
    out.chapters = std::move(chapters);
    return out;
}

// ---------------------------------------------------------------------------
// XML provider — Zefania shape + the platform's own <bible><book> shape.
// ---------------------------------------------------------------------------
class XmlBibleProvider final : public IBibleProvider {
public:
    const char* Name() const noexcept override { return "zefania-xml"; }
    const char* Format() const noexcept override { return "xml"; }
    std::vector<std::string> SupportedExtensions() const override { return {".xml"}; }

    Result<BibleVersion> Parse(std::string_view source, std::string_view) const override {
        auto parsed = xml::Parse(source);
        if (!parsed.ok()) return parsed.error();
        const XmlNode& bible = parsed.value();
        // The platform's own <bible> shape or Zefania's <XMLBIBLE> (which
        // uppercases every tag) — both accepted, compared case-insensitively.
        if (!TagIs(bible, "bible") && !TagIs(bible, "xmlbible"))
            return Error::Make(Err::Bible_UnsupportedFormat, "BibleProvider",
                               "root element is not <bible>/<XMLBIBLE> (got <" + bible.tag + ">)");

        BibleVersion out;
        out.metadata.abbreviation = bible.Attr("abbrev");
        out.metadata.name = bible.Attr("name");
        if (out.metadata.name.empty()) out.metadata.name = AttrNoCase(bible, "biblename");
        out.metadata.source = "xml";

        for (const auto& bookNode : bible.children) {
            if (!TagIs(bookNode, "book") && !TagIs(bookNode, "biblebook")) continue;
            // Canonical id: prefer the short id (bsname), then derive it from
            // the full name or Zefania book number (bnum/bnumber) — real Zefania
            // files carry bnumber="43" bname="John" with no bsname, and a numeric
            // id would break reference resolution. Uppercase for consistency.
            std::string id;
            std::string bnum = AttrNoCase(bookNode, "bnum");
            if (bnum.empty()) bnum = AttrNoCase(bookNode, "bnumber");
            std::string name = !bookNode.Attr("bname").empty() ? bookNode.Attr("bname")
                                                               : bookNode.Attr("name");
            if (!bookNode.Attr("bsname").empty()) {
                id = bookNode.Attr("bsname");
            } else if (!bnum.empty()) {
                // bnum/bnumber is the unambiguous canonical 1-66 ordering in
                // Zefania; prefer it over the display name, which may be
                // abbreviated ("Psalm", "1 Sam.") and not match the canonical
                // table.
                id = CanonicalIdFromNumber(ToInt(bnum));
                if (id.empty()) id = bnum;
            } else if (!name.empty()) {
                id = CanonicalIdFromName(name);
                if (id.empty()) id = name;
            } else {
                id = bookNode.Attr("num");
            }
            if (id.empty()) id = name.empty() ? "BOOK" : name;
            if (name.empty()) name = id;
            for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            EnsureBook(out.books, id);
            for (auto& b : out.books)
                if (b.id == id) {
                    b.name = name;
                    if (b.testament.empty()) b.testament = (b.order <= 39) ? "old" : "new";
                }
            // Book-level heading (Zefania <t> / <h> under <book>).
            std::string bookHeading;
            if (const auto* t = bookNode.FindChild("t")) bookHeading = Collapse(Trim(t->text));
            if (const auto* h = bookNode.FindChild("h")) bookHeading = Collapse(Trim(h->text));

            for (const auto& chNode : bookNode.children) {
                if (!TagIs(chNode, "chapter")) continue;
                std::string cnum = chNode.Attr("number");
                if (cnum.empty()) cnum = chNode.Attr("num");
                if (cnum.empty()) cnum = AttrNoCase(chNode, "cnumber");
                int chapter = ToInt(cnum);
                if (chapter <= 0) continue;
                std::string chapTitle = bookHeading;
                if (const auto* t = chNode.FindChild("t"))
                    chapTitle = Collapse(Trim(t->text));
                std::string heading;
                for (const auto& vNode : chNode.children) {
                    if (TagIs(vNode, "h")) {
                        heading = Collapse(Trim(vNode.text));
                        continue;
                    }
                    if (!TagIs(vNode, "verse") && !TagIs(vNode, "v") && !TagIs(vNode, "vers"))
                        continue;
                    std::string vnum = vNode.Attr("number");
                    if (vnum.empty()) vnum = vNode.Attr("num");
                    if (vnum.empty()) vnum = AttrNoCase(vNode, "vnumber");
                    int verse = ToInt(vnum);
                    if (verse <= 0) continue;
                    BibleVerse bv;
                    bv.bookId = id;
                    bv.chapter = chapter;
                    bv.verse = verse;
                    bv.text = Collapse(Trim(vNode.text));
                    bv.heading = heading;
                    heading.clear();
                    out.verses.push_back(std::move(bv));
                }
            }
        }
        if (out.verses.empty())
            return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                               "XML contains no verses");
        out = Finalize(std::move(out));
        return out;
    }
};

// ---------------------------------------------------------------------------
// JSON provider — the platform's structured interchange shape.
// ---------------------------------------------------------------------------
class JsonBibleProvider final : public IBibleProvider {
public:
    const char* Name() const noexcept override { return "bible-json"; }
    const char* Format() const noexcept override { return "json"; }
    std::vector<std::string> SupportedExtensions() const override { return {".json"}; }

    Result<BibleVersion> Parse(std::string_view source, std::string_view) const override {
        auto parsed = json::Parse(std::string(source));
        if (!parsed.ok())
            return Error::Make(Err::Bible_CorruptFile, "BibleProvider",
                               "invalid JSON: " + parsed.error().message);
        const json::Value& root = parsed.value();

        BibleVersion out;
        if (const auto* meta = root.Find("metadata")) {
            out.metadata.id = std::string(meta->Find("id") ? meta->Find("id")->asString() : "");
            out.metadata.name = std::string(meta->Find("name") ? meta->Find("name")->asString() : "");
            out.metadata.language =
                std::string(meta->Find("language") ? meta->Find("language")->asString() : "");
            out.metadata.copyright =
                std::string(meta->Find("copyright") ? meta->Find("copyright")->asString() : "");
            out.metadata.abbreviation =
                std::string(meta->Find("abbreviation") ? meta->Find("abbreviation")->asString() : "");
            // Common Bible-JSON dumps name these differently: "module" is the short id
            // ("kjv"), "shortname" the abbreviation ("KJV"), "lang_short" the language.
            auto fallback = [&](std::string& field, const char* key) {
                if (field.empty())
                    if (const auto* v = meta->Find(key)) field = std::string(v->asString());
            };
            fallback(out.metadata.id, "module");
            fallback(out.metadata.abbreviation, "shortname");
            fallback(out.metadata.language, "lang_short");
            out.metadata.source = "json";
        }
        if (const auto* booksArr = root.Find("books")) {
            if (const auto* arr = booksArr->asArray()) {
                int order = 1;
                for (const auto& bv : *arr) {
                    BibleBook b;
                    b.id = std::string(bv.Find("id") ? bv.Find("id")->asString() : "");
                    b.name = std::string(bv.Find("name") ? bv.Find("name")->asString() : b.id);
                    b.testament = std::string(
                        bv.Find("testament") ? bv.Find("testament")->asString() : "");
                    b.order = order++;
                    if (const auto* al = bv.Find("aliases")) {
                        if (const auto* alArr = al->asArray())
                            for (const auto& a : *alArr)
                                b.aliases.push_back(std::string(a.asString()));
                    }
                    if (!b.id.empty()) out.books.push_back(std::move(b));
                }
            }
        }
        const auto* versesArr = root.Find("verses");
        if (!versesArr || !versesArr->asArray())
            return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                               "JSON has no \"verses\" array");
        for (const auto& vv : *versesArr->asArray()) {
            BibleVerse bv;
            bv.bookId = std::string(vv.Find("book") ? vv.Find("book")->asString()
                                                    : (vv.Find("bookId") ? vv.Find("bookId")->asString() : ""));
            // "book" may be the canonical 1-based book number (1 = Genesis ... 66 = Revelation).
            if (bv.bookId.empty())
                if (const auto* num = vv.Find("book"); num && num->type() == json::Value::Type::Number) {
                    const auto& canon = DefaultBooks();
                    const long long n = num->asInt();
                    if (n >= 1 && n <= static_cast<long long>(canon.size()))
                        bv.bookId = canon[static_cast<size_t>(n - 1)].id;
                }
            bv.chapter = static_cast<int>(vv.Find("chapter") ? vv.Find("chapter")->asInt() : 0);
            bv.verse = static_cast<int>(vv.Find("verse") ? vv.Find("verse")->asInt() : 0);
            bv.text = Collapse(std::string(vv.Find("text") ? vv.Find("text")->asString() : ""));
            bv.heading = std::string(vv.Find("heading") ? vv.Find("heading")->asString() : "");
            bv.redLetter = vv.Find("redLetter") ? vv.Find("redLetter")->asBool() : false;
            if (bv.bookId.empty() || bv.chapter <= 0 || bv.verse <= 0)
                return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                                   "verse missing book/chapter/verse");
            EnsureBook(out.books, bv.bookId);
            out.verses.push_back(std::move(bv));
        }
        out = Finalize(std::move(out));
        return out;
    }
};

// ---------------------------------------------------------------------------
// USFM provider — \id, \h, \c, \v, \s, \f.
// ---------------------------------------------------------------------------
class UsfmBibleProvider final : public IBibleProvider {
public:
    const char* Name() const noexcept override { return "usfm"; }
    const char* Format() const noexcept override { return "usfm"; }
    std::vector<std::string> SupportedExtensions() const override { return {".usfm", ".sfm"}; }

    Result<BibleVersion> Parse(std::string_view source, std::string_view) const override {
        BibleVersion out;
        out.metadata.source = "usfm";
        std::string bookId, bookName;
        int chapter = 0;

        // Tokenize on '\' directives. Verse text accumulates until the next
        // directive; footnotes (\f ... \f*) are captured and stripped. A
        // section heading (\s) applies to the next verse created (USFM
        // semantics: \s precedes the verses it heads).
        size_t i = 0;
        std::string verseText;
        int verseNum = 0;
        std::string pendingHeading;        // set by \s for the next verse
        std::string verseHeading;          // captured per verse at creation
        auto flushVerse = [&]() {
            if (bookId.empty() || chapter <= 0 || verseNum <= 0) {
                verseText.clear();
                return;
            }
            BibleVerse bv;
            bv.bookId = bookId;
            bv.chapter = chapter;
            bv.verse = verseNum;
            bv.text = Collapse(Trim(verseText));
            bv.heading = verseHeading;
            out.verses.push_back(std::move(bv));
            verseText.clear();
            verseNum = 0;
            verseHeading.clear();
        };

        while (i < source.size()) {
            if (source[i] != '\\') {
                size_t next = source.find('\\', i);
                if (next == std::string_view::npos) {
                    verseText.append(source.substr(i));
                    i = source.size();
                } else {
                    verseText.append(source.substr(i, next - i));
                    i = next;
                }
                continue;
            }
            ++i;   // skip '\'
            if (i >= source.size()) break;
            // Directive token = letters (marker includes leading '+' for notes).
            size_t tStart = i;
            while (i < source.size() && std::isalpha(static_cast<unsigned char>(source[i])))
                ++i;
            std::string dir(source.substr(tStart, i - tStart));
            if (dir.empty()) { verseText.push_back('\\'); continue; }
            // Rest of the line is the payload.
            size_t lineEnd = source.find('\n', i);
            std::string rest = (lineEnd == std::string_view::npos)
                                   ? std::string(source.substr(i))
                                   : std::string(source.substr(i, lineEnd - i));
            i = (lineEnd == std::string_view::npos) ? source.size() : lineEnd + 1;

            if (dir == "id" || dir == "h" || dir == "toc1") {
                std::string val = Trim(rest);
                if (val.empty()) continue;
                if (dir == "id") {
                    flushVerse();
                    bookId = val;
                    bookName = val;
                    verseNum = 0;
                    EnsureBook(out.books, bookId);
                } else {
                    bookName = val;
                }
                for (auto& b : out.books)
                    if (b.id == bookId) b.name = bookName;
            } else if (dir == "c") {
                flushVerse();
                chapter = ToInt(Trim(rest));
                verseNum = 0;
            } else if (dir == "s" || dir == "ms") {
                pendingHeading = Collapse(Trim(rest));
            } else if (dir == "v") {
                flushVerse();
                // "\v 16 text" — skip the separating whitespace first.
                size_t sp = rest.find_first_not_of(" \t");
                std::string rest2 = sp == std::string::npos ? std::string() : rest.substr(sp);
                size_t numEnd = rest2.find(' ');
                verseNum = ToInt(numEnd == std::string::npos ? rest2 : rest2.substr(0, numEnd));
                verseText = numEnd == std::string::npos ? std::string() : rest2.substr(numEnd + 1);
                verseHeading = pendingHeading;   // \s before this verse heads it
                pendingHeading.clear();
            } else if (dir == "f" || dir == "fe" || dir == "x") {
                // Inline note: capture text up to the closing marker.
                size_t close = source.find("\\" + dir + "*", i);
                size_t take = (close == std::string_view::npos) ? verseText.size()
                                                                : close - i;
                verseText.append(source.substr(i, take));
                if (close != std::string_view::npos) {
                    i = close + dir.size() + 2;
                }
            }
        }
        flushVerse();
        if (out.verses.empty())
            return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                               "USFM contains no verses");
        out = Finalize(std::move(out));
        return out;
    }
};

// ---------------------------------------------------------------------------
// OSIS provider — <osis><osisText><div type="book"><chapter><verse>.
// ---------------------------------------------------------------------------
class OsisBibleProvider final : public IBibleProvider {
public:
    const char* Name() const noexcept override { return "osis"; }
    const char* Format() const noexcept override { return "osis"; }
    std::vector<std::string> SupportedExtensions() const override { return {".osis", ".xml"}; }

    Result<BibleVersion> Parse(std::string_view source, std::string_view) const override {
        auto parsed = xml::Parse(source);
        if (!parsed.ok()) return parsed.error();
        const XmlNode& root = parsed.value();
        if (root.tag != "osis")
            return Error::Make(Err::Bible_UnsupportedFormat, "BibleProvider",
                               "root element is not <osis>");
        const XmlNode* text = root.FindDescendant("osisText");
        if (!text) text = &root;

        BibleVersion out;
        out.metadata.source = "osis";
        std::string pendingHeading;

        // Iterate book divs (also tolerate books directly under osisText).
        std::vector<const XmlNode*> bookNodes;
        for (const auto& child : text->children)
            if (child.tag == "div" && child.Attr("type") == "book") bookNodes.push_back(&child);
        if (bookNodes.empty()) {
            // Fall back to treating every div as a book.
            for (const auto& child : text->children)
                if (child.tag == "div") bookNodes.push_back(&child);
        }
        for (const auto* bookNode : bookNodes) {
            std::string osisId = bookNode->Attr("osisID");
            std::string id = osisId;
            // Strip testament prefix if any ("Gen" -> "GEN"; keep as-is otherwise).
            std::string name = osisId;
            if (const auto* title = bookNode->FindChild("title"))
                name = Collapse(Trim(title->text));
            for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (id.empty()) id = name.empty() ? "BOOK" : name;
            EnsureBook(out.books, id);
            for (auto& b : out.books)
                if (b.id == id) {
                    b.name = name;
                    if (b.testament.empty()) b.testament = (b.order <= 39) ? "old" : "new";
                }
            for (const auto& chNode : bookNode->children) {
                if (chNode.tag != "chapter") continue;
                int chapter = ToInt(chNode.Attr("number") != "" ? chNode.Attr("number")
                                                                : chNode.Attr("osisID"));
                if (chapter <= 0) {
                    // osisID "Gen.1" -> trailing number.
                    std::string oid = chNode.Attr("osisID");
                    size_t dot = oid.rfind('.');
                    if (dot != std::string::npos) chapter = ToInt(oid.substr(dot + 1));
                }
                if (chapter <= 0) continue;
                for (const auto& vNode : chNode.children) {
                    if (vNode.tag == "title") {
                        pendingHeading = Collapse(Trim(vNode.text));
                        continue;
                    }
                    if (vNode.tag != "verse") continue;
                    int verse = ToInt(vNode.Attr("number") != "" ? vNode.Attr("number")
                                                                 : vNode.Attr("osisID"));
                    if (verse <= 0) {
                        std::string oid = vNode.Attr("osisID");
                        size_t dot = oid.rfind('.');
                        if (dot != std::string::npos) verse = ToInt(oid.substr(dot + 1));
                    }
                    if (verse <= 0) continue;
                    BibleVerse bv;
                    bv.bookId = id;
                    bv.chapter = chapter;
                    bv.verse = verse;
                    bv.text = Collapse(Trim(vNode.text));
                    bv.heading = pendingHeading;
                    pendingHeading.clear();
                    out.verses.push_back(std::move(bv));
                }
            }
        }
        if (out.verses.empty())
            return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                               "OSIS contains no verses");
        out = Finalize(std::move(out));
        return out;
    }
};

// ---------------------------------------------------------------------------
// Plain text provider — "GEN 1:1 In the beginning ...".
// ---------------------------------------------------------------------------
class PlainTextBibleProvider final : public IBibleProvider {
public:
    const char* Name() const noexcept override { return "plain-text"; }
    const char* Format() const noexcept override { return "txt"; }
    std::vector<std::string> SupportedExtensions() const override { return {".txt"}; }

    Result<BibleVersion> Parse(std::string_view source, std::string_view) const override {
        BibleVersion out;
        out.metadata.source = "txt";
        std::istringstream lines{std::string(source)};
        std::string line;
        std::string bookId;
        std::string bookName;
        while (std::getline(lines, line)) {
            std::string t = Trim(line);
            if (t.empty()) continue;
            // "GEN 1:1 text"
            size_t sp = t.find(' ');
            if (sp == std::string::npos || sp == 0) continue;
            std::string bookTok = t.substr(0, sp);
            std::string rest = Collapse(Trim(t.substr(sp + 1)));
            size_t colon = rest.find(':');
            if (colon == std::string::npos) continue;
            int chapter = ToInt(Trim(rest.substr(0, colon)));
            size_t vEnd = colon + 1;
            while (vEnd < rest.size() && std::isdigit(static_cast<unsigned char>(rest[vEnd])))
                ++vEnd;
            int verse = ToInt(rest.substr(colon + 1, vEnd - colon - 1));
            std::string text = Collapse(Trim(rest.substr(vEnd)));
            if (chapter <= 0 || verse <= 0 || text.empty()) continue;

            // Resolve the book token through the canonical table.
            std::vector<BibleBook> canon = DefaultBooks();
            auto r = ReferenceResolver::Resolve(bookTok, canon);
            if (r.ok()) {
                bookId = r.value().bookId;
                bookName = r.value().bookName;
            } else {
                if (bookId != bookTok) {
                    bookId = bookTok;
                    for (char& c : bookId) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    bookName = bookTok;
                }
            }
            EnsureBook(out.books, bookId);
            for (auto& b : out.books)
                if (b.id == bookId) { b.name = bookName; break; }
            BibleVerse bv;
            bv.bookId = bookId;
            bv.chapter = chapter;
            bv.verse = verse;
            bv.text = text;
            out.verses.push_back(std::move(bv));
        }
        if (out.verses.empty())
            return Error::Make(Err::Bible_ValidationFailed, "BibleProvider",
                               "text contains no recognizable verses");
        out = Finalize(std::move(out));
        return out;
    }
};

} // namespace

std::shared_ptr<IBibleProvider> CreateXmlBibleProvider() {
    return std::make_shared<XmlBibleProvider>();
}
std::shared_ptr<IBibleProvider> CreateJsonBibleProvider() {
    return std::make_shared<JsonBibleProvider>();
}
std::shared_ptr<IBibleProvider> CreateUsfmBibleProvider() {
    return std::make_shared<UsfmBibleProvider>();
}
std::shared_ptr<IBibleProvider> CreateOsisBibleProvider() {
    return std::make_shared<OsisBibleProvider>();
}
std::shared_ptr<IBibleProvider> CreatePlainTextBibleProvider() {
    return std::make_shared<PlainTextBibleProvider>();
}

} // namespace bps::bible
