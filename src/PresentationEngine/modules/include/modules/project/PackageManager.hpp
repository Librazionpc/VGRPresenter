#pragma once

// PackageManager (docs/specs/15 §Package Manager): builds and consumes
// portable project packages — a single .bpspkg ZIP file containing the project
// document, its referenced CAMS assets, and a dependency manifest. Building on
// the CAMS ZipWriter means the package pipeline inherits CRC-32 integrity and
// standard ZIP compatibility (no new file-format code).

#include "modules/project/Project.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct PackageManifest {
    std::string format = "bps-project-package";
    int version = 1;
    std::string projectId;
    std::string projectName;
    std::vector<std::string> assetUuids;
    std::string createdAt;
};

// One file entry inside a package.
struct PackageEntry {
    std::string archivePath;   // e.g. "assets/<uuid>.bin"
    std::string sourceUuid;    // CAMS uuid (empty for the project document)
    std::string sourcePath;    // VFS path inside the library mount
};

class PackageManager {
public:
    static PackageManager& Instance();

    // Build a package for a project into `hostDestPath` (.bpspkg).
    Result<void> Export(std::string_view projectId, std::string_view hostDestPath);

    // Import a package: extracts the project + assets into the given CAMS
    // mount, then opens the project. Returns the project id.
    Result<std::string> Import(std::string_view hostPkgPath, std::string_view targetMount = {});

    // Inspect a package without importing it.
    Result<PackageManifest> Inspect(std::string_view hostPkgPath);

    size_t PackagesImported() const { return importedCount_.load(); }

private:
    PackageManager() = default;

    mutable std::mutex mutex_;
    std::atomic<uint64_t> importedCount_{0};
};

} // namespace bps::project
