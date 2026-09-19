#include "modules/project/BackupManager.hpp"

#include "modules/project/ProjectManager.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "platform/PlatformAccessor.hpp"

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
    uint64_t v = static_cast<uint64_t>(t) ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 11);
    return std::format("bak-{:x}", v);
}
} // namespace

BackupManager& BackupManager::Instance() {
    static BackupManager instance;
    return instance;
}

Result<void> BackupManager::Persist(const BackupRecord& b) {
    json::Value::Object o;
    o["id"] = json::Value::String(b.id);
    o["projectId"] = json::Value::String(b.projectId);
    o["kind"] = json::Value::String(b.kind);
    o["timestampMs"] = json::Value::Number(static_cast<double>(b.timestampMs));
    o["destination"] = json::Value::String(b.destination);
    return DatabaseManager::Instance().Put("backups", b.id, json::Value(std::move(o)));
}

Result<std::string> BackupManager::Backup(std::string_view projectId, std::string_view destDir,
                                          bool full) {
    auto& db = DatabaseManager::Instance();
    auto doc = db.Get("projects", std::string(projectId));
    if (!doc.ok()) return doc.error();

    bool doFull = full;
    if (!doFull) {
        // Incremental when a full backup exists; the first backup is full.
        std::lock_guard<std::mutex> lock(mutex_);
        doFull = true;
        for (const auto& b : backups_) {
            if (b.projectId == projectId && b.kind == "full") {
                doFull = false;
                break;
            }
        }
    }

    BackupRecord rec;
    rec.id = GenerateId();
    rec.projectId = std::string(projectId);
    rec.kind = doFull ? "full" : "incremental";
    rec.timestampMs = MsNow();
    rec.destination = std::string(destDir);
    if (rec.destination.empty()) rec.destination = "db";

    if (rec.destination != "db") {
        auto& fs = platform::PlatformAccessor::Get().Filesystem();
        (void)fs.CreateDirectories(rec.destination);
        std::string path = rec.destination + "/" + rec.id + ".bak.json";
        if (auto r = fs.Write(path, doc.value().ToString()); !r.ok()) return r.error();
        rec.destination = path;
    } else {
        // Store the full project document under the backup id.
        if (auto r = db.Put("backups-data", rec.id, doc.value()); !r.ok()) return r.error();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        backups_.push_back(rec);
    }
    if (auto r = Persist(rec); !r.ok()) return r.error();
    (void)EventBus::Instance().Publish(events::BackupCompleted{rec.projectId, rec.destination});
    Logger::Instance().Info("Backup " + rec.id + " (" + rec.kind + ") for project " +
                                std::string(projectId),
                            "BackupManager");
    return Result<std::string>{rec.id};
}

Result<void> BackupManager::ApplyRetention(std::string_view projectId, int keepFull,
                                           int keepIncremental) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackupRecord> proj;
    for (const auto& b : backups_)
        if (b.projectId == projectId) proj.push_back(b);
    std::sort(proj.begin(), proj.end(),
              [](const BackupRecord& a, const BackupRecord& b) { return a.timestampMs > b.timestampMs; });
    int fullSeen = 0, incSeen = 0;
    std::vector<std::string> toRemove;
    for (const auto& b : proj) {
        if (b.kind == "full") {
            ++fullSeen;
            if (fullSeen > keepFull) toRemove.push_back(b.id);
        } else {
            ++incSeen;
            if (incSeen > keepIncremental) toRemove.push_back(b.id);
        }
    }
    for (const auto& id : toRemove) {
        backups_.erase(std::remove_if(backups_.begin(), backups_.end(),
                                      [&](const BackupRecord& b) { return b.id == id; }),
                       backups_.end());
        (void)DatabaseManager::Instance().Remove("backups", id);
        (void)DatabaseManager::Instance().Remove("backups-data", id);
    }
    return Ok();
}

Result<uint64_t> BackupManager::EnableScheduled(std::chrono::milliseconds every,
                                                std::string_view destDir) {
    if (taskId_ != 0) (void)TaskScheduler::Instance().Cancel(taskId_);
    std::string dir(destDir);
    auto& sched = TaskScheduler::Instance();
    auto r = sched.ScheduleEvery([this, dir]() {
        for (const auto& p : ProjectManager::Instance().OpenProjects())
            (void)Backup(p.id, dir.empty() ? "db" : dir, false);
    }, every);
    if (!r.ok()) return r.error();
    taskId_ = r.value();
    return Result<uint64_t>{taskId_};
}

void BackupManager::DisableScheduled() {
    if (taskId_ != 0) {
        (void)TaskScheduler::Instance().Cancel(taskId_);
        taskId_ = 0;
    }
}

std::vector<BackupRecord> BackupManager::List(std::string_view projectId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackupRecord> out;
    for (const auto& b : backups_)
        if (b.projectId == projectId) out.push_back(b);
    std::sort(out.begin(), out.end(),
              [](const BackupRecord& a, const BackupRecord& b) { return a.timestampMs > b.timestampMs; });
    return out;
}

Result<BackupRecord> BackupManager::Latest(std::string_view projectId) const {
    auto list = List(projectId);
    if (list.empty())
        return Error::Make(Err::Project_RecoveryNotFound, "Backup", "no backups for project");
    return Result<BackupRecord>{list.front()};
}

Result<void> BackupManager::Restore(std::string_view backupId, std::string_view destPath) {
    auto doc = DatabaseManager::Instance().Get("backups-data", std::string(backupId));
    if (!doc.ok()) {
        // Fall back to the persisted metadata record's destination file.
        auto rec = DatabaseManager::Instance().Get("backups", std::string(backupId));
        if (!rec.ok()) return rec.error();
        std::string path;
        if (const auto* n = rec.value().Find("destination")) path = std::string(n->asString());
        if (path.empty() || path == "db") return Error::Make(Err::Project_RecoveryNotFound,
                                                              "Backup", "no data for backup");
        auto text = platform::PlatformAccessor::Get().Filesystem().ReadText(path);
        if (!text.ok()) return text.error();
        auto parsed = json::Parse(text.value());
        if (!parsed.ok()) return parsed.error();
        return platform::PlatformAccessor::Get().Filesystem().Write(
            destPath, parsed.value().ToString());
    }
    return platform::PlatformAccessor::Get().Filesystem().Write(destPath, doc.value().ToString());
}

Result<void> BackupManager::ClearProject(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> toRemove;
    for (const auto& b : backups_)
        if (b.projectId == projectId) toRemove.push_back(b.id);
    backups_.erase(std::remove_if(backups_.begin(), backups_.end(),
                                  [&](const BackupRecord& b) { return b.projectId == projectId; }),
                   backups_.end());
    for (const auto& id : toRemove) {
        (void)DatabaseManager::Instance().Remove("backups", id);
        (void)DatabaseManager::Instance().Remove("backups-data", id);
    }
    return Ok();
}

size_t BackupManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backups_.size();
}

} // namespace bps::project
