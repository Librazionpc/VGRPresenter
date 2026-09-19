#pragma once

// Bible Engine canonical model (Phase 12, docs/specs/24). The internal,
// format-independent representation every provider converts into. The engine
// never exposes USFM/OSIS/XML/JSON shapes to the rest of the platform — only
// these types.

#include "core/common/Common.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::bible {

// --- Translation / version metadata -------------------------------------------
struct TranslationMetadata {
    std::string id;             // stable id ("KJV", "vgr", ...)
    std::string name;           // display name
    std::string language;
    std::string copyright;
    std::string license;
    std::string source;         // origin description
    std::string version;        // translation version string
    std::string abbreviation;   // short tag ("KJV", "WMB")
    bool readOnly = false;
};

// --- Book -----------------------------------------------------------------------
struct BibleBook {
    std::string id;              // stable ("GEN", "PSA", "MRK", "1CO", ...)
    std::string name;            // "Genesis", "1 Corinthians"
    std::string testament;       // "old" | "new"
    std::vector<std::string> aliases;   // "Gen", "Gn", "Ge", ...
    int order = 0;
};

// --- Footnotes & cross references ------------------------------------------------
struct Footnote {
    std::string marker;          // "*", "a", ...
    std::string text;
};

struct CrossReference {
    std::string reference;       // "JHN 3:16"
    std::string text;            // optional label/note
};

// --- Verse / chapter ---------------------------------------------------------------
struct BibleVerse {
    std::string bookId;
    int chapter = 0;
    int verse = 0;
    std::string text;
    std::string heading;         // section heading when this verse starts one
    bool redLetter = false;
    std::vector<Footnote> footnotes;
    std::vector<CrossReference> crossRefs;
};

struct BibleChapter {
    std::string bookId;
    int number = 0;
    std::string title;           // chapter heading
    std::vector<BibleVerse> verses;
};

// --- A full installed translation ---------------------------------------------------
struct BibleVersion {
    TranslationMetadata metadata;
    std::vector<BibleBook> books;                        // ordered by `order`
    std::map<std::string, BibleChapter, std::less<>> chapters;   // "GEN.1" -> chapter
    std::vector<BibleVerse> verses;                      // flattened, canonical order
};

// --- Passage reference (docs/specs/24 §Reference Resolution) -----------------------
struct PassageRef {
    std::string bookId;
    std::string bookName;        // display name resolved from the book table
    int chapter = 0;
    int verseStart = 0;          // 0 = whole chapter
    int verseEnd = 0;            // 0 = single verse (or whole chapter when verseStart == 0)
    std::string raw;             // original text, for round-tripping

    bool Valid() const { return !bookId.empty(); }
    bool IsWholeBook() const { return chapter <= 0; }
    bool IsWholeChapter() const { return !IsWholeBook() && verseStart <= 0; }
    // Canonical reference label, e.g. "JHN 3:16" / "PSA 23" / "GEN 1:1-10".
    std::string ToString() const;
};

// --- A resolved passage (what the query engine returns) ------------------------------
struct Passage {
    PassageRef ref;
    std::vector<BibleVerse> verses;
};

// --- Parallel Bible result -------------------------------------------------------------
struct ParallelVerse {
    std::string bibleId;
    std::string reference;       // "JHN 3:16"
    bool present = false;        // false when the version lacks this verse
    std::string text;
};

// --- User data (kept separate from Scripture text) -------------------------------------
struct UserNote {
    std::string id;
    PassageRef ref;
    std::string text;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
};

// --- Search hit --------------------------------------------------------------------------
struct BibleSearchHit {
    std::string bibleId;
    std::string reference;       // "JHN 3:16"
    std::string bookId;
    int chapter = 0;
    int verse = 0;
    std::string snippet;
    double score = 0.0;
};

// --- Import / formatting options -----------------------------------------------------------
struct ImportOptions {
    std::string id;              // override translation id ("" = from source)
    std::string name;            // override display name
    bool index = true;           // auto-index into the Search Engine
    bool replace = false;        // replace an existing Bible with the same id
};

struct FormatOptions {
    enum class Mode : int { Paragraph = 0, VersePerLine };
    Mode mode = Mode::Paragraph;
    bool includeNumbers = false;     // prefix verses with numbers
    bool includeHeadings = true;
    bool includeFootnotes = false;
    bool redLetterMarkers = false;   // wrap red-letter text in markers
};

} // namespace bps::bible
