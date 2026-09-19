#pragma once

// ReferenceManager (docs/specs/15 §Reference Manager): analyzes a project's
// asset usage against the CAMS library — finds unused, broken, duplicate and
// shared assets — and supports cleanup operations. CAMS integration only
// (reads metadata; never touches files).

#include "modules/project/Project.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct ReferenceReport {
    std::vector<std::string> unused;      // library assets not referenced by the project
    std::vector<std::string> broken;      // project references with no library asset
    std::vector<std::string> duplicates;  // library assets with identical content hash
    std::vector<std::string> shared;      // assets referenced by more than one project
};

class ReferenceManager {
public:
    static ReferenceManager& Instance();

    // Analyze one project against the CAMS library.
    ReferenceReport Analyze(std::string_view projectId) const;

    // Analyze all open projects (shared = assets used by ≥2 projects).
    ReferenceReport AnalyzeAll() const;

    // Cleanup helpers (CAMS-backed).
    Result<void> RemoveUnused(std::string_view projectId,
                              const std::vector<std::string>& onlyUuids = {});
    Result<void> ClearDuplicates(std::string_view projectId);

    size_t AnalyzeCount() const { return analyzes_.load(); }

private:
    ReferenceManager() = default;

    mutable std::mutex mutex_;
    mutable std::atomic<uint64_t> analyzes_{0};
};

} // namespace bps::project
