#pragma once

// BackupManager (docs/specs/15 §Backup Manager): automatic and scheduled
// backups with retention policies. Full backups capture the project file;
// incremental backups record the delta since the last full backup. Backups are
// JSON documents in the DatabaseManager (collection "backups") so they survive
// restarts; cloud backups are a documented future extension.

#include "modules/project/Project.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct BackupRecord {
    std::string id;
    std::string projectId;
    std::string kind;          // "full" | "incremental"
    int64_t timestampMs = 0;
    std::string destination;   // host path or "db"
};

class BackupManager {
public:
    static BackupManager& Instance();

    // Run a backup now. `full` forces a full backup; otherwise an incremental
    // backup is taken when a full backup exists, else a full one.
    Result<std::string> Backup(std::string_view projectId, std::string_view destDir = {},
                               bool full = false);

    // Retention: keep at most `keepFull` full backups and `keepIncremental`
    // incremental backups per project; older ones are pruned.
    Result<void> ApplyRetention(std::string_view projectId, int keepFull = 5,
                                int keepIncremental = 20);

    // Scheduled backups: enable a TaskScheduler cadence (ms). Returns the task
    // id; cancel via DisableScheduled().
    Result<uint64_t> EnableScheduled(std::chrono::milliseconds every,
                                     std::string_view destDir = {});
    void DisableScheduled();

    std::vector<BackupRecord> List(std::string_view projectId) const;
    Result<BackupRecord> Latest(std::string_view projectId) const;
    Result<void> Restore(std::string_view backupId, std::string_view destPath);
    Result<void> ClearProject(std::string_view projectId);

    size_t Count() const;

private:
    BackupManager() = default;
    Result<void> Persist(const BackupRecord& b);

    mutable std::mutex mutex_;
    std::vector<BackupRecord> backups_;
    uint64_t taskId_ = 0;
    std::string destDir_;
};

} // namespace bps::project
