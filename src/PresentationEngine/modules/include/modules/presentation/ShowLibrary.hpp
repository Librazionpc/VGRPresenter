#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bps::presentation {
struct Presentation;
struct ShowEntry {
    std::string path;        // absolute path of the .vgr file
    std::string id;          // the show's UUID (from its header)
    std::string name;        // the show's name (from its header)
    std::string category;    // library category ("" = uncategorised)
    int64_t modifiedMs = 0;
    int slideCount = 0;
};

class ShowLibrary {
public:
    explicit ShowLibrary(std::string rootDir);

    const std::string& Root() const { return root_; }

    // Re-scans the library (creating the root folder if it does not exist yet).
    Result<void> Refresh();

    // THE LIST BEFORE THE SCAN. A scan opens and parses every .vgr in the library (4000+ files, often in
    // a cloud-synced folder), which takes seconds cold — and the Shows pane sat EMPTY for all of it. The
    // result of the last scan is persisted (<cache dir>/show-library-index): this loads it at once, so the
    // pane has its list the moment the engine is up, and Refresh() then re-reads ONLY the files whose
    // size or modification time changed (a stat per file, not a parse). Returns the entries loaded.
    size_t LoadCachedIndex();

    std::vector<std::string> Categories() const;                 // sorted, case-insensitive
    std::vector<ShowEntry> Shows() const;                        // every show, newest first
    std::vector<ShowEntry> ShowsIn(std::string_view category) const;   // "" = uncategorised
    // Case-insensitive match on show name or category name.
    std::vector<ShowEntry> Search(std::string_view text) const;
    // Files that could not be read as shows, as "<path>: <reason>".
    std::vector<std::string> Problems() const;

    Result<void> CreateCategory(std::string_view name);
    Result<void> RenameCategory(std::string_view from, std::string_view to);
    // Moves its shows into Unlabeled, then removes the empty category folder.
    Result<void> RemoveCategory(std::string_view name);
    Result<void> ClearCategory(std::string_view name);
    Result<std::string> MoveShow(std::string_view path, std::string_view category);
    // ---- Show CRUD (create = NewShowPath + PresentationDocument::Save; read =
    Result<std::string> RenameShow(std::string_view path, std::string_view newName);
    // A copy with a fresh identity, in the same category, named "<name> copy"
    // (or `newName`). Returns the copy's path.
    Result<std::string> DuplicateShow(std::string_view path, std::string_view newName = {});
    // Deleting is recoverable: the file moves to <root>/.deleted/ (never scanned
    // as a category). Returns its new path. EmptyDeleted() removes them for good.
    Result<std::string> DeleteShow(std::string_view path);
    Result<void> EmptyDeleted();
    // A free path for a new show called `showName` in `category` — sanitised
    // file name, never overwrites an existing file ("Name (2).vgr"...).
    Result<std::string> NewShowPath(std::string_view category, std::string_view showName) const;
    // THE NAME A SECOND SHOW OF THE SAME NAME GETS — the way a file system numbers a copy: "Name", then "Name (2)",
    // "Name (3)"... It returns `showName` itself when no show of that name is in `category` yet, else the first
    // "Name (n)" (n >= 2) that is free; a name that already ends in " (k)" is numbered from its base. The SHOW's name
    // carries the number as well as its file, so the copies can be told apart in every list.
    std::string NumberedShowName(std::string_view category, std::string_view showName) const;

    // THE SAME SHOW, ALREADY HERE. The path of a show in `category` that has this show's name AND exactly its
    // slides (every slide's title, text and block text, in order), or "" when there is none. Importing the same
    // file twice used to write "Name (2).vgr", "Name (3).vgr"... — a library of 1,400 songs listed more than
    // once. The caller skips the write when this finds the show; a same-named show whose words differ is
    // a different show and is still saved beside it.
    std::string FindSameShow(std::string_view category, const Presentation& show) const;

    // CLEARING WHAT IS ALREADY THERE. Finds shows with the same name and exactly the same words and keeps ONE of
    // each: within a folder the original ("Name.vgr", else the lowest "(N)", else the oldest); an uncategorised
    // copy of a show that also sits in a named folder goes too. Two NAMED folders that both hold a song are
    // left alone — each is its own hymnal. Nothing is deleted for good: the extras move to <root>/.deleted/
    // (the same bin DeleteShow uses), and the library is re-scanned once at the end.
    struct DuplicateReport {
        int groups = 0;       // songs that existed more than once
        int removed = 0;      // copies moved to the bin
        int unreadable = 0;   // files that could not be read (left where they are)
    };
    Result<DuplicateReport> RemoveDuplicates();

    // Turns arbitrary text into a safe file/folder name (used by NewShowPath).
    static std::string SafeName(std::string_view text);

private:
    // One file as the last scan saw it: the stat that proves it is unchanged, and what its header said.
    struct IndexRec {
        uint64_t size = 0;
        int64_t mtimeNs = 0;
        ShowEntry entry;
    };
    using IndexMap = std::unordered_map<std::string, IndexRec>;

    // What one filesystem pass produces.
    struct Scanned {
        std::vector<ShowEntry> entries;
        std::vector<std::string> categories;
        std::vector<std::string> problems;
        IndexMap index;   // the records of this pass (what is persisted)
    };
    Result<void> ScanUnlocked(Scanned& out, const IndexMap& previous) const;
    std::string IndexPath() const;
    void SaveIndex(const Scanned& scanned) const;
    Result<void> Scan();
    std::string CategoryDir(std::string_view category) const;

    std::string root_;
    mutable std::mutex mutex_;
    std::vector<ShowEntry> entries_;
    std::vector<std::string> categories_;
    std::vector<std::string> problems_;
    IndexMap index_;   // the persisted/last-scan records, guarded by mutex_
};
} // namespace bps::presentation