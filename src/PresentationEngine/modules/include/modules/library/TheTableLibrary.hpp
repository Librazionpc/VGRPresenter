#pragma once

#include "core/common/Common.hpp"

#include <atomic>
#include <array>
#include <functional>
#include <map>
#include <unordered_map>
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
    std::string code;        // the sermon's own date-code number ("0217"), when the file name carries one
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
    std::string fullText;    // matched paragraph (and successor when spanned)
    double score = 0.0;
    bool spanned = false;
};

// A reader's note on one paragraph, persisted with the library (the Bible
// module's UserNote, keyed the table way: "bookId:chapter:verse").
struct TableNote {
    std::string id;          // "note-<ms>" (the Bible module's shape)
    std::string ref;         // "1953 12:3" (the display reference as stored)
    std::string bookId;      // "Y1953"
    int chapter = 0;
    int verse = 0;
    std::string text;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
};

struct SermonParts {
    std::string year;     // the year book's display name: "1947"
    std::string code;     // "47-0412"
    std::string title;    // "Faith Is The Substance" (the stored code prefix removed)
    std::string citation; // the whole line: "47-0412 - Faith Is The Substance"
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
    // Whether there is any text at all: stops at the first paragraph (VerseCount walks every chapter of every year).
    bool HasVerses() const;

    Result<std::vector<TheTableSearchHit>> Search(std::string_view query, size_t limit = 60,
                                                 bool exactWords = false,
                                                 std::string_view scopeBookId = "",
                                                 int scopeChapter = 0,
                                                 const std::function<bool()>& cancelled = {}) const;

    std::string Citation(std::string_view bookId, int chapter, int verse = 0) const;

    std::string CitationCode(std::string_view bookId, int chapter) const;

    SermonParts Parts(std::string_view bookId, int chapter) const;

    static std::string CitationNeedle(std::string_view handle);

    Result<void> AddNote(std::string_view bookId, int chapter, int verse,
                         const std::string& text);
    // A note removed is a note that never happened — the JSON write-through
    // keeps the file truthful (the Bible module's RemoveNote, table-shaped).
    Result<void> RemoveNote(std::string_view bookId, int chapter, int verse);
    Result<std::vector<TableNote>> Notes(std::string_view bookId, int chapter,
                                         int verse) const;
    // Every note in the library, most-recently-modified first (the drawer).
    Result<std::vector<TableNote>> Notes() const;
    Result<void> SetHighlight(std::string_view bookId, int chapter, int verse, bool on);
    Result<std::vector<std::string>> Highlights() const;   // "Y1953:3:5" keys
    Result<bool> IsHighlighted(std::string_view bookId, int chapter, int verse) const;

    Result<size_t> IndexWithSearchEngine(const std::atomic<bool>* cancelled = nullptr);
    // Drops this library's documents ("table:*") from the Search Engine index.
    Result<void> UnindexFromSearchEngine();

    Result<std::string> ImportSermon(std::string_view fileName, std::string_view content);

    static bool IsSermonFile(std::string_view fileName, std::string_view content,
                             std::string& whyNot);

    static Result<std::string> ConvertToCleanText(std::string_view fileName, std::string_view content);

    struct ImportReport {
        int imported = 0;
        int skipped = 0;   // a sermon with the same year + title is already in
        int failed = 0;    // a .txt/.pdf that could not be read or parsed
        int unsupported = 0;
        std::vector<std::string> rejected;
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
    // Is this paragraph real? Called with mutex_ held (the user-data writes
    // validate before storing — a note on an unknown sermon is an error).
    bool VerseKnownLocked(std::string_view bookId, int chapter, int verse) const;
    // The citation line, mutex_ already held (the user-data writes build it).
    std::string CitationLocked(std::string_view bookId, int chapter, int verse) const;
    // Adds one sermon's clean text (see ConvertToCleanText) to the books WITHOUT saving; returns its reference ("1953 12:1").
    // With `replaceUnreadable`, a stored sermon of the same title that does not read as text (see ReadableText) is replaced.
    Result<std::string> AddCleanText(std::string_view fileName, const std::string& text, bool replaceUnreadable = false);
    // Writes a snapshot of the books to the library file. It takes no lock: it works on the copy it is given, so browsing is never
    // held up behind a file write.
    Result<void> Persist(const std::vector<TheTableBook>& books) const;
    std::string NextFilePath() const;

    mutable std::mutex mutex_;
    mutable std::mutex scanMutex_;
    std::string filePath_;
    std::vector<TheTableBook> books_;
    std::map<std::string, TableNote> notes_;
    std::map<std::string, bool> highlights_;
    int64_t lastNoteMs_ = 0;
    uint32_t importSeq_ = 0;   // disambiguates same-named sermons
    mutable std::unordered_map<std::string, std::vector<std::string>> lowerParagraphs_;
    // 16k bits keeps false positives low for a sermon with thousands of unique
    // words, while staying small enough to keep summaries for the whole library.
    using WordBloom = std::array<uint64_t, 256>;
    std::unordered_map<std::string, WordBloom> exactWordBlooms_;
    bool exactWordBloomsReady_ = false;
    uint64_t libraryRevision_ = 0;
};
} // namespace bps::library