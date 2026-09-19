#include "modules/project/SnapshotManager.hpp"

#include "core/database/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string GenerateId() {
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t v = static_cast<uint64_t>(t) ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 7);
    return std::format("snap-{:x}", v);
}
} // namespace

SnapshotManager& SnapshotManager::Instance() {
    static SnapshotManager instance;
    return instance;
}

Result<void> SnapshotManager::Persist(const Snapshot& s) {
    json::Value::Object o;
    o["id"] = json::Value::String(s.id);
    o["projectId"] = json::Value::String(s.projectId);
    o["label"] = json::Value::String(s.label);
    o["automatic"] = json::Value::Bool(s.automatic);
    o["createdAtMs"] = json::Value::Number(static_cast<double>(s.createdAtMs));
    o["data"] = s.data;
    return DatabaseManager::Instance().Put("snapshots", s.id, json::Value(std::move(o)));
}

Result<std::string> SnapshotManager::Create(std::string_view projectId, std::string_view label,
                                            const json::Value& data, bool automatic) {
    Snapshot s;
    s.id = GenerateId();
    s.projectId = std::string(projectId);
    s.label = std::string(label);
    s.automatic = automatic;
    s.createdAtMs = MsNow();
    s.data = data;
    if (auto r = Persist(s); !r.ok()) return r.error();

    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.push_back(s);
    // Enforce per-project retention (oldest dropped). The prune only considers
    // snapshots of this project — no cross-project comparator.
    auto isProject = [&](const Snapshot& x) { return x.projectId == projectId; };
    size_t count = static_cast<size_t>(
        std::count_if(snapshots_.begin(), snapshots_.end(), isProject));
    while (count > limitPerProject_) {
        auto first = std::find_if(snapshots_.begin(), snapshots_.end(), isProject);
        auto oldest = first;
        for (auto it = std::next(first); it != snapshots_.end(); ++it) {
            if (isProject(*it) && it->createdAtMs < oldest->createdAtMs) oldest = it;
        }
        (void)DatabaseManager::Instance().Remove("snapshots", oldest->id);
        snapshots_.erase(oldest);
        --count;
    }
    return Result<std::string>{s.id};
}

std::vector<Snapshot> SnapshotManager::List(std::string_view projectId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Snapshot> out;
    for (const auto& s : snapshots_)
        if (s.projectId == projectId) out.push_back(s);
    std::sort(out.begin(), out.end(),
              [](const Snapshot& a, const Snapshot& b) { return a.createdAtMs > b.createdAtMs; });
    return out;
}

Result<Snapshot> SnapshotManager::Get(std::string_view snapshotId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                           [&](const Snapshot& s) { return s.id == snapshotId; });
    if (it == snapshots_.end())
        return Error::Make(Err::Project_RecoveryNotFound, "Snapshot", "no such snapshot");
    return Result<Snapshot>{*it};
}

Result<void> SnapshotManager::Restore(std::string_view snapshotId) {
    auto s = Get(snapshotId);
    if (!s.ok()) return s.error();
    // Write the captured project state back into the database (the project
    // manager re-reads it on next Load/Open).
    return DatabaseManager::Instance().Put("projects", s.value().projectId, s.value().data);
}

Result<void> SnapshotManager::Remove(std::string_view snapshotId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                           [&](const Snapshot& s) { return s.id == snapshotId; });
    if (it == snapshots_.end())
        return Error::Make(Err::Project_RecoveryNotFound, "Snapshot", "no such snapshot");
    snapshots_.erase(it);
    return DatabaseManager::Instance().Remove("snapshots", std::string(snapshotId));
}

Result<void> SnapshotManager::ClearProject(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> toRemove;
    for (const auto& s : snapshots_)
        if (s.projectId == projectId) toRemove.push_back(s.id);
    snapshots_.erase(std::remove_if(snapshots_.begin(), snapshots_.end(),
                                    [&](const Snapshot& s) { return s.projectId == projectId; }),
                     snapshots_.end());
    for (const auto& id : toRemove)
        (void)DatabaseManager::Instance().Remove("snapshots", id);
    return Ok();
}

size_t SnapshotManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.size();
}

} // namespace bps::project
