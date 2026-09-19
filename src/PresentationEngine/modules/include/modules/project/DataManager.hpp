#pragma once

// DataManager (docs/specs/15): the Phase 4 facade. Owns the Project & Data
// System lifecycle and exposes the whole subsystem to the engine through one
// IService: projects, workspace, documents, history, undo/redo, sessions,
// snapshots, recovery, backups, packages, dependencies, references, recents,
// favorites, templates and profiles. Integrates the Core (Logger, Config,
// EventBus, TaskScheduler, DatabaseManager), CAMS (assets, packages, refs) and
// the Notification Service (via project.* events).

#include "modules/project/BackupManager.hpp"
#include "modules/project/DependencyManager.hpp"
#include "modules/project/DocumentManager.hpp"
#include "modules/project/FavoritesManager.hpp"
#include "modules/project/HistoryManager.hpp"
#include "modules/project/PackageManager.hpp"
#include "modules/project/ProfileManager.hpp"
#include "modules/project/ProjectManager.hpp"
#include "modules/project/RecentManager.hpp"
#include "modules/project/RecoveryManager.hpp"
#include "modules/project/ReferenceManager.hpp"
#include "modules/project/SessionManager.hpp"
#include "modules/project/SnapshotManager.hpp"
#include "modules/project/TemplateManager.hpp"
#include "modules/project/UndoRedoManager.hpp"
#include "modules/project/WorkspaceManager.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

class DataManager final : public IService {
public:
    static DataManager& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "DataManager"; }

    // --- Subsystem accessors (delegated) ---
    ProjectManager& Projects() { return ProjectManager::Instance(); }
    WorkspaceManager& Workspace() { return WorkspaceManager::Instance(); }
    DocumentManager& Documents() { return DocumentManager::Instance(); }
    HistoryManager& History() { return HistoryManager::Instance(); }
    UndoRedoManager& UndoRedo() { return UndoRedoManager::Instance(); }
    SessionManager& Sessions() { return SessionManager::Instance(); }
    SnapshotManager& Snapshots() { return SnapshotManager::Instance(); }
    RecoveryManager& Recovery() { return RecoveryManager::Instance(); }
    BackupManager& Backups() { return BackupManager::Instance(); }
    PackageManager& Packages() { return PackageManager::Instance(); }
    DependencyManager& Dependencies() { return DependencyManager::Instance(); }
    ReferenceManager& References() { return ReferenceManager::Instance(); }
    RecentManager& Recents() { return RecentManager::Instance(); }
    FavoritesManager& Favorites() { return FavoritesManager::Instance(); }
    TemplateManager& Templates() { return TemplateManager::Instance(); }
    ProfileManager& Profiles() { return ProfileManager::Instance(); }

    // --- Convenience ---
    size_t OpenProjectCount() const { return ProjectManager::Instance().OpenProjects().size(); }
    size_t OpenDocumentCount() const { return DocumentManager::Instance().OpenCount(); }
    uint64_t UndoDepth() const { return UndoRedoManager::Instance().Depth(); }

    // Test hook.
    void SetPresenting(bool presenting);

private:
    DataManager() = default;

    void WireEvents();
    void OnAutosaveTick();
    void OnSnapshotTick();

    std::atomic<bool> initialized_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> errorCount_{0};
    uint64_t autosaveTask_ = 0;
    uint64_t snapshotTask_ = 0;
    std::vector<Subscription> subscriptions_;
    mutable std::mutex mutex_;
    std::string lastError_;
};

} // namespace bps::project
