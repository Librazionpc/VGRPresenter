#pragma once

// TheTableLibrary (the sermon library behind "The Table" tab) — a REFERENCE
// library with the same architecture as the Bible: books -> chapters ->
// verses, where a sermon's year is the book, the sermon is the chapter and
// each paragraph is a verse. It is a deliberate SIBLING of modules/bible, not
// an extension of it: BibleEngine stays scripture-only. This module reuses
// the same shapes (book/chapter/verse, "BOOK C:V" reference strings, scored
// search hits) so the UI's library-browser architecture serves both tabs, and
// each imports through its own importer:
//
//   Scripture:  BibleEngine     <- zefania/json/usfm/... providers
//   The Table:  TheTableLibrary <- txt / pdf sermon importer (this module)
//
// PDF text extraction is built in (raw-text + FlateDecode streams through the
// engine's zlib DeflateCompressor); a sermon PDF parsed at import time
// contributes its paragraphs as verses. Books are years, chapters are
// sermons; the sermon title becomes chapter 1's heading. Persistence: one
// JSON document in the user dir (same convention as the design libraries).
// A .txt/.pdf import UPDATES the library in place (year book created when
// missing, sermon appended as the next chapter).

#include "core/common/Common.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::search {
class SearchEngine;
}

namespace bps::library {

struct TheTableVerse {
    int number = 0;          // 1-based paragraph index within the chapter
    std::string text;
    std::string heading;     // sermon title on verse 1 (display only)
};

struct TheTableChapter {
    int number = 0;          // 1-based sermon index within the year
    std::string title;       // "0217 Only Believe" (from the file name)
    std::vector<TheTableVerse> verses;
};

struct TheTableBook {
    std::string id;          // "Y1953"
    std::string name;        // "1953" (display)
    int order = 0;
    std::vector<TheTableChapter> chapters;
};

struct TheTableSearchHit {
    std::string reference;   // "1953 12:3" (book display name + chapter + verse)
    std::string bookId;
    int chapter = 0;
    int verse = 0;
    std::string snippet;     // the hit's paragraph (trimmed)
    double score = 0.0;
};

class TheTableLibrary final {
public:
    // Opens (or creates) the library persisted at `filePath`.
    static std::shared_ptr<TheTableLibrary> Open(const std::string& filePath);

    // --- Browsing -----------------------------------------------------------------
    std::vector<TheTableBook> Books() const;
    // The years and their sermons WITHOUT the words. Books() copies every paragraph of every sermon; once hundreds of sermons are in, that
    // is a copy of the whole library, so anything that only lists (the year list, a reference lookup) asks for this instead.
    struct ChapterInfo {
        int number = 0;
        std::string title;
        size_t verseCount = 0;
    };
    struct BookInfo {
        std::string id;
        std::string name;
        int order = 0;
        std::vector<ChapterInfo> chapters;
    };
    std::vector<BookInfo> BookIndex() const;
    // One chapter (paragraphs in order); error when the book/chapter is unknown.
    Result<TheTableChapter> GetChapter(std::string_view bookId, int chapter) const;
    size_t VerseCount() const;

    // --- Search -------------------------------------------------------------------
    // Whole-library word search (case-insensitive, ALL terms must appear in a
    // paragraph). Best-first by term coverage. `limit` caps the result count.
    Result<std::vector<TheTableSearchHit>> Search(std::string_view query, size_t limit = 60) const;

    // --- Search Engine integration (docs/specs/24 §Search, the Bible pattern) -----
    // Registers this library's index adapter with the platform Search Engine and
    // indexes every sermon as ONE document (id "table:<bookId>:<chapter>") whose
    // content is the sermon's title + every paragraph's text. One document per
    // sermon, not per paragraph: 1,206 sermons index cleanly where 300k paragraph
    // documents would not (the engine re-copies every candidate document per
    // query); paragraph-level hits stay on this library's own Search().
    // Incremental: a re-index upserts, so other content stays searchable. Safe to
    // call repeatedly (a second call re-upserts the same ids).
    Result<size_t> IndexWithSearchEngine();
    // Drops this library's documents ("table:*") from the Search Engine index.
    Result<void> UnindexFromSearchEngine();

    // --- Import (the "New sermon" flow: .txt or .pdf) ------------------------------
    // `fileName` supplies the year + sermon title when the content itself does
    // not ("1953/53_0217_Only_Believe.pdf" -> year 1953, "0217 Only Believe").
    // Returns the new verse's reference ("1953 12:1") on success.
    Result<std::string> ImportSermon(std::string_view fileName, std::string_view content);

    // The clean-up stage every import goes through first: a .pdf (its text read out) or a .txt (of any encoding) becomes plain UTF-8
    // text - running heads, page numbers, control characters and stray spacing removed, one paragraph per block, a blank line between.
    // Stores nothing; fails when the file has no readable text.
    static Result<std::string> ConvertToCleanText(std::string_view fileName, std::string_view content);

    // Whole-folder import (the "Add sermons folder" flow): every .pdf/.txt
    // under `folderPath`, recursively, imported by the same name mapping.
    // `progress` fires per file (done, total, current name); returns the
    // imported / skipped-duplicate / failed counts.
    // With `convertedRoot`, each PDF's clean text is also kept there as "<convertedRoot>/<version>/<year>/<name>.txt" (to read and check),
    // and reused on a later import while it is newer than the PDF.
    struct ImportReport {
        int imported = 0;
        int skipped = 0;   // a sermon with the same year + title is already in
        int failed = 0;
    };
    Result<ImportReport> ImportFolder(
        std::string_view folderPath,
        const std::function<void(int done, int total, std::string_view current)>& progress = {},
        std::string_view convertedRoot = {});

    // --- Persistence ---------------------------------------------------------------
    Result<void> Save() const;   // user-dir JSON (atomic write)
    Result<void> Load();

private:
    explicit TheTableLibrary(std::string filePath);

    TheTableBook& BookForYear(std::string_view year);
    // Adds one sermon's clean text (see ConvertToCleanText) to the books WITHOUT saving; returns its reference ("1953 12:1").
    // With `replaceUnreadable`, a stored sermon of the same title that does not read as text (see ReadableText) is replaced.
    Result<std::string> AddCleanText(std::string_view fileName, const std::string& text, bool replaceUnreadable = false);
    // Writes a snapshot of the books to the library file. It takes no lock: it works on the copy it is given, so browsing is never
    // held up behind a file write.
    Result<void> Persist(const std::vector<TheTableBook>& books) const;
    std::string NextFilePath() const;

    mutable std::mutex mutex_;
    std::string filePath_;
    std::vector<TheTableBook> books_;
    uint32_t importSeq_ = 0;   // disambiguates same-named sermons
};

} // namespace bps::library
