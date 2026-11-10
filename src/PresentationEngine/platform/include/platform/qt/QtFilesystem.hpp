#pragma once

#include "../IFilesystem.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace bps::platform {

// ---------------------------------------------------------------------------
// QtFilesystem — the Qt-backed IFilesystem (the platform cage's first Qt
// service; see docs/platform/qt_first_platform.md).
//
// WHY QT HERE: a filesystem path is the one piece of "OS work" every feature
// touches, and it is where the three native backends kept disagreeing. Qt owns
// the mapping from a UTF-8 std::string to a name the OS can actually store
// (QFile/QDir go through the wide APIs on Windows, the byte APIs elsewhere), so
// this class is ONE implementation compiled on Windows, Linux and macOS instead
// of one per OS.
//
// WHAT IS QT and what is std C++:
//   * Qt: every file/dir/stat/space operation (QFile, QSaveFile, QFileInfo,
//     QDir, QDirIterator, QTemporaryFile). QSaveFile is the atomic replace: it
//     writes a temporary beside the target, flushes it, and commits with a
//     rename that leaves the target untouched on ANY failure.
//   * std::filesystem: the three operations Qt has no portable primitive for —
//     create_symlink, read_symlink and absolute() — plus Extension()/
//     Copy(recursive), which mirror the previous backend's exact semantics.
//     Those are ISO C++ (no OS header, no #ifdef), so the "one implementation"
//     rule still holds.
//
// CONTRACT: identical to the std::filesystem backend this replaced. It is a drop-in
// behind IFilesystem (the engine only ever sees the interface), selected at
// unconditionally (see platform/PlatformServices.hpp).
class QtFilesystem final : public IFilesystem {
public:
    QtFilesystem() = default;
    ~QtFilesystem() override;

    QtFilesystem(const QtFilesystem&) = delete;
    QtFilesystem& operator=(const QtFilesystem&) = delete;

    bool Exists(std::string_view path) const override;
    bool IsDirectory(std::string_view path) const override;
    bool IsRegularFile(std::string_view path) const override;

    Result<std::string> ReadText(std::string_view path) const override;
    Result<std::vector<uint8_t>> ReadBinary(std::string_view path) const override;
    Result<void> Write(std::string_view path, std::string_view data) override;
    Result<void> WriteBinary(std::string_view path, const std::vector<uint8_t>& data) override;
    Result<void> Append(std::string_view path, std::string_view data) override;
    Result<std::unique_ptr<IFileWriteStream>> OpenWriteStream(std::string_view path) override;

    Result<void> WriteAtomic(std::string_view path, std::string_view data) override;
    Result<void> WriteBinaryAtomic(std::string_view path,
                                   const std::vector<uint8_t>& data) override;

    Result<void> CopyAtomic(std::string_view from, std::string_view to) override;
    Result<void> RenameRetry(std::string_view from, std::string_view to) override;

    Result<void> CreateDirectory(std::string_view path) override;
    Result<void> CreateDirectories(std::string_view path) override;
    Result<void> Remove(std::string_view path) override;
    Result<void> RemoveAll(std::string_view path) override;
    Result<void> Move(std::string_view from, std::string_view to) override;
    Result<void> Rename(std::string_view from, std::string_view to) override;
    Result<void> Copy(std::string_view from, std::string_view to) override;

    Result<uint64_t> FileSize(std::string_view path) const override;
    Result<std::string> Absolute(std::string_view path) const override;
    Result<FileMetadata> Metadata(std::string_view path) const override;
    Result<std::string> CreateTempFile(std::string_view prefix) override;
    Result<std::vector<std::string>> FindFiles(std::string_view dir,
                                               std::string_view extension) const override;
    Result<void> CreateSymlink(std::string_view target, std::string_view linkPath) override;
    Result<std::string> ReadSymlink(std::string_view linkPath) const override;
    Result<std::vector<std::string>> Enumerate(std::string_view dir) const override;

    std::string Join(std::string_view a, std::string_view b) const override;
    std::string Extension(std::string_view path) const override;

    Result<WatchToken> Watch(std::string_view path,
                             std::function<void(std::string_view)> onChange) override;
    Result<void> CancelWatch(WatchToken token) override;

private:
    // One writer for Write/WriteBinary/Append: same open-mode rules, same
    // flush-and-check-the-close discipline the C stdio backend had.
    Result<void> WriteThrough(std::string_view path, const void* bytes, size_t size,
                              bool append, std::string_view op);

    // QSaveFile: temporary beside the target, flush, atomic commit. Parent
    // directories are created first (the engine saves into fresh project
    // folders), and a short write cancels the commit so the target is intact.
    Result<void> ReplaceAtomic(std::string_view path, const void* bytes, size_t size);

    void PollLoop();

    struct WatchEntry {
        std::string path;
        std::function<void(std::string_view)> onChange;
        int64_t stampMs = 0;   // last write time, the change fingerprint
    };

    mutable std::mutex mutex_;
    std::unordered_map<WatchToken, WatchEntry> watches_;
    WatchToken nextToken_ = 1;
    std::atomic<bool> stop_{false};
    std::thread pollThread_;
    bool pollThreadStarted_ = false;
};

} // namespace bps::platform
