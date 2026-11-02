#pragma once

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
    std::string fit;
};

struct LibraryFolder {
    std::string path;
    std::string name;       // the folder's own name ("Videos")
    size_t count = 0;       // items found by the last scan
    bool scanned = false;   // has Scan() finished for it in THIS run (its items may be remembered from the last)
};

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

    Result<size_t> Scan(std::string_view path);

    std::vector<LibrarySubfolder> Subfolders(std::string_view libraryFolder) const;

    // ---- the FILES themselves: rename, delete, move ----------------------------
    Result<void> RenameItem(std::string_view path, std::string_view newName);
    Result<void> RemoveItem(std::string_view path);
    Result<std::string> TrashItem(std::string_view path);
    Result<std::string> RestoreTrashedItem(std::string_view token);
    Result<void> MoveItem(std::string_view path, std::string_view targetFolder);

    std::vector<LibraryItem> Items(std::string_view folder = {}) const;
    size_t Count() const;

    std::vector<LibraryItem> Search(std::string_view text, std::string_view folder = {}) const;

    // ---- THE FILE'S OWN FILL (see LibraryItem::fit) ----------------------------
    Result<void> SetItemFit(std::string_view path, std::string_view fit);
    // The stored fit for `path`, or "" (it has none, or is not in the library).
    std::string ItemFit(std::string_view path) const;

private:
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
    std::string MatchKey(const std::string& key) const;
    bool FindItemLocked(const std::string& key, std::string& folderOut, size_t& indexOut) const;
    std::string ApplyItemChangeLocked(const std::string& oldKey, const LibraryItem* replacement);
    // The library folder a folder inside one belongs to ("" when the target is
    // neither). Mutex_ must be held: it only compares stored paths.
    std::string ContainingFolderLocked(const std::string& targetKey) const;
    std::string InsertItemLocked(const LibraryItem& item);
    // The trash folder: a sibling of the library's own files (see TrashItem).
    std::string TrashDir() const;
    void SweepTrash() const;

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