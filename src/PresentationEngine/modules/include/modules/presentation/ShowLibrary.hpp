#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bps::presentation {
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