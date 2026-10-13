#pragma once

// PAL filesystem subsystem (Phase 2). Every file operation in the engine goes
// through this interface — modules never touch std::filesystem directly.
// Implementations: platform/common/Filesystem.cpp (POSIX-capable common),
// overridden per OS where needed (platform/windows, platform/macos).

#include "core/common/Common.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bps::platform {

class IFilesystem {
public:
    virtual ~IFilesystem() = default;

    virtual bool Exists(std::string_view path) const = 0;
    virtual bool IsDirectory(std::string_view path) const = 0;
    virtual bool IsRegularFile(std::string_view path) const = 0;

    virtual Result<std::string> ReadText(std::string_view path) const = 0;
    virtual Result<std::vector<uint8_t>> ReadBinary(std::string_view path) const = 0;
    virtual Result<void> Write(std::string_view path, std::string_view data) = 0;
    virtual Result<void> WriteBinary(std::string_view path, const std::vector<uint8_t>& data) = 0;
    virtual Result<void> Append(std::string_view path, std::string_view data) = 0;

    // ---- Atomic replace: the ONE way the engine saves a file it owns -------
    // Writes the bytes to a SIBLING temp file, checks they all landed, then
    // renames the temp over `path`. A reader therefore sees either the previous
    // complete file or the complete new one — never a truncated or zero-filled
    // one. Writing the target IN PLACE is what left <dataDir>/kernel.json as
    // 5973 bytes of NULs after an interrupted flush, and the next boot failed on
    // the file it had just corrupted.
    //
    // Use it for every internal document the engine reads back: settings,
    // project files, backups, library indexes, caches. Do NOT use it for
    // user-facing exports (exporters, render output, packages), where the
    // destination is the user's own choice and a plain Write is the honest op.
    // Parent directories are created as needed.
    virtual Result<void> WriteAtomic(std::string_view path, std::string_view data) = 0;
    virtual Result<void> WriteBinaryAtomic(std::string_view path,
                                           const std::vector<uint8_t>& data) = 0;

    // The same guarantee for a file-to-file save: copy into a sibling temp,
    // prove it landed whole, then swap. Used where the new file's bytes come
    // from an existing one — a backup snapshot, promoting a crash-recovery file
    // into place — because a copy interrupted halfway leaves a file that LOOKS
    // like a complete backup or show and cannot be read.
    virtual Result<void> CopyAtomic(std::string_view from, std::string_view to) = 0;

    // Rename with a BOUNDED retry — the swap WriteAtomic performs. A rename
    // onto or within a live-synced folder (OneDrive, Dropbox) transiently fails
    // with EIO/EBUSY while the sync filter holds the file; the lock clears
    // within a fraction of a second, so a few bounded attempts rescue the save
    // (the batch import of 680 shows into a OneDrive library hit this on ~8% of
    // writes). Exposed so hand-rolled savers that need extra ordering — see
    // vgr::VgrFile, which parks the previous file as .bak first — share the one
    // retry policy instead of each carrying their own copy.
    virtual Result<void> RenameRetry(std::string_view from, std::string_view to) = 0;

    virtual Result<void> CreateDirectory(std::string_view path) = 0;
    virtual Result<void> CreateDirectories(std::string_view path) = 0;
    virtual Result<void> Remove(std::string_view path) = 0;      // file or empty dir
    virtual Result<void> RemoveAll(std::string_view path) = 0;   // recursive
    virtual Result<void> Move(std::string_view from, std::string_view to) = 0;
    virtual Result<void> Rename(std::string_view from, std::string_view to) = 0;  // alias of Move
    virtual Result<void> Copy(std::string_view from, std::string_view to) = 0;

    virtual Result<uint64_t> FileSize(std::string_view path) const = 0;
    virtual Result<std::string> Absolute(std::string_view path) const = 0;

    struct FileMetadata {
        uint64_t sizeBytes = 0;
        int64_t modifiedEpochNs = 0;   // wall-clock ns since epoch
        bool isDirectory = false;
        bool isRegularFile = false;
        uint32_t permissions = 0;      // POSIX mode bits (r/w/x for owner/group/other); 0 if unsupported
    };
    virtual Result<FileMetadata> Metadata(std::string_view path) const = 0;

    // Creates an empty file in the system temp dir; returns its full path.
    virtual Result<std::string> CreateTempFile(std::string_view prefix) = 0;

    // Recursive search: every file under `dir` whose Extension() == `extension`
    // ("" matches all files). Errors on a missing root.
    virtual Result<std::vector<std::string>> FindFiles(std::string_view dir,
                                                       std::string_view extension) const = 0;

    virtual Result<void> CreateSymlink(std::string_view target, std::string_view linkPath) = 0;
    virtual Result<std::string> ReadSymlink(std::string_view linkPath) const = 0;

    // Full paths of every entry (files + dirs) in `dir`; error if not a dir.
    virtual Result<std::vector<std::string>> Enumerate(std::string_view dir) const = 0;

    virtual std::string Join(std::string_view a, std::string_view b) const = 0;
    virtual std::string Extension(std::string_view path) const = 0;   // e.g. ".json"

    // Best-effort directory/file watcher: onChange fires with the changed path
    // (polling based; not guaranteed to catch every transient change). A token
    // of 0 means watching is unsupported.
    using WatchToken = uint64_t;
    virtual Result<WatchToken> Watch(std::string_view path,
                                     std::function<void(std::string_view)> onChange) = 0;
    virtual Result<void> CancelWatch(WatchToken token) = 0;
};

} // namespace bps::platform
