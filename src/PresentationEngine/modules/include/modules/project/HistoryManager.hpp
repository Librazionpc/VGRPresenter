#pragma once

// HistoryManager (docs/specs/15 §History Manager): maintains edit history,
// project save history and recovery checkpoints. The UndoRedoManager owns the
// live command stacks; this manager keeps the *persistent* record (what was
// changed, when, by which command) so recovery and audit work.

#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

class HistoryManager {
public:
    static HistoryManager& Instance();

    struct Entry {
        std::string projectId;
        std::string command;      // e.g. "text.edit", "slide.move"
        std::string detail;       // human-readable summary
        int64_t timestampMs = 0;
    };

    // --- Edit history ---
    Result<void> Record(std::string_view projectId, std::string_view command,
                        std::string_view detail);
    std::vector<Entry> EditHistory(std::string_view projectId, int limit = 100) const;
    Result<void> ClearEditHistory(std::string_view projectId);

    // --- Save history ---
    void RecordSave(std::string_view projectId, std::string_view path, bool autosave);
    std::vector<Entry> SaveHistory(std::string_view projectId, int limit = 50) const;

    // --- Recovery checkpoints (also used by the RecoveryManager) ---
    Result<void> Checkpoint(std::string_view projectId, std::string_view detail);
    std::vector<Entry> Checkpoints(std::string_view projectId) const;
    Result<void> ClearCheckpoints(std::string_view projectId);

    // --- Persistence (collection "history") ---
    Result<void> Save();
    Result<void> Load();
    Result<void> ClearAll();

    size_t TotalEntries() const;

private:
    HistoryManager() = default;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;          // chronological
    std::vector<Entry> saves_;            // chronological
    std::vector<Entry> checkpoints_;      // chronological
    size_t limit_ = 2000;
};

} // namespace bps::project
