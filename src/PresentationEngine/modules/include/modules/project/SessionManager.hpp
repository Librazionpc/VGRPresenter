#pragma once

// SessionManager (docs/specs/15 §Session Manager): tracks the current engine
// session — id, user, active project, runtime statistics, temporary data — and
// supports restoring the previous session after a crash (the RecoveryManager
// uses the session record to detect unclean shutdowns).

#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace bps::project {

struct SessionInfo {
    std::string id;
    std::string user;
    std::string activeProjectId;
    int64_t startedAtMs = 0;
    int64_t lastActiveAtMs = 0;
    bool cleanShutdown = true;
    uint64_t presentationsOpened = 0;
    uint64_t savesPerformed = 0;
    uint64_t errorsEncountered = 0;
};

class SessionManager {
public:
    static SessionManager& Instance();

    Result<void> Begin(std::string_view user = {});
    Result<void> End();   // marks clean shutdown; persisted

    void SetActiveProject(std::string_view projectId);
    std::string ActiveProject() const;
    void SetUser(std::string_view user);
    std::string User() const;

    void NotePresentationOpened() { ++presentationsOpened_; }
    void NoteSave() { ++savesPerformed_; }
    void NoteError() { ++errorsEncountered_; }

    // By-value snapshot (the internal struct is written under mutex_).
    SessionInfo Info() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }
    bool WasCleanShutdown() const { return cleanShutdown_; }

    // --- Persistence (collection "session") ---
    Result<void> Save();
    Result<void> Load();
    Result<SessionInfo> Previous() const;   // the last persisted session

private:
    SessionManager() = default;

    mutable std::mutex mutex_;
    SessionInfo info_;
    std::atomic<bool> cleanShutdown_{true};
    std::atomic<uint64_t> presentationsOpened_{0};
    std::atomic<uint64_t> savesPerformed_{0};
    std::atomic<uint64_t> errorsEncountered_{0};
};

} // namespace bps::project
