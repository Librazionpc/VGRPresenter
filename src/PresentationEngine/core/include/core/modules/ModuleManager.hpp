#pragma once

#include "core/common/Common.hpp"
#include "core/lifecycle/LifecycleManager.hpp"
#include "interfaces/IModule.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace bps {

// Module states (docs/specs/08 §5). A projection of the canonical LifecycleManager
// states; Disabled and Failed are ModuleManager policy states.
enum class ModuleState : int {
    Installed = 0,
    Loaded,
    Initialized,
    Running,
    Paused,
    Suspended,
    Stopped,
    Unloaded,
    Disabled,
    Failed
};

inline const char* ToString(ModuleState s) {
    switch (s) {
        case ModuleState::Installed:   return "Installed";
        case ModuleState::Loaded:      return "Loaded";
        case ModuleState::Initialized: return "Initialized";
        case ModuleState::Running:     return "Running";
        case ModuleState::Paused:      return "Paused";
        case ModuleState::Suspended:   return "Suspended";
        case ModuleState::Stopped:     return "Stopped";
        case ModuleState::Unloaded:    return "Unloaded";
        case ModuleState::Disabled:    return "Disabled";
        case ModuleState::Failed:      return "Failed";
    }
    return "Unknown";
}

class ModuleManager final : public IService {
public:
    static ModuleManager& Instance();

    struct ModuleInfo {
        std::string id;
        Version version;
        ModuleState state = ModuleState::Installed;
    };

    Result<void> RegisterModule(std::shared_ptr<IModule> module);
    Result<void> UnregisterModule(std::string_view id);

    // --- Discovery (08 §Discover) ---
    // Registers a factory that creates the module implementation for `id`.
    Result<void> RegisterFactory(std::string_view id,
                                 std::function<std::shared_ptr<IModule>()> factory);
    // Scans <searchPath> for module sidecars (<id>.json) and registers each one
    // whose factory is known. Unknown ids are logged and skipped.
    using IService::Start;
    using IService::Stop;
    using IService::Reload;

    Result<void> Discover(std::string_view searchPath);
    size_t DiscoveredCount() const;

    Result<void> Load(std::string_view id);
    Result<void> Start(std::string_view id);
    Result<void> Stop(std::string_view id);
    Result<void> Pause(std::string_view id);
    Result<void> Resume(std::string_view id);
    Result<void> Suspend(std::string_view id);
    Result<void> Reload(std::string_view id);      // stop + start (00 §5)
    Result<void> Disable(std::string_view id);

    ModuleState State(std::string_view id) const;
    std::vector<ModuleInfo> Snapshot() const;
    std::shared_ptr<IModule> GetModule(std::string_view id) const;

    const char* ServiceName() const noexcept override { return "ModuleManager"; }
    HealthReport GetHealth() const override;

private:
    ModuleManager() = default;

    static LifecycleState ToLifecycle(ModuleState s);
    Result<void> DoTransition(std::string_view id, ModuleState target);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IModule>> modules_;
    std::unordered_map<std::string, std::function<std::shared_ptr<IModule>()>> factories_;
    std::vector<std::string> discovered_;
    std::string lastError_;                     // guarded by mutex_   // guarded by mutex_
    std::unordered_map<std::string, ModuleState> states_;                  // guarded by mutex_
};

} // namespace bps
