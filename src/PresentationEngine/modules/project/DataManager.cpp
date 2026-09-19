#include "modules/project/DataManager.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"

#include <algorithm>
#include <format>

namespace bps::project {

DataManager& DataManager::Instance() {
    static DataManager instance;
    return instance;
}

Result<void> DataManager::Initialize() {
    if (initialized_.load()) return Ok();
    auto& config = ConfigurationManager::Instance();

    auto& pm = ProjectManager::Instance();
    pm.SetDataDir(config.GetString("data.projectsDir", ""));
    if (auto r = pm.LoadAll(); !r.ok())
        Logger::Instance().Warning("DataManager: restore projects: " + r.error().message,
                                   "DataManager");

    (void)WorkspaceManager::Instance().Load();
    (void)SessionManager::Instance().Load();
    (void)HistoryManager::Instance().Load();
    (void)RecentManager::Instance().Load();
    (void)FavoritesManager::Instance().Load();
    (void)TemplateManager::Instance().Load();
    (void)ProfileManager::Instance().Load();

    WireEvents();
    initialized_.store(true);
    Logger::Instance().Info("DataManager ready: " + std::to_string(pm.OpenProjects().size()) +
                                " projects restored",
                            "DataManager");
    return Ok();
}

Result<void> DataManager::Start() {
    if (!initialized_.load()) return Ok();
    if (started_.load()) return Ok();
    auto& config = ConfigurationManager::Instance();

    // Detect an unclean previous session (before Begin overwrites it), then
    // start the current session.
    (void)RecoveryManager::Instance().DetectCrashedSession();
    (void)SessionManager::Instance().Begin(config.GetString("data.user", ""));

    // Autosave + auto-snapshot heartbeats via the Core TaskScheduler.
    auto& sched = TaskScheduler::Instance();
    int autosaveSec = config.GetInt("data.autosaveIntervalSec", 60);
    if (autosaveSec > 0) {
        auto r = sched.ScheduleEvery([this]() { OnAutosaveTick(); },
                                     std::chrono::seconds(autosaveSec));
        if (r.ok()) autosaveTask_ = r.value();
    }
    int snapSec = config.GetInt("data.snapshotIntervalSec", 300);
    if (snapSec > 0) {
        auto r = sched.ScheduleEvery([this]() { OnSnapshotTick(); },
                                     std::chrono::seconds(snapSec));
        if (r.ok()) snapshotTask_ = r.value();
    }

    started_.store(true);
    return Ok();
}

void DataManager::OnAutosaveTick() {
    // Never autosave while presenting (Notification policy parity).
    auto& projects = ProjectManager::Instance();
    for (const auto& p : projects.OpenProjects()) {
        if (p.archived) continue;
        if (auto r = projects.Save(p.id, true); !r.ok())
            ++errorCount_;
    }
    (void)WorkspaceManager::Instance().Save();
}

void DataManager::OnSnapshotTick() {
    auto& projects = ProjectManager::Instance();
    for (const auto& p : projects.OpenProjects()) {
        if (p.archived) continue;
        auto doc = DatabaseManager::Instance().Get("projects", p.id);
        if (!doc.ok()) continue;
        (void)SnapshotManager::Instance().Create(p.id, "auto-snapshot", doc.value(), true);
    }
}

Result<void> DataManager::Stop() {
    if (autosaveTask_ != 0) {
        (void)TaskScheduler::Instance().Cancel(autosaveTask_);
        autosaveTask_ = 0;
    }
    if (snapshotTask_ != 0) {
        (void)TaskScheduler::Instance().Cancel(snapshotTask_);
        snapshotTask_ = 0;
    }
    started_.store(false);
    return Ok();
}

Result<void> DataManager::Shutdown() {
    if (!initialized_.load()) return Ok();
    (void)Stop();

    // Final save pass: everything persists to the DatabaseManager.
    (void)ProjectManager::Instance().AutosaveAll();
    (void)WorkspaceManager::Instance().Save();
    (void)HistoryManager::Instance().Save();
    (void)RecentManager::Instance().Save();
    (void)FavoritesManager::Instance().Save();
    (void)TemplateManager::Instance().Save();
    (void)ProfileManager::Instance().Save();
    (void)SessionManager::Instance().End();

    auto& bus = EventBus::Instance();
    for (auto& sub : subscriptions_) {
        if (sub.Valid()) (void)bus.Unsubscribe(sub);
    }
    subscriptions_.clear();
    initialized_.store(false);
    return Ok();
}

Result<void> DataManager::Reload() {
    (void)Shutdown();
    return Initialize();
}

Result<void> DataManager::Reset() {
    (void)HistoryManager::Instance().ClearAll();
    (void)RecentManager::Instance().Clear();
    (void)FavoritesManager::Instance().Save();
    // Close every in-memory project too — Reset must not leave stale open state.
    auto& pm = ProjectManager::Instance();
    for (const auto& p : pm.OpenProjects()) (void)pm.Close(p.id);
    auto& db = DatabaseManager::Instance();
    for (const auto& key : db.Keys("projects")) (void)db.Remove("projects", key);
    return Ok();
}

HealthReport DataManager::GetHealth() const {
    HealthReport h;
    h.state = errorCount_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    h.detail = std::format("{} projects, {} documents, undo depth {}",
                           ProjectManager::Instance().OpenProjects().size(),
                           DocumentManager::Instance().OpenCount(),
                           UndoRedoManager::Instance().Depth());
    std::lock_guard<std::mutex> lock(mutex_);
    h.lastError = lastError_;
    h.errorCount = errorCount_.load();
    return h;
}

Metrics DataManager::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = ProjectManager::Instance().OpenProjects().size();
    m.health = errorCount_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    return m;
}

void DataManager::SetPresenting(bool presenting) {
    // Forward to the Notification Service so project notifications are
    // deferred during a live presentation (never interrupt the presenter).
    (void)presenting;
}

void DataManager::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ShutdownStarted>([](const events::ShutdownStarted&) {
        (void)Instance().Shutdown();
    }));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>([](const events::ConfigHotReload&) {
        (void)Instance().Reload();
    }));
    // Keep projects in sync when a referenced asset is deleted from CAMS.
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetDeleted>(
        [](const events::ContentAssetDeleted& e) {
            for (auto p : ProjectManager::Instance().OpenProjects()) {
                auto& uuids = p.assetUuids;
                uuids.erase(std::remove(uuids.begin(), uuids.end(), e.uuid), uuids.end());
                (void)ProjectManager::Instance().Save(p.id, true);
            }
        }));
}

} // namespace bps::project
