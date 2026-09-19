#pragma once

// RecoveryManager (docs/specs/15 §Recovery Manager): automatically restores
// unsaved work, crashed sessions and incomplete imports. On engine start it
// detects an unclean previous session, records unsaved document state, and
// publishes project.recovery_available so the UI can offer recovery actions.

#include "modules/project/SessionManager.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct RecoveryItem {
    std::string kind;        // "unsaved-work" | "crashed-session" | "incomplete-import"
    std::string projectId;
    std::string documentId;
    std::string detail;
    int64_t timestampMs = 0;
};

class RecoveryManager {
public:
    static RecoveryManager& Instance();

    // --- Recording ---
    Result<void> RecordUnsavedWork(std::string_view projectId, std::string_view documentId,
                                   std::string_view detail);
    Result<void> ClearProject(std::string_view projectId);
    Result<void> Resolve(std::string_view itemId);

    // --- Detection (called on startup after SessionManager::Load) ---
    // Returns true when the previous session ended uncleanly.
    bool DetectCrashedSession();
    Result<std::string> CurrentSessionId() const;

    // --- Suggestions ---
    std::vector<RecoveryItem> Suggestions() const;
    Result<RecoveryItem> Suggest(std::string_view projectId) const;
    size_t Count() const;

private:
    RecoveryManager() = default;
    Result<void> Persist();

    mutable std::mutex mutex_;
    std::vector<RecoveryItem> items_;
};

} // namespace bps::project
