#pragma once

// Virtual File System (docs/specs/13 §Virtual File System). No module opens
// files directly — content is addressed through a mount, which can back onto
// the disk (PAL IFilesystem), a ZIP archive, or memory. Cloud mounts are a
// documented future extension.

#include "core/common/Common.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps::content {

// A mount is a named root. Paths inside a mount use forward slashes and no
// leading slash, e.g. "images/worship/logo.png".
class IVfs {
public:
    virtual ~IVfs() = default;

    virtual const char* Kind() const noexcept = 0;   // "disk" / "memory" / "zip"
    virtual const char* Name() const noexcept = 0;   // mount name (the "mount:/" prefix)

    virtual Result<std::vector<uint8_t>> Read(std::string_view path) const = 0;
    virtual Result<std::string> ReadText(std::string_view path) const = 0;
    virtual Result<void> Write(std::string_view path, const std::vector<uint8_t>& data) = 0;
    virtual Result<void> WriteText(std::string_view path, std::string_view text) = 0;
    virtual Result<void> Remove(std::string_view path) = 0;
    virtual Result<void> Move(std::string_view from, std::string_view to) = 0;
    virtual Result<bool> Exists(std::string_view path) const = 0;
    virtual Result<uint64_t> Size(std::string_view path) const = 0;
    virtual Result<std::vector<std::string>> List(std::string_view dir) const = 0;  // full paths
    virtual Result<void> CreateDirectories(std::string_view dir) = 0;
};

// --- DiskVfs: backed by the PAL filesystem (Phase 2) ------------------------
// Files live under a root directory on disk.
class DiskVfs final : public IVfs {
public:
    explicit DiskVfs(std::string name, std::string rootDir);

    const char* Kind() const noexcept override { return "disk"; }
    const char* Name() const noexcept override { return name_.c_str(); }

    Result<std::vector<uint8_t>> Read(std::string_view path) const override;
    Result<std::string> ReadText(std::string_view path) const override;
    Result<void> Write(std::string_view path, const std::vector<uint8_t>& data) override;
    Result<void> WriteText(std::string_view path, std::string_view text) override;
    Result<void> Remove(std::string_view path) override;
    Result<void> Move(std::string_view from, std::string_view to) override;
    Result<bool> Exists(std::string_view path) const override;
    Result<uint64_t> Size(std::string_view path) const override;
    Result<std::vector<std::string>> List(std::string_view dir) const override;
    Result<void> CreateDirectories(std::string_view dir) override;

    const std::string& Root() const noexcept { return root_; }
    std::string ToHostPath(std::string_view rel) const;

private:
    std::string name_;
    std::string root_;
};

// --- MemoryVfs: in-memory map (tests, transient content) ---------------------
class MemoryVfs final : public IVfs {
public:
    explicit MemoryVfs(std::string name);

    const char* Kind() const noexcept override { return "memory"; }
    const char* Name() const noexcept override { return name_.c_str(); }

    Result<std::vector<uint8_t>> Read(std::string_view path) const override;
    Result<std::string> ReadText(std::string_view path) const override;
    Result<void> Write(std::string_view path, const std::vector<uint8_t>& data) override;
    Result<void> WriteText(std::string_view path, std::string_view text) override;
    Result<void> Remove(std::string_view path) override;
    Result<void> Move(std::string_view from, std::string_view to) override;
    Result<bool> Exists(std::string_view path) const override;
    Result<uint64_t> Size(std::string_view path) const override;
    Result<std::vector<std::string>> List(std::string_view dir) const override;
    Result<void> CreateDirectories(std::string_view dir) override;

    size_t EntryCount() const;

private:
    std::string name_;
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<uint8_t>> entries_;   // key: normalized rel path
};

// --- ZipVfs: read-only view of a ZIP archive (stored entries) ----------------
// Reads standard .zip archives with compression method 0 (stored). Deflate
// entries return Content_UnsupportedFormat (zlib-backed support is future).
class ZipVfs final : public IVfs {
public:
    explicit ZipVfs(std::string name, std::string archivePath);

    const char* Kind() const noexcept override { return "zip"; }
    const char* Name() const noexcept override { return name_.c_str(); }

    Result<void> Open();   // parses central directory; idempotent

    Result<std::vector<uint8_t>> Read(std::string_view path) const override;
    Result<std::string> ReadText(std::string_view path) const override;
    Result<void> Write(std::string_view, const std::vector<uint8_t>&) override {
        return Error::Make(Err::Unsupported, "CAMS", "ZipVfs is read-only");
    }
    Result<void> WriteText(std::string_view, std::string_view) override {
        return Error::Make(Err::Unsupported, "CAMS", "ZipVfs is read-only");
    }
    Result<void> Remove(std::string_view) override {
        return Error::Make(Err::Unsupported, "CAMS", "ZipVfs is read-only");
    }
    Result<void> Move(std::string_view, std::string_view) override {
        return Error::Make(Err::Unsupported, "CAMS", "ZipVfs is read-only");
    }
    Result<bool> Exists(std::string_view path) const override;
    Result<uint64_t> Size(std::string_view path) const override;
    Result<std::vector<std::string>> List(std::string_view dir) const override;
    Result<void> CreateDirectories(std::string_view) override {
        return Error::Make(Err::Unsupported, "CAMS", "ZipVfs is read-only");
    }

    size_t EntryCount() const { return entries_.size(); }
    const std::vector<std::string>& EntryNames() const { return entryNames_; }

private:
    struct Entry {
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localOffset = 0;
        uint16_t method = 0;
    };

    Result<void> OpenLocked();   // requires mutex_

    mutable std::mutex mutex_;
    std::string name_;
    std::string archive_;
    bool opened_ = false;
    std::vector<uint8_t> bytes_;             // cached archive bytes (after Open)
    std::map<std::string, Entry> entries_;
    std::vector<std::string> entryNames_;
};

// --- ZipWriter: creates stored-entry .zip archives (PackageExporter) ---------
// Produces standard ZIP files (method 0) readable by any unzip tool.
class ZipWriter {
public:
    // Append a file entry with the given bytes. `name` is the archive-internal
    // path, e.g. "packages/logo.png".
    Result<void> AddEntry(std::string name, const std::vector<uint8_t>& data);

    // Serialize the whole archive.
    Result<std::vector<uint8_t>> Finalize() const;

    // Convenience: write the archive to a host file through the PAL.
    Result<void> WriteTo(std::string_view hostPath) const;

private:
    struct Pending {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t crc = 0;
    };
    std::vector<Pending> entries_;
};

// CRC-32 (IEEE), used by ZipWriter and AssetDatabase checksums.
uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

} // namespace bps::content
