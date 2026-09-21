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

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::library {

struct TheTableVerse {
    int number = 0;          // 1-based paragraph index within the chapter
    std::string text;
    std::string heading;     // sermon title on verse 1 (display only)
};

struct TheTableChapter {
    int number = 0;          // 1-based sermon index within the year
    std::string title;       // "0217 Only Believe" (from the file name)
    std::vector<TableVerse> verses;
};

struct TheTableBook {
    std::string id;          // "Y1953"
    std::string name;        // "1953" (display)
    int order = 0;
    std::vector<TableChapter> chapters;
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
    // One chapter (paragraphs in order); error when the book/chapter is unknown.
    Result<TheTableChapter> GetChapter(std::string_view bookId, int chapter) const;
    size_t VerseCount() const;

    // --- Search -------------------------------------------------------------------
    // Whole-library word search (case-insensitive, ALL terms must appear in a
    // paragraph). Best-first by term coverage. `limit` caps the result count.
    Result<std::vector<TheTableSearchHit>> Search(std::string_view query, size_t limit = 60) const;

    // --- Import (the "New sermon" flow: .txt or .pdf) ------------------------------
    // `fileName` supplies the year + sermon title when the content itself does
    // not ("1953/53_0217_Only_Believe.pdf" -> year 1953, "0217 Only Believe").
    // Returns the new verse's reference ("1953 12:1") on success.
    Result<std::string> ImportSermon(std::string_view fileName, std::string_view content);

    // --- Persistence ---------------------------------------------------------------
    Result<void> Save() const;   // user-dir JSON (atomic write)
    Result<void> Load();

private:
    explicit TheTableLibrary(std::string filePath);

    TheTableBook& BookForYear(std::string_view year);
    std::string NextFilePath() const;

    mutable std::mutex mutex_;
    std::string filePath_;
    std::vector<TableBook> books_;
    uint32_t importSeq_ = 0;   // disambiguates same-named sermons
};

} // namespace bps::library
