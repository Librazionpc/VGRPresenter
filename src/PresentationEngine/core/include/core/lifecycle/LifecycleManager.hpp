#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bps {

// Canonical 12-state lifecycle (docs/specs/12 §5; architecture brief):
// Discovered -> Created -> Registered -> Initialized -> Started -> Running ->
// Paused -> Suspended -> Resumed -> Stopping -> Stopped -> Destroyed.
enum class LifecycleState : int {
    Discovered = 0,
    Created,
    Registered,
    Initialized,
    Started,
    Running,
    Paused,
    Suspended,
    Resumed,
    Stopping,
    Stopped,
    Destroyed
};

inline const char* ToString(LifecycleState s) {
    switch (s) {
        case LifecycleState::Discovered:  return "Discovered";
        case LifecycleState::Created:     return "Created";
        case LifecycleState::Registered:  return "Registered";
        case LifecycleState::Initialized: return "Initialized";
        case LifecycleState::Started:     return "Started";
        case LifecycleState::Running:     return "Running";
        case LifecycleState::Paused:      return "Paused";
        case LifecycleState::Suspended:   return "Suspended";
        case LifecycleState::Resumed:     return "Resumed";
        case LifecycleState::Stopping:    return "Stopping";
        case LifecycleState::Stopped:     return "Stopped";
        case LifecycleState::Destroyed:   return "Destroyed";
    }
    return "Unknown";
}

enum class LifecyclePhase : int { Before = 0, On, After };

struct LifecycleHook {
    LifecyclePhase phase = LifecyclePhase::On;
    std::function<Result<void>()> fn;
};

// The lifecycle engine every subsystem follows (docs/specs/12).
class LifecycleManager final : public IService {
public:
    static LifecycleManager& Instance();

    struct EntityInfo {
        std::string id;
        Version version;
        std::vector<std::string> dependencies;
        LifecycleState state = LifecycleState::Registered;
    };

    Result<void> RegisterEntity(std::string_view id, const Version& version = kEngineVersion,
                                std::vector<std::string> dependencies = {});
    Result<void> UnregisterEntity(std::string_view id);   // removes entity + hooks
    Result<void> AddHook(std::string_view id, LifecycleState target, LifecycleHook hook);

    LifecycleState State(std::string_view id) const;
    Result<void> Transition(std::string_view id, LifecycleState target);
    Result<void> TransitionDependent(std::string_view id, LifecycleState target);
    Result<void> SetState(std::string_view id, LifecycleState state);   // direct set (Kernel boot)

    std::vector<EntityInfo> Snapshot() const;
    size_t EntityCount() const;

    const char* ServiceName() const noexcept override { return "LifecycleManager"; }
    HealthReport GetHealth() const override;

private:
    struct Entity {
        Version version;
        std::vector<std::string> dependencies;
        LifecycleState state = LifecycleState::Registered;   // entities enter via registration
        std::map<LifecycleState, std::vector<LifecycleHook>> hooks;  // by target state
    };

    bool IsLegalTransition(LifecycleState from, LifecycleState to) const;
    static LifecycleState NextLinear(LifecycleState s);   // next state on the canonical path
    int Order(LifecycleState s) const;
    Result<void> RunHooks(Entity& e, LifecycleState target, bool reverse);
    Result<void> StepEntity(Entity& e, LifecycleState target);   // caller holds mutex_
    Entity* FindLocked(std::string_view id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entity> entities_;
};

} // namespace bps
