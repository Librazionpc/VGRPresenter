#pragma once

#include "core/common/Common.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bps::platform {
// A file mapped read-only into the address space. Its pages are the file's (the operating system pages them in and out
// on its own), not private memory of the process: a big read-only text can be kept this way at close to no cost in
// resident memory. The bytes stay valid until this object is destroyed; the file cannot be replaced or deleted while it
// is mapped (on Windows), so whoever owns it also owns removing the file afterwards.
class IMappedFile {
public:
    virtual ~IMappedFile() = default;
    virtual const char* Data() const = 0;
    virtual size_t Size() const = 0;
};

class IFileWriteStream {
public:
    virtual ~IFileWriteStream() = default;
    virtual Result<size_t> Write(const uint8_t* bytes, size_t size) = 0;
    virtual Result<void> Seek(uint64_t offset) = 0;
    virtual Result<void> Flush() = 0;
    virtual Result<void> Close() = 0;
};

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

    // Opens a seekable output stream for formats that finalize their headers
    // after streaming data. Unsupported by legacy/test filesystems unless
    // explicitly implemented.
    virtual Result<std::unique_ptr<IFileWriteStream>> OpenWriteStream(std::string_view) {
        return Error::Make(Err::Unsupported, "Filesystem", "seekable output streams are unavailable");
    }

    // Maps a whole file read-only (see IMappedFile). Unsupported on a filesystem that cannot (legacy/test ones).
    virtual Result<std::shared_ptr<const IMappedFile>> MapReadOnly(std::string_view) const {
        return Error::Make(Err::Unsupported, "Filesystem", "memory-mapped files are unavailable");
    }

    // ---- Atomic replace: the ONE way the engine saves a file it owns -------
    virtual Result<void> WriteAtomic(std::string_view path, std::string_view data) = 0;
    virtual Result<void> WriteBinaryAtomic(std::string_view path,
                                           const std::vector<uint8_t>& data) = 0;

    virtual Result<void> CopyAtomic(std::string_view from, std::string_view to) = 0;

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

    using WatchToken = uint64_t;
    virtual Result<WatchToken> Watch(std::string_view path,
                                     std::function<void(std::string_view)> onChange) = 0;
    virtual Result<void> CancelWatch(WatchToken token) = 0;
};
} // namespace bps::platform
