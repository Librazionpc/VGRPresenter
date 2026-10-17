#pragma once

// MediaLibrary: the folders of images, videos and audio the user has added, and the files in
// them. Adding a folder is how media is brought in - it is scanned (recursively) and
// every image, video and audio file under it becomes an item the UI can list, with the thumbnails
// ThumbnailCache makes for it.
//
// Nothing is copied or moved: a folder stays where it is and only its LIST is stored
// (a small JSON file), so removing a folder just forgets it and never touches a file.
//
// What was FOUND in each folder is remembered too (an index file beside the folder list), so
// the next start shows every folder's files at once instead of an empty grid until the disk
// has been walked again. Load() restores that index and Scan() then refreshes it, so the
// library can disagree with the disk only until the first scan of a run finishes, and a file
// that has gone in the meantime just shows a placeholder until then.
//
// Thread-safety: every method may be called from any thread. Scan() does its slow
// work (walking the folder, reading file sizes) outside the lock, so the UI can keep
// listing folders and items while a scan runs on a worker.

#include "core/common/Common.hpp"
#include "modules/media/FrameSource.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bps::media {

struct LibraryItem {
    std::string id;         // stable per path (see MediaPathHash) - also names its cached thumbnails
    std::string path;       // absolute path of the file
    std::string name;       // file name without its extension
    MediaKind kind = MediaKind::Image;
    std::string folder;     // the library folder it was found under
    uint64_t sizeBytes = 0;
    int64_t modifiedMs = 0;
    // THIS FILE'S OWN FILL CHOICE ("contain" | "cover" | "fill", "" = none),
    // set by the operator on the file itself and WINNING over the output's own
    // default (Settings · Outputs → Fill) when it is on air. The operator saying
    // "this portrait photo, on every output, cropped" is a fact about the file,
    // not about one screen — so it is stored per file and travels with it.
    std::string fit;
};

struct LibraryFolder {
    std::string path;
    std::string name;       // the folder's own name ("Videos")
    size_t count = 0;       // items found by the last scan
    bool scanned = false;   // has Scan() finished for it in THIS run (its items may be remembered from the last)
};

// A folder INSIDE a library folder. The library keeps folders as the user added them; what lies
// beneath is discovered by the scan, so a parent folder shows everything under it and each nested
// folder can be opened on its own.
struct LibrarySubfolder {
    std::string path;       // absolute path of the nested folder
    std::string name;       // its own name
    std::string parent;     // the folder it is directly inside (the library folder itself, or another nested one)
    int depth = 1;          // 1 = directly inside a library folder
    size_t count = 0;       // files in it AND everything below it
};

// FreeShow's file-path hash: a 32-bit rolling hash rendered as "a<n>" (or "i<n>" for a
// negative value). Short, stable, and safe to use in a file name.
std::string MediaPathHash(std::string_view path);

// The kind of media a path is, judged by its extension; nullopt when it is not an image,
// video or audio file the library handles.
std::optional<MediaKind> MediaKindOf(std::string_view path);

class MediaLibrary {
public:
    // `storageFile`: where the folder list is kept (the parent folder is created on save).
    explicit MediaLibrary(std::string storageFile);

    // Reads the stored folder list and the remembered index of what was in each folder (a missing
    // file is an empty library, not an error; a damaged index is ignored - Scan() rebuilds it).
    Result<void> Load();

    // Adds a folder. InvalidArgument if it is not a folder, AlreadyExists if it (or the
    // same folder written differently) is already there. Saves the list.
    Result<void> AddFolder(std::string_view path);
    // Forgets a folder and its items. NotFound if it is not in the library.
    Result<void> RemoveFolder(std::string_view path);
    std::vector<LibraryFolder> Folders() const;   // in the order they were added

    // Walks a folder (recursively) and replaces its items with what is on disk now.
    // Returns how many were found. NotFound if the folder is not in the library or has
    // gone missing. Safe to run on a worker thread.
    Result<size_t> Scan(std::string_view path);

    // The folders inside a library folder, at any depth, in tree order (every folder is followed by what is
    // inside it). Only folders that hold media somewhere beneath them are listed. Empty until the folder
    // has been scanned (or restored from the remembered index).
    std::vector<LibrarySubfolder> Subfolders(std::string_view libraryFolder) const;

    // ---- the FILES themselves: rename, delete, move ----------------------------
    // The library never copies anything (a folder only keeps its LIST), so all
    // three of these are REAL file operations on the user's disk, and all three
    // are written the same way: resolve and act with the lock RELEASED (a PAL call
    // under mutex_ is exactly what the lock-scope rule forbids), then re-key the
    // remembered index under it and save. `path` must be an item the library found
    // (NotFound otherwise), and a `path` that is not a media item is refused.
    //
    // Renames the file to `newName` — a NAME, not a path (a separator is refused);
    // the old extension is kept when `newName` carries none, and a name already
    // taken in that folder is refused rather than overwritten.
    Result<void> RenameItem(std::string_view path, std::string_view newName);
    // Deletes the file from disk PERMANENTLY and drops it from the index. No undo:
    // the UI uses TrashItem for the operator's own Delete (which can be taken back),
    // and this is what deletes the copy the trash is holding.
    Result<void> RemoveItem(std::string_view path);
    // Deletes the file by MOVING it into the library's trash — a folder beside the
    // library's own storage, never inside a library folder, so no scan can find the
    // file again — and returns the TOKEN that puts it back. Nothing is destroyed, so
    // a Delete can offer Undo straight away. A token is good for THIS RUN: a restart
    // has nothing to put back, and the sweep in TrashItem clears what it was holding.
    Result<std::string> TrashItem(std::string_view path);
    // Puts a trashed file back where it came from and re-files it in the index (its
    // own folder, in that folder's order — so the grid does not shuffle because a
    // file came back). NotFound for a token this run does not know, or a trash file
    // that has gone; AlreadyExists when something has taken the file's name again —
    // the trashed copy stays, and the same token works once the name is free.
    Result<std::string> RestoreTrashedItem(std::string_view token);
    // Moves the file INTO `targetFolder`, which must be a library folder or a
    // folder inside one (moving a file out of the library would take it off the
    // shelf, and forgetting a folder is what RemoveFolder is for). The file keeps
    // its name; a name already taken there is refused.
    Result<void> MoveItem(std::string_view path, std::string_view targetFolder);

    // The items of one folder, or of every folder when `folder` is empty - in natural
    // name order ("img2" before "img10"). `folder` may be a library folder or any folder INSIDE one:
    // either way it includes everything beneath it.
    std::vector<LibraryItem> Items(std::string_view folder = {}) const;
    size_t Count() const;

    // The items whose NAME matches `text` (in one folder, or in every folder when `folder` is empty),
    // best first. Case-insensitive; every word of `text` has to appear in the name, in any order, so
    // "harvest 9" finds "9_16 harvest". Names that START with the first word rank above names that only
    // contain it. An empty `text` returns Items(folder).
    std::vector<LibraryItem> Search(std::string_view text, std::string_view folder = {}) const;

    // ---- THE FILE'S OWN FILL (see LibraryItem::fit) ----------------------------
    // One of "" (follow the output's own Fill), "contain", "cover", "fill";
    // anything else is an InvalidArgument, and a path the library does not list is
    // a NotFound. Persisted on its own small map (path → fit) beside the index,
    // because a rescan rebuilds every item from the disk and a choice the operator
    // made must not go with it. `Items()` hands the stored fit back on the item it
    // belongs to, and a rename/move carries it to the file's new path.
    Result<void> SetItemFit(std::string_view path, std::string_view fit);
    // The stored fit for `path`, or "" (it has none, or is not in the library).
    std::string ItemFit(std::string_view path) const;

private:
    // SPLIT IN TWO ON PURPOSE (the lock-scope rule this module was breaking):
    // BuildFoldersJsonLocked runs with mutex_ HELD and is pure serialization;
    // CommitFolders does the PAL write and is called with the lock RELEASED. They
    // used to be one helper (Save) called from inside the mutators' locked scope.
    std::string BuildFoldersJsonLocked() const;
    // Writes the folder list. PAL: NEVER call with mutex_ held. "" / no store
    // path is a no-op.
    Result<void> CommitFolders(const std::string& json) const;
    // Writes the remembered index of what is in each folder. Best effort: failing to keep it
    // only costs a slower next start. PAL: call with the lock RELEASED.
    void SaveIndex(const std::string& text) const;
    std::string BuildIndexLocked() const;   // the index as JSON; mutex_ must be held
    // The per-file fits as JSON. Pure serialization, safe under mutex_ — the
    // WRITE (SaveFits) happens after the lock is released, like SaveIndex.
    std::string BuildFitsJsonLocked() const;
    void SaveFits(const std::string& text) const;   // PAL: never call with mutex_ held
    // The comparison key of `path` (Absolute, then case/separator folding). PAL:
    // resolving the path is a filesystem call, so it runs with NO lock held.
    std::string CanonicalKey(std::string_view path) const;
    // The library's own spelling of the folder whose KEY matches (comparison
    // ignores case and separator style), else empty. Pure in-memory lookup, safe
    // under the lock — this is the half of the old Canonical() that may run there.
    std::string MatchKey(const std::string& key) const;
    // The item `key` (a Norm()ed path) names, plus the library folder it is filed
    // under and its slot in that folder's list. In-memory only, so mutex_ must be
    // HELD by the caller.
    bool FindItemLocked(const std::string& key, std::string& folderOut, size_t& indexOut) const;
    // Applies a re-key to the remembered index and returns the index JSON to
    // write: `replacement` (when given) takes the item's place, otherwise the item
    // is dropped. Mutex_ must be held; the caller saves the returned text AFTER
    // releasing it (SaveIndex is a PAL write).
    std::string ApplyItemChangeLocked(const std::string& oldKey, const LibraryItem* replacement);
    // The library folder a folder inside one belongs to ("" when the target is
    // neither). Mutex_ must be held: it only compares stored paths.
    std::string ContainingFolderLocked(const std::string& targetKey) const;
    // Re-files `item` in its own folder, keeping that folder's order, and returns
    // the index text to write — "" when the folder has left the library in the
    // meantime, in which case the file is still put back but nothing is indexed.
    // Mutex_ must be held; the caller saves the text AFTER releasing it.
    std::string InsertItemLocked(const LibraryItem& item);
    // The trash folder: a sibling of the library's own files (see TrashItem).
    std::string TrashDir() const;
    // Deletes trash entries older than kTrashRetentionMs — a file the operator
    // deleted has to leave the disk SOME day, and a restart can no longer restore
    // it anyway. PAL: never call with mutex_ held.
    void SweepTrash() const;

    // One undoable delete: where the file really lives now, and what it was (so a
    // restore can re-file it in its folder's order). Guarded by mutex_ like the
    // index — it is part of the same "what the library knows" state.
    struct TrashEntry {
        std::string token;
        LibraryItem item;
    };

    std::string storageFile_;
    std::string indexFile_;    // beside storageFile_
    std::string fitsFile_;     // the per-file fills, beside the index
    mutable std::mutex mutex_;
    std::vector<std::string> order_;                              // folders, as stored
    std::map<std::string, std::vector<LibraryItem>, std::less<>> items_;    // by folder path
    std::map<std::string, bool, std::less<>> scanned_;
    std::map<std::string, TrashEntry, std::less<>> trash_;   // this run's undone deletes, by token
    std::map<std::string, std::string, std::less<>> fits_;   // Norm(path) -> "contain"|"cover"|"fill" (absent = none)
};

} // namespace bps::media
