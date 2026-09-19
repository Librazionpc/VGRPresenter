#pragma once

// Common (OS-agnostic) PAL backend for the filesystem subsystem. Built on
// std::filesystem per the PAL design rule — OS-specific behavior is layered
// over this in platform/windows and platform/macos where needed.

#include "../IFilesystem.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace bps::platform {

class FilesystemImpl final : public IFilesystem {
public:
    FilesystemImpl() = default;
    ~FilesystemImpl() override;

    bool Exists(std::string_view path) const override;
    bool IsDirectory(std::string_view path) const override;
    bool IsRegularFile(std::string_view path) const override;

    Result<std::string> ReadText(std::string_view path) const override;
    Result<std::vector<uint8_t>> ReadBinary(std::string_view path) const override;
    Result<void> Write(std::string_view path, std::string_view data) override;
    Result<void> WriteBinary(std::string_view path, const std::vector<uint8_t>& data) override;
    Result<void> Append(std::string_view path, std::string_view data) override;

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
    void PollLoop();

    struct WatchEntry {
        std::string path;
        std::function<void(std::string_view)> onChange;
        std::string fingerprint; // last mtime tick, used to detect change
    };

    mutable std::mutex mutex_;
    std::unordered_map<WatchToken, WatchEntry> watches_;
    WatchToken nextToken_ = 1;
    std::atomic<bool> stop_{false};
    std::thread pollThread_;
    bool pollThreadStarted_ = false;
};

} // namespace bps::platform
