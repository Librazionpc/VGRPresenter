#pragma once

// Resource contracts + feature registry (docs/specs/16 §8, §15, §18). Every
// module/feature declares what it needs (ModuleContract); the FeatureManager
// registers them and the runtime decides enable/disable/suspend/throttle.
// This is the "every module describes itself" requirement.

#include "core/common/Common.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bps::adaptive {

// What a module/feature declares about itself (docs/specs/16 §Resource
// Contracts). All bytes; 0 = unknown/not applicable.
struct ModuleContract {
    std::string name;
    uint64_t minRamBytes = 0;
    uint64_t recommendedRamBytes = 0;
    uint64_t maxRamBytes = 0;
    bool gpuOptional = true;
    bool gpuRequired = false;
    unsigned cpuThreads = 1;         // recommended worker count
    uint64_t diskBytes = 0;
    int estimatedStartupMs = 0;      // 0 = instant
    bool supportsLazyLoading = true;
    bool supportsSuspension = true;
    std::vector<std::string> dependencies;
};

enum class FeatureState : int { Disabled = 0, Enabled, Suspended, Throttled };

inline const char* ToString(FeatureState s) {
    switch (s) {
        case FeatureState::Disabled:  return "Disabled";
        case FeatureState::Enabled:   return "Enabled";
        case FeatureState::Suspended: return "Suspended";
        case FeatureState::Throttled: return "Throttled";
    }
    return "Unknown";
}

struct FeatureDef {
    std::string id;
    std::string name;
    ModuleContract contract;
    FeatureState state = FeatureState::Enabled;
    bool enabledByDefault = true;
};

class FeatureManager {
public:
    FeatureManager() = default;

    Result<void> Register(const FeatureDef& def);   // Err::AlreadyExists on dup id
    Result<void> Unregister(std::string_view id);

    Result<void> SetEnabled(std::string_view id, bool enabled);
    Result<void> SetSuspended(std::string_view id, bool suspended);
    Result<void> SetThrottled(std::string_view id, bool throttled);
    Result<FeatureState> State(std::string_view id) const;
    bool IsEnabled(std::string_view id) const;

    std::optional<ModuleContract> Contract(std::string_view id) const;
    std::vector<FeatureDef> All() const;
    size_t Count() const;

    // Auto-recommend state from a contract + the machine (used at startup).
    FeatureState RecommendState(const FeatureDef& def, uint64_t totalRamBytes,
                                bool gpuAvailable) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, FeatureDef, std::less<>> features_;
};

} // namespace bps::adaptive
