#include "core/lifecycle/LifecycleManager.hpp"

#include "core/logging/Logger.hpp"

namespace bps {

LifecycleManager& LifecycleManager::Instance() {
    static LifecycleManager instance;
    return instance;
}

int LifecycleManager::Order(LifecycleState s) const { return static_cast<int>(s); }

LifecycleState LifecycleManager::NextLinear(LifecycleState s) {
    switch (s) {
        case LifecycleState::Discovered:  return LifecycleState::Created;
        case LifecycleState::Created:     return LifecycleState::Registered;
        case LifecycleState::Registered:  return LifecycleState::Initialized;
        case LifecycleState::Initialized: return LifecycleState::Started;
        case LifecycleState::Started:     return LifecycleState::Running;
        case LifecycleState::Running:     return LifecycleState::Stopping;
        case LifecycleState::Paused:      return LifecycleState::Suspended;
        case LifecycleState::Suspended:   return LifecycleState::Resumed;
        case LifecycleState::Resumed:     return LifecycleState::Running;
        case LifecycleState::Stopping:    return LifecycleState::Stopped;
        case LifecycleState::Stopped:     return LifecycleState::Destroyed;
        case LifecycleState::Destroyed:   return LifecycleState::Discovered;
    }
    return LifecycleState::Destroyed;
}

bool LifecycleManager::IsLegalTransition(LifecycleState from, LifecycleState to) const {
    if (from == to) return true;  // no-op
    auto can = [](LifecycleState f, LifecycleState t) {
        switch (f) {
            case LifecycleState::Discovered:  return t == LifecycleState::Created;
            case LifecycleState::Created:     return t == LifecycleState::Registered;
            case LifecycleState::Registered:  return t == LifecycleState::Initialized;
            case LifecycleState::Initialized: return t == LifecycleState::Started;
            case LifecycleState::Started:     return t == LifecycleState::Running;
            case LifecycleState::Running:
                return t == LifecycleState::Paused || t == LifecycleState::Suspended ||
                       t == LifecycleState::Stopping;
            case LifecycleState::Paused:
                return t == LifecycleState::Running || t == LifecycleState::Suspended ||
                       t == LifecycleState::Stopping;
            case LifecycleState::Suspended:
                return t == LifecycleState::Running || t == LifecycleState::Paused ||
                       t == LifecycleState::Stopping;
            case LifecycleState::Resumed:     return t == LifecycleState::Running;
            case LifecycleState::Stopping:    return t == LifecycleState::Stopped;
            case LifecycleState::Stopped:
                return t == LifecycleState::Destroyed || t == LifecycleState::Registered;  // restart
            case LifecycleState::Destroyed:   return t == LifecycleState::Discovered;      // reinstall
        }
        return false;
    };
    return can(from, to);
}

LifecycleManager::Entity* LifecycleManager::FindLocked(std::string_view id) {
    auto it = entities_.find(std::string(id));
    return it == entities_.end() ? nullptr : &it->second;
}

Result<void> LifecycleManager::RegisterEntity(std::string_view id, const Version& version,
                                              std::vector<std::string> dependencies) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entities_.count(std::string(id)) > 0)
        return Error::Make(Err::AlreadyExists, "LifecycleManager",
                           "entity already registered: " + std::string(id));
    Entity e;
    e.version = version;
    e.dependencies = std::move(dependencies);
    entities_[std::string(id)] = std::move(e);
    return Ok();
}

Result<void> LifecycleManager::UnregisterEntity(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entities_.erase(std::string(id)) == 0)
        return Error::Make(Err::NotFound, "LifecycleManager",
                           "entity not registered: " + std::string(id));
    return Ok();
}

Result<void> LifecycleManager::AddHook(std::string_view id, LifecycleState target,
                                       LifecycleHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entity* e = FindLocked(id);
    if (!e)
        return Error::Make(Err::NotFound, "LifecycleManager", "entity not found: " + std::string(id));
    e->hooks[target].push_back(std::move(hook));
    return Ok();
}

LifecycleState LifecycleManager::State(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities_.find(std::string(id));
    return it == entities_.end() ? LifecycleState::Destroyed : it->second.state;
}

Result<void> LifecycleManager::RunHooks(Entity& e, LifecycleState target, bool reverse) {
    auto& hooks = e.hooks[target];
    auto run = [&](LifecyclePhase phase) -> Result<void> {
        for (auto& h : hooks)
            if (h.phase == phase) {
                auto r = h.fn();
                if (!r.ok()) return r;
            }
        return Ok();
    };
    if (reverse) {
        auto r = run(LifecyclePhase::After);
        if (!r.ok()) return r;
        r = run(LifecyclePhase::On);
        if (!r.ok()) return r;
        return run(LifecyclePhase::Before);
    }
    auto r = run(LifecyclePhase::Before);
    if (!r.ok()) return r;
    r = run(LifecyclePhase::On);
    if (!r.ok()) return r;
    return run(LifecyclePhase::After);
}

// Apply a single legal edge: run hooks (with rollback on failure), then commit state.
// Caller holds mutex_.
Result<void> LifecycleManager::StepEntity(Entity& e, LifecycleState target) {
    auto r = RunHooks(e, target, false);
    if (!r.ok()) {
        (void)RunHooks(e, target, true);   // rollback: reverse hooks, stay in prior state (12 §10)
        return r;
    }
    e.state = target;
    return Ok();
}

Result<void> LifecycleManager::Transition(std::string_view id, LifecycleState target) {
    std::unique_lock<std::mutex> lock(mutex_);
    Entity* e = FindLocked(id);
    if (!e)
        return Error::Make(Err::NotFound, "LifecycleManager", "entity not found: " + std::string(id));

    LifecycleState from = e->state;
    if (from == target) return Ok();

    // Dependency gating against the final target (12 §3).
    int tOrder = Order(target);
    for (const auto& dep : e->dependencies) {
        auto it = entities_.find(dep);
        if (it == entities_.end()) {
            std::string msg = "dependency '" + dep + "' of '" + std::string(id) + "' not registered";
            lock.unlock();
            return Error::Make(Err::Lifecycle_DependencyBlocked, "LifecycleManager", msg);
        }
        int required = std::min(tOrder, Order(LifecycleState::Running));
        if (Order(it->second.state) < required) {
            std::string msg = "dependency '" + dep + "' is " +
                              std::string(ToString(it->second.state)) + ", need >= " +
                              std::string(ToString(static_cast<LifecycleState>(required))) +
                              " to move '" + std::string(id) + "' to " +
                              std::string(ToString(target));
            lock.unlock();
            return Error::Make(Err::Lifecycle_DependencyBlocked, "LifecycleManager", msg);
        }
    }

    // Direct legal edge?
    if (IsLegalTransition(from, target)) {
        auto r = StepEntity(*e, target);
        lock.unlock();
        if (!r.ok()) {
            Logger::Instance().Error("LifecycleManager: transition '" + std::string(id) + "' to " +
                                     std::string(ToString(target)) + " failed: " + r.error().message);
            return Error::Make(Err::Lifecycle_HookFailed, "LifecycleManager",
                               "hook failed during transition", &r.error());
        }
        return Ok();
    }

    // No direct edge: walk along the canonical path (12 §5). The linear path never
    // passes through the side-branch states Paused/Suspended/Resumed, so
    // Running -> Stopped goes Running -> Stopping -> Stopped.
    //
    // Reset edges jump backward in rank and are only honored for their canonical
    // purpose: Stopped -> Running restarts (via the Stopped -> Registered edge),
    // Destroyed -> <target> reinstalls (via Destroyed -> Discovered).
    LifecycleState cur = from;
    if (Order(target) <= Order(from)) {
        bool resetAllowed =
            (from == LifecycleState::Stopped && target == LifecycleState::Running) ||
            (from == LifecycleState::Destroyed);
        if (!resetAllowed) {
            std::string msg = "illegal transition " + std::string(ToString(from)) + " -> " +
                              std::string(ToString(target)) + " for " + std::string(id);
            lock.unlock();
            return Error::Make(Err::Lifecycle_InvalidTransition, "LifecycleManager", msg);
        }
        LifecycleState resetTo = from == LifecycleState::Stopped
                                     ? LifecycleState::Registered
                                     : LifecycleState::Discovered;
        auto reset = StepEntity(*e, resetTo);
        if (!reset.ok()) {
            std::string msg = "hook failed during reset of '" + std::string(id) + "' to " +
                              std::string(ToString(resetTo));
            lock.unlock();
            return Error::Make(Err::Lifecycle_HookFailed, "LifecycleManager", msg, &reset.error());
        }
        cur = resetTo;
    }
    while (cur != target) {
        LifecycleState next = NextLinear(cur);
        if (IsLegalTransition(cur, target)) next = target;
        if (!IsLegalTransition(cur, next)) {
            std::string msg = "no path from " + std::string(ToString(from)) + " to " +
                              std::string(ToString(target)) + " for " + std::string(id);
            lock.unlock();
            return Error::Make(Err::Lifecycle_InvalidTransition, "LifecycleManager", msg);
        }
        auto r = StepEntity(*e, next);
        if (!r.ok()) {
            std::string msg = "hook failed during transition '" + std::string(id) + "' to " +
                              std::string(ToString(target)) + " at " + std::string(ToString(next));
            lock.unlock();
            Logger::Instance().Error("LifecycleManager: " + msg + ": " + r.error().message);
            return Error::Make(Err::Lifecycle_HookFailed, "LifecycleManager", msg, &r.error());
        }
        cur = next;
    }
    return Ok();
}

Result<void> LifecycleManager::TransitionDependent(std::string_view id, LifecycleState target) {
    std::vector<std::string> deps;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entity* e = FindLocked(id);
        if (!e)
            return Error::Make(Err::NotFound, "LifecycleManager", "entity not found: " + std::string(id));
        deps = e->dependencies;
    }
    int required = std::min(Order(target), Order(LifecycleState::Running));
    for (const auto& dep : deps) {
        if (Order(State(dep)) < required) {
            auto r = TransitionDependent(dep, static_cast<LifecycleState>(required));
            if (!r.ok()) return r;
        }
    }
    return Transition(id, target);
}

Result<void> LifecycleManager::SetState(std::string_view id, LifecycleState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entity* e = FindLocked(id);
    if (!e)
        return Error::Make(Err::NotFound, "LifecycleManager", "entity not found: " + std::string(id));
    e->state = state;
    return Ok();
}

std::vector<LifecycleManager::EntityInfo> LifecycleManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EntityInfo> out;
    out.reserve(entities_.size());
    for (const auto& [id, e] : entities_) out.push_back(EntityInfo{id, e.version, e.dependencies, e.state});
    return out;
}

size_t LifecycleManager::EntityCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entities_.size();
}

HealthReport LifecycleManager::GetHealth() const {
    HealthReport r;
    r.detail = std::format("{} entities tracked", EntityCount());
    return r;
}

} // namespace bps
