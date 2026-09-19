#include "core/modules/ModuleManager.hpp"

#include "core/config/Json.hpp"
#include "core/lifecycle/LifecycleManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <format>

namespace bps {

ModuleManager& ModuleManager::Instance() {
    static ModuleManager instance;
    return instance;
}

LifecycleState ModuleManager::ToLifecycle(ModuleState s) {
    // Maps the module-manager vocabulary onto the canonical 12-state machine (12 §5).
    switch (s) {
        case ModuleState::Installed:   return LifecycleState::Registered;
        case ModuleState::Loaded:      return LifecycleState::Initialized;
        case ModuleState::Initialized: return LifecycleState::Initialized;
        case ModuleState::Running:     return LifecycleState::Running;
        case ModuleState::Paused:      return LifecycleState::Paused;
        case ModuleState::Suspended:   return LifecycleState::Suspended;
        case ModuleState::Stopped:     return LifecycleState::Stopped;
        case ModuleState::Unloaded:    return LifecycleState::Destroyed;
        case ModuleState::Disabled:    return LifecycleState::Stopped;
        case ModuleState::Failed:      return LifecycleState::Stopped;
    }
    return LifecycleState::Stopped;
}

Result<void> ModuleManager::RegisterModule(std::shared_ptr<IModule> module) {
    if (!module)
        return Error::Make(Err::InvalidArgument, "ModuleManager", "null module");
    const ModuleManifest& m = module->Manifest();

    std::lock_guard<std::mutex> lock(mutex_);
    if (modules_.count(m.id) > 0)
        return Error::Make(Err::AlreadyExists, "ModuleManager", "module already registered: " + m.id);

    // Register into the canonical lifecycle manager (12).
    auto r = LifecycleManager::Instance().RegisterEntity("module:" + m.id, m.version, m.dependencies);
    if (!r.ok()) return r;

    modules_[m.id] = std::move(module);
    states_[m.id] = ModuleState::Installed;
    Logger::Instance().Info("Module registered: " + m.id + " v" + m.version.ToString(), "Modules");
    return Ok();
}

Result<void> ModuleManager::RegisterFactory(std::string_view id,
                                             std::function<std::shared_ptr<IModule>()> factory) {
    if (!factory)
        return Error::Make(Err::InvalidArgument, "ModuleManager", "null module factory");
    std::lock_guard<std::mutex> lock(mutex_);
    if (factories_.count(std::string(id)) > 0)
        return Error::Make(Err::AlreadyExists, "ModuleManager", "factory already registered: " + std::string(id));
    factories_[std::string(id)] = std::move(factory);
    return Ok();
}

Result<void> ModuleManager::Discover(std::string_view searchPath) {
    std::vector<ModuleManifest> manifests;
    auto& fsys = platform::PlatformAccessor::Get().Filesystem();
    if (!fsys.IsDirectory(searchPath)) return Ok();
    auto entries = fsys.Enumerate(searchPath);
    if (!entries.ok()) return Ok();
    for (const auto& path : entries.value()) {
        if (!fsys.IsRegularFile(path) || fsys.Extension(path) != ".json") continue;
        auto text = fsys.ReadText(path);
        if (!text.ok()) {
            Logger::Instance().Warning("ModuleManager: cannot read sidecar " + path);
            continue;
        }
        auto parsed = json::Parse(text.value());
        if (!parsed.ok()) {
            Logger::Instance().Warning("ModuleManager: bad sidecar " + path);
            continue;
        }
        const json::Value& v = parsed.value();
        ModuleManifest m;
        m.id = std::string(v.Find("id") ? v.Find("id")->asString() : "");
        m.version = Version::Parse(v.Find("version") ? v.Find("version")->asString() : "0.0.0");
        m.author = std::string(v.Find("author") ? v.Find("author")->asString() : "");
        m.requiredCoreVersion =
            Version::Parse(v.Find("requiredCoreVersion") ? v.Find("requiredCoreVersion")->asString()
                                                         : "0.0.0");
        if (m.id.empty()) continue;
        manifests.push_back(std::move(m));
    }

    int registered = 0;
    for (auto& m : manifests) {
        std::shared_ptr<IModule> module;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (std::find(discovered_.begin(), discovered_.end(), m.id) != discovered_.end()) continue;
            auto fit = factories_.find(m.id);
            if (fit == factories_.end()) {
                Logger::Instance().Warning("ModuleManager: no factory for discovered module '" +
                                           m.id + "'");
                continue;
            }
            module = fit->second();
            discovered_.push_back(m.id);
        }
        if (!module) {
            Logger::Instance().Error("ModuleManager: factory for '" + m.id + "' returned null");
            continue;
        }
        auto r = RegisterModule(std::move(module));
        if (!r.ok())
            Logger::Instance().Warning("ModuleManager: failed to register discovered module '" +
                                       m.id + "': " + r.error().message);
        else
            ++registered;
    }
    Logger::Instance().Info(std::format("ModuleManager: discovered {} sidecars, registered {}",
                                        manifests.size(), registered),
                            "Modules");
    return Ok();
}

size_t ModuleManager::DiscoveredCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discovered_.size();
}

Result<void> ModuleManager::UnregisterModule(std::string_view id) {
    std::shared_ptr<IModule> module;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = modules_.find(std::string(id));
        if (it == modules_.end())
            return Error::Make(Err::NotFound, "ModuleManager",
                               "module not found: " + std::string(id));
        module = it->second;
        modules_.erase(it);
        states_.erase(std::string(id));
        factories_.erase(std::string(id));
    }
    // Run OnUnload so the module releases subscriptions and cancels timers
    // (08 §3) — otherwise e.g. an active autoplay sequence would keep calling
    // into a destroyed module. A throwing module must not block unregistration.
    try {
        (void)module->OnUnload();
    } catch (...) {
        Logger::Instance().Error("ModuleManager: " + std::string(id) +
                                 " threw during unload (ignored)");
    }
    // The lifecycle entity was registered alongside (12): remove it so a later
    // re-registration of the same id is not rejected.
    return LifecycleManager::Instance().UnregisterEntity("module:" + std::string(id));
}

Result<void> ModuleManager::DoTransition(std::string_view id, ModuleState target) {
    std::shared_ptr<IModule> module;
    ModuleState from;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = modules_.find(std::string(id));
        if (it == modules_.end())
            return Error::Make(Err::NotFound, "ModuleManager", "module not found: " + std::string(id));
        module = it->second;
        from = states_[std::string(id)];
    }
    if (from == target) return Ok();

    std::string entityId = "module:" + std::string(id);
    auto& lc = LifecycleManager::Instance();

    // Restart path: a Stopped/Unloaded module re-enters through the reset edge
    // Stopped -> Registered (12 §3), then walks forward to Running. Plain lifecycle
    // Transition() rejects backward jumps, so the reset is explicit.
    if (target == ModuleState::Running &&
        (from == ModuleState::Stopped || from == ModuleState::Unloaded)) {
        auto reset = lc.Transition(entityId, LifecycleState::Registered);
        if (!reset.ok()) return reset;
    }

    auto r = lc.TransitionDependent(entityId, ToLifecycle(target));
    if (!r.ok()) return r;

    // Module callbacks (08 §3). Start vs Resume depends on the origin state.
    Result<void> cb = Ok();
    switch (target) {
        case ModuleState::Loaded:
            cb = module->OnLoad();
            break;
        case ModuleState::Running:
            // A module started without an explicit Load() must still run OnLoad()
            // (e.g. to subscribe to events) before OnStart (08 §3).
            if (from == ModuleState::Installed) {
                if (auto r = module->OnLoad(); !r.ok()) return r;
            }
            cb = (from == ModuleState::Paused || from == ModuleState::Suspended)
                     ? module->OnResume()
                     : module->OnStart();
            break;
        case ModuleState::Paused:
            cb = module->OnPause();
            break;
        case ModuleState::Suspended:
            cb = module->OnSuspend();
            break;
        case ModuleState::Stopped:
            cb = module->OnStop();
            break;
        case ModuleState::Unloaded:
            cb = module->OnUnload();
            break;
        default:
            break;
    }
    if (!cb.ok()) {
        // Rollback the lifecycle state (12 §10) and surface the failure.
        if (from != ModuleState::Installed && from != ModuleState::Disabled)
            (void)lc.Transition(entityId, ToLifecycle(from));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            states_[std::string(id)] = ModuleState::Failed;
            lastError_ = "module callback failed: " + std::string(id) + " (" + cb.error().message + ")";
        }
        return Error::Make(Err::Module_LoadFailed, "ModuleManager",
                           "module callback failed: " + std::string(id), &cb.error());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[std::string(id)] = target;
    }
    (void)EventBus::Instance().Publish(events::ModuleStateChanged{std::string(id),
                                                                 static_cast<int>(from),
                                                                 static_cast<int>(target)});
    return Ok();
}

Result<void> ModuleManager::Load(std::string_view id) {
    return DoTransition(id, ModuleState::Loaded);
}

Result<void> ModuleManager::Start(std::string_view id) {
    return DoTransition(id, ModuleState::Running);
}

Result<void> ModuleManager::Stop(std::string_view id) {
    return DoTransition(id, ModuleState::Stopped);
}

Result<void> ModuleManager::Pause(std::string_view id) {
    return DoTransition(id, ModuleState::Paused);
}

Result<void> ModuleManager::Resume(std::string_view id) {
    return DoTransition(id, ModuleState::Running);
}

Result<void> ModuleManager::Suspend(std::string_view id) {
    return DoTransition(id, ModuleState::Suspended);
}

Result<void> ModuleManager::Reload(std::string_view id) {
    auto r = DoTransition(id, ModuleState::Stopped);
    if (!r.ok()) return r;
    return DoTransition(id, ModuleState::Running);
}

Result<void> ModuleManager::Disable(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(std::string(id));
    if (it == states_.end())
        return Error::Make(Err::NotFound, "ModuleManager", "module not found: " + std::string(id));
    it->second = ModuleState::Disabled;
    return Ok();
}

ModuleState ModuleManager::State(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(std::string(id));
    return it == states_.end() ? ModuleState::Unloaded : it->second;
}

std::vector<ModuleManager::ModuleInfo> ModuleManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> out;
    out.reserve(modules_.size());
    for (const auto& [id, module] : modules_) {
        auto st = states_.find(id);
        out.push_back(ModuleInfo{id, module->Manifest().version,
                                 st == states_.end() ? ModuleState::Installed : st->second});
    }
    return out;
}

std::shared_ptr<IModule> ModuleManager::GetModule(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(std::string(id));
    return it == modules_.end() ? nullptr : it->second;
}

HealthReport ModuleManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t failing = 0;
    for (const auto& [id, st] : states_)
        if (st == ModuleState::Failed) ++failing;
    r.errorCount = failing;
    r.state = failing == 0 ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("{} modules, {} failed", modules_.size(), failing);
    r.lastError = lastError_;
    return r;
}

} // namespace bps
