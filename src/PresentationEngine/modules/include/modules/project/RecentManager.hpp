#pragma once

// RecentManager (docs/specs/15 §Recent Manager): recently opened projects,
// assets, templates and searches — persisted so the UI can offer "recent"
// lists on startup.

#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct RecentItem {
    std::string kind;     // "project" | "asset" | "template" | "search"
    std::string id;       // uuid / query text
    std::string name;
    int64_t timestampMs = 0;
};

class RecentManager {
public:
    static RecentManager& Instance();

    Result<void> Record(std::string_view kind, std::string_view id, std::string_view name);
    std::vector<RecentItem> List(std::string_view kind, int limit = 10) const;
    Result<void> Remove(std::string_view kind, std::string_view id);
    Result<void> Clear();

    Result<void> Save();
    Result<void> Load();
    size_t Count() const;

private:
    RecentManager() = default;

    mutable std::mutex mutex_;
    std::vector<RecentItem> items_;
    size_t limit_ = 50;
};

} // namespace bps::project
