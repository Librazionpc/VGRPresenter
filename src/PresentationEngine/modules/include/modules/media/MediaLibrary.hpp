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

private:
    Result<void> Save() const;
    // Writes the remembered index of what is in each folder. Best effort: failing to keep it
    // only costs a slower next start.
    void SaveIndex(const std::string& text) const;
    std::string BuildIndexLocked() const;   // the index as JSON; mutex_ must be held
    // The library's own spelling of `path` if it is a folder in the library (comparison
    // ignores case and separator style), else empty.
    std::string Canonical(std::string_view path) const;

    std::string storageFile_;
    std::string indexFile_;    // beside storageFile_
    mutable std::mutex mutex_;
    std::vector<std::string> order_;                              // folders, as stored
    std::map<std::string, std::vector<LibraryItem>, std::less<>> items_;    // by folder path
    std::map<std::string, bool, std::less<>> scanned_;
};

} // namespace bps::media
