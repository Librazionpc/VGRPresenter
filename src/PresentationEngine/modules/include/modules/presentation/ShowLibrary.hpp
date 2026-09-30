#pragma once

// ShowLibrary (docs/architecture/ProjectSystem.md §UX state): the folder of
// shows a user organises and finds them in. A library is a directory:
//
//   <root>/Sunday Service.vgr            uncategorised show
//   <root>/Youth/Friday Night.vgr        show in the "Youth" category
//   <root>/Conferences/Easter 2027.vgr   show in the "Conferences" category
//
// Categories are plain sub-folders (one level) and shows are ordinary .vgr
// files, so the library is transparent — it can be browsed, backed up and synced
// with normal file tools, and nothing here can lose a show that a database
// couldn't. This is a LIBRARY-level category (which shows belong together); it
// is unrelated to the categories INSIDE a show (songs / notes / pastor notes),
// which live in the show itself (Presentation::categories).
//
// The index is built by reading each file's .vgr header; a file that cannot be
// read (damaged, not a show) is listed in Problems() rather than silently
// missing, so the user can see why a show has not appeared.

#include "core/common/Common.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
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

    std::vector<std::string> Categories() const;                 // sorted, case-insensitive
    std::vector<ShowEntry> Shows() const;                        // every show, newest first
    std::vector<ShowEntry> ShowsIn(std::string_view category) const;   // "" = uncategorised
    // Case-insensitive match on show name or category name.
    std::vector<ShowEntry> Search(std::string_view text) const;
    // Files that could not be read as shows, as "<path>: <reason>".
    std::vector<std::string> Problems() const;

    // --- Organising ---
    // Names are folder names: no path separators or reserved characters, not
    // empty, not "." / "..". Returns InvalidArgument otherwise.
    Result<void> CreateCategory(std::string_view name);
    Result<void> RenameCategory(std::string_view from, std::string_view to);
    Result<void> RemoveCategory(std::string_view name);          // only when empty
    // Moves every show in `name` to the recoverable .deleted bin (one rescan).
    // A no-op when the category does not exist. Re-imports use it to replace
    // the previous batch instead of piling duplicates beside it.
    Result<void> ClearCategory(std::string_view name);
    // Moves a show into `category` ("" = out of every category). Returns the
    // show's new path. Fails (leaving the file where it is) if a show with that
    // file name already exists there.
    Result<std::string> MoveShow(std::string_view path, std::string_view category);
    // ---- Show CRUD (create = NewShowPath + PresentationDocument::Save; read =
    // Open; update = Save) ----
    // Renames the show: its stored name AND its file name (kept unique in its
    // folder). Returns the new path.
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
    Result<void> ScanLocked();
    std::string CategoryDir(std::string_view category) const;

    std::string root_;
    mutable std::mutex mutex_;
    std::vector<ShowEntry> entries_;
    std::vector<std::string> categories_;
    std::vector<std::string> problems_;
};

} // namespace bps::presentation
