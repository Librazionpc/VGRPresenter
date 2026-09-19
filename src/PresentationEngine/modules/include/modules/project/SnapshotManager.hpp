#pragma once

// SnapshotManager (docs/specs/15 §Snapshot Manager): manual and automatic
// snapshots / version checkpoints of project state. A snapshot captures the
// project's persisted representation at a point in time and can be restored.
// Auto-snapshots are created by the DataManager on a scheduler cadence.

#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct Snapshot {
    std::string id;
    std::string projectId;
    std::string label;
    bool automatic = false;
    int64_t createdAtMs = 0;
    json::Value data;   // the serialized project/document state
};

class SnapshotManager {
public:
    static SnapshotManager& Instance();

    Result<std::string> Create(std::string_view projectId, std::string_view label,
                               const json::Value& data, bool automatic = false);
    std::vector<Snapshot> List(std::string_view projectId) const;
    Result<Snapshot> Get(std::string_view snapshotId) const;
    Result<void> Restore(std::string_view snapshotId);   // writes data back via DatabaseManager
    Result<void> Remove(std::string_view snapshotId);
    Result<void> ClearProject(std::string_view projectId);

    size_t Count() const;

private:
    SnapshotManager() = default;
    Result<void> Persist(const Snapshot& s);

    mutable std::mutex mutex_;
    std::vector<Snapshot> snapshots_;
    size_t limitPerProject_ = 50;
};

} // namespace bps::project
