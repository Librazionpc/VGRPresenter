#pragma once

// ResourceBudgetManager (docs/specs/16 §5): every subsystem receives a budget.
// On a 4 GB laptop the renderer gets 512 MB, AI is disabled, video 256 MB,
// cache 128 MB — automatically. Budgets derive from total RAM + quality
// settings + pressure level, and are republished on change.

#include "core/common/Common.hpp"
#include "modules/adaptive/Quality.hpp"

#include <map>
#include <mutex>
#include <string>

namespace bps::adaptive {

// Known budget consumers (docs/specs/16 §5).
namespace Subsystem {
inline constexpr const char* kRenderer = "renderer";
inline constexpr const char* kAi = "ai";
inline constexpr const char* kCache = "cache";
inline constexpr const char* kSearch = "search";
inline constexpr const char* kVideo = "video";
inline constexpr const char* kThumbnails = "thumbnails";
} // namespace Subsystem

struct Budget {
    std::string subsystem;
    uint64_t maxBytes = 0;
    unsigned threadLimit = 0;    // 0 = no limit
    bool enabled = true;
};

class ResourceBudgetManager {
public:
    ResourceBudgetManager() = default;

    // Recompute all budgets from the machine + quality settings.
    void Recompute(uint64_t totalRamBytes, const QualitySettings& q,
                   PressureLevel pressure);

    Budget Get(std::string_view subsystem) const;
    std::vector<Budget> All() const;

    // Total budgeted bytes (for the dashboard).
    uint64_t Total() const;
    size_t Count() const { return budgets_.size(); }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Budget, std::less<>> budgets_;
};

} // namespace bps::adaptive
