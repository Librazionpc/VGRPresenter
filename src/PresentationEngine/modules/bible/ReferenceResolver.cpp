#include "modules/bible/ReferenceResolver.hpp"

#include <algorithm>
#include <cctype>

namespace bps::bible {

namespace {

// Canonical 66-book table (docs/specs/24 §Reference Resolution). Serves both
// as the fallback abbreviation source and the name/id table when no Bible is
// loaded yet, so "John 3:16" resolves against canonical spellings alone.
struct CanonicalBook {
    const char* id;        // "JHN"
    const char* name;      // "john" (lowercase, for matching)
    const char* abbrev;    // canonical abbreviation ("jn")
};

const CanonicalBook kCanonicalBooks[] = {
    {"GEN", "genesis", "gen"},   {"EXO", "exodus", "ex"},
    {"LEV", "leviticus", "lev"}, {"NUM", "numbers", "num"},
    {"DEU", "deuteronomy", "deut"}, {"JOS", "joshua", "josh"},
    {"JDG", "judges", "jdg"},    {"RUT", "ruth", "rut"},
    {"1SA", "1 samuel", "1sam"}, {"2SA", "2 samuel", "2sam"},
    {"1KI", "1 kings", "1kgs"},  {"2KI", "2 kings", "2kgs"},
    {"1CH", "1 chronicles", "1chr"}, {"2CH", "2 chronicles", "2chr"},
    {"EZR", "ezra", "ezr"},      {"NEH", "nehemiah", "neh"},
    {"EST", "esther", "est"},    {"JOB", "job", "job"},
    {"PSA", "psalms", "psa"},    {"PRO", "proverbs", "prov"},
    {"ECC", "ecclesiastes", "ecc"}, {"SNG", "song of solomon", "sng"},
    {"ISA", "isaiah", "isa"},    {"JER", "jeremiah", "jer"},
    {"LAM", "lamentations", "lam"}, {"EZK", "ezekiel", "ezk"},
    {"DAN", "daniel", "dan"},    {"HOS", "hosea", "hos"},
    {"JOL", "joel", "jol"},      {"AMO", "amos", "amo"},
    {"OBA", "obadiah", "oba"},   {"JON", "jonah", "jon"},
    {"MIC", "micah", "mic"},     {"NAM", "nahum", "nam"},
    {"HAB", "habakkuk", "hab"},  {"ZEP", "zephaniah", "zep"},
    {"HAG", "haggai", "hag"},    {"ZEC", "zechariah", "zec"},
    {"MAL", "malachi", "mal"},
    {"MAT", "matthew", "mat"},   {"MRK", "mark", "mrk"},
    {"LUK", "luke", "luk"},      {"JHN", "john", "jhn"},
    {"ACT", "acts", "act"},      {"ROM", "romans", "rom"},
    {"1CO", "1 corinthians", "1co"}, {"2CO", "2 corinthians", "2co"},
    {"GAL", "galatians", "gal"}, {"EPH", "ephesians", "eph"},
    {"PHP", "philippians", "php"}, {"COL", "colossians", "col"},
    {"1TH", "1 thessalonians", "1th"}, {"2TH", "2 thessalonians", "2th"},
    {"1TI", "1 timothy", "1ti"}, {"2TI", "2 timothy", "2ti"},
    {"TIT", "titus", "tit"},     {"PHM", "philemon", "phm"},
    {"HEB", "hebrews", "heb"},   {"JAS", "james", "jas"},
    {"1PE", "1 peter", "1pe"},   {"2PE", "2 peter", "2pe"},
    {"1JN", "1 john", "1jn"},    {"2JN", "2 john", "2jn"},
    {"3JN", "3 john", "3jn"},    {"JUD", "jude", "jud"},
    {"REV", "revelation", "rev"},
    // Common alternate spellings (same id/name, extra matching keys).
    {"JHN", "john", "jn"},
    {"JHN", "john", "joh"},
    {"PSA", "psalms", "ps"},
    {"PSA", "psalms", "psal"},
    {"PSA", "psalms", "psalm"},
    {"1CO", "1 corinthians", "1 cor"},
    {"2CO", "2 corinthians", "2 cor"},
    {"1TH", "1 thessalonians", "1 thess"},
    {"2TH", "2 thessalonians", "2 thess"},
    {"1TI", "1 timothy", "1 tim"},
    {"2TI", "2 timothy", "2 tim"},
    {"MAT", "matthew", "matt"},
    {"1SA", "1 samuel", "1 sam"},
    {"2SA", "2 samuel", "2 sam"},
};

} // namespace

std::string ReferenceResolver::Normalize(std::string_view text) {
    std::string out;
    bool pendingSpace = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pendingSpace = !out.empty();
        } else {
            if (pendingSpace) out.push_back(' ');
            pendingSpace = false;
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

const char* ReferenceResolver::CanonicalAbbreviation(std::string_view bookId) {
    for (const auto& b : kCanonicalBooks)
        if (bookId == b.id) return b.abbrev;
    return "";
}

Result<PassageRef> ReferenceResolver::ParseBody(const std::string& normalized,
                                                const std::string& raw,
                                                const BibleBook& book) {
    PassageRef ref;
    ref.bookId = book.id;
    ref.bookName = book.name;
    ref.raw = raw;

    // Whole book ("John", "Psalms").
    if (normalized.empty()) return ref;

    // Chapter[:verseStart[-verseEnd]].
    size_t i = 0;
    int chapter = 0;
    while (i < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[i]))) {
        chapter = chapter * 10 + (normalized[i] - '0');
        ++i;
    }
    if (i == 0 || chapter <= 0)
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "no chapter after book name: " + raw);
    ref.chapter = chapter;

    if (i == normalized.size()) return ref;   // whole chapter

    if (normalized[i] != ':')
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "unexpected text after chapter: " + raw);
    ++i;

    int verse = 0;
    while (i < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[i]))) {
        verse = verse * 10 + (normalized[i] - '0');
        ++i;
    }
    if (verse <= 0)
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "no verse number: " + raw);
    ref.verseStart = verse;
    ref.verseEnd = verse;

    // Optional range: "16-18".
    if (i < normalized.size() && normalized[i] == '-') {
        ++i;
        int end = 0;
        while (i < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[i]))) {
            end = end * 10 + (normalized[i] - '0');
            ++i;
        }
        if (end < verse)
            return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                               "verse range is reversed: " + raw);
        ref.verseEnd = end;
    }
    if (i != normalized.size())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "trailing text in reference: " + raw);
    return ref;
}

Result<PassageRef> ReferenceResolver::Resolve(std::string_view text,
                                              const std::vector<BibleBook>& books) {
    const std::string normalized = Normalize(text);
    if (normalized.empty())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine", "empty reference");

    // Build the book lookup: the caller's books first (name, id, aliases),
    // plus the canonical table (name + abbreviation) as a fallback so common
    // spellings resolve even when a Bible omits aliases.
    struct Key {
        std::string key;
        BibleBook book;
    };
    std::vector<Key> keys;
    for (const auto& b : books) {
        std::string lowerId;
        for (char c : b.id) lowerId.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        std::string lowerName;
        for (char c : b.name) lowerName.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        keys.push_back({lowerId, b});
        keys.push_back({lowerName, b});
        for (const auto& a : b.aliases) {
            std::string lowerA;
            for (char c : a) lowerA.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            keys.push_back({lowerA, b});
        }
    }
    for (const auto& c : kCanonicalBooks) {
        BibleBook b;
        b.id = c.id;
        b.name = c.name;
        keys.push_back({c.name, b});
        keys.push_back({c.abbrev, b});
    }

    // Longest matching book key wins ("1 corinthians" beats "1 c", "corinthians").
    const Key* best = nullptr;
    size_t bestLen = 0;
    for (const auto& k : keys) {
        if (normalized.size() >= k.key.size() && normalized.starts_with(k.key)) {
            // The match must end at a word boundary (or consume the whole text).
            bool boundary = normalized.size() == k.key.size() ||
                            normalized[k.key.size()] == ' ' ||
                            normalized[k.key.size()] == ':' ||
                            normalized[k.key.size()] == ',';
            if (boundary && k.key.size() > bestLen) {
                best = &k;
                bestLen = k.key.size();
            }
        }
    }
    if (!best)
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "unknown book in reference: " + std::string(text));

    // Keep the caller's book metadata when available (aliases + testament).
    std::string body = normalized.substr(bestLen);
    if (!body.empty() && body[0] == ' ') body.erase(0, 1);
    return ParseBody(body, std::string(text), best->book);
}

Result<std::vector<PassageRef>> ReferenceResolver::ResolveAll(
    std::string_view text, const std::vector<BibleBook>& books) {
    const std::string normalized = Normalize(text);
    std::vector<PassageRef> out;
    size_t start = 0;
    while (start < normalized.size()) {
        // Split on ',' and ';'.
        size_t end = normalized.find_first_of(",;", start);
        std::string part = (end == std::string::npos)
                               ? normalized.substr(start)
                               : normalized.substr(start, end - start);
        std::string trimmed = Normalize(part);
        if (!trimmed.empty()) {
            if (auto r = Resolve(trimmed, books); r.ok())
                out.push_back(r.value());
            else
                return r.error();   // a malformed item fails the whole batch
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (out.empty())
        return Error::Make(Err::Bible_InvalidReference, "BibleEngine",
                           "no references resolved from: " + std::string(text));
    return out;
}

} // namespace bps::bible
