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
