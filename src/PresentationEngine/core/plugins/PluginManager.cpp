#include "core/plugins/PluginManager.hpp"

#include "core/config/Json.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <format>

namespace bps {

PluginManager& PluginManager::Instance() {
    static PluginManager instance;
    return instance;
}

PluginManifest PluginManager::ParseManifestFile(const std::string& path) {
    PluginManifest m;
    auto text = platform::PlatformAccessor::Get().Filesystem().ReadText(path);
    if (!text.ok()) {
        Logger::Instance().Warning("PluginManager: cannot read sidecar " + path);
        return m;
    }
    auto parsed = json::Parse(text.value());
    if (!parsed.ok()) {
        Logger::Instance().Warning("PluginManager: bad sidecar " + path);
        return m;
    }
    const json::Value& v = parsed.value();
    m.id = std::string(v.Find("id") ? v.Find("id")->asString() : "");
    m.version = Version::Parse(v.Find("version") ? v.Find("version")->asString() : "0.0.0");
    m.author = std::string(v.Find("author") ? v.Find("author")->asString() : "");
    m.requiredCoreVersion =
        Version::Parse(v.Find("requiredCoreVersion") ? v.Find("requiredCoreVersion")->asString()
                                                     : "0.0.0");
    m.abiVersion = v.Find("abiVersion") ? static_cast<int>(v.Find("abiVersion")->asInt(1)) : 1;
    m.libraryPath = std::string(v.Find("libraryPath") ? v.Find("libraryPath")->asString() : "");
    if (const auto* deps = v.Find("dependencies") ? v.Find("dependencies")->asArray() : nullptr)
        for (const auto& d : *deps) m.dependencies.push_back(std::string(d.asString()));
    if (const auto* caps = v.Find("capabilities") ? v.Find("capabilities")->asArray() : nullptr)
        for (const auto& c : *caps) m.capabilities.push_back(std::string(c.asString()));
    return m;
}

std::vector<PluginManifest> PluginManager::LoadManifests(std::string_view searchPath) {
    std::vector<PluginManifest> out;
    auto& fsys = platform::PlatformAccessor::Get().Filesystem();
    if (!fsys.IsDirectory(searchPath)) return out;
    auto entries = fsys.Enumerate(searchPath);
    if (!entries.ok()) return out;
    for (const auto& p : entries.value()) {
        if (!fsys.IsRegularFile(p)) continue;
        if (fsys.Extension(p) != ".json") continue;
        PluginManifest m = ParseManifestFile(p);
        if (m.id.empty() || m.libraryPath.empty()) continue;
        // Resolve relative library paths against the sidecar's directory.
        if (m.libraryPath.front() != '/') {
            size_t slash = p.find_last_of('/');
            std::string dir = slash == std::string::npos ? "." : p.substr(0, slash);
            if (auto full = fsys.Absolute(fsys.Join(dir, m.libraryPath)); full.ok())
                m.libraryPath = full.value();
        }
        out.push_back(std::move(m));
    }
    return out;
}

Result<void> PluginManager::Discover(std::string_view searchPath) {
    auto manifests = LoadManifests(searchPath);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& m : manifests) {
        auto it = std::find_if(discovered_.begin(), discovered_.end(),
                               [&](const PluginManifest& e) { return e.id == m.id; });
        if (it == discovered_.end()) {
            discovered_.push_back(std::move(m));
            Logger::Instance().Info("Plugin discovered: " + discovered_.back().id, "Plugins");
        }
    }
    return Ok();
}

std::shared_ptr<IPlugin> PluginManager::OpenLibrary(const PluginManifest& m, void** handleOut) {
    // All library access goes through the PAL (Phase 2): dlopen/dlsym/dlclose
    // live in platform/linux/LinuxLibrary.cpp, never here.
    auto libShared = platform::PlatformAccessor::Shared();
    if (!libShared) {
        Logger::Instance().Error("PluginManager: PAL unavailable — cannot load plugins");
        return nullptr;
    }
    auto& lib = libShared->Library();
    auto loaded = lib.Load(m.libraryPath);
    if (!loaded.ok()) {
        Logger::Instance().Error("PluginManager: load failed for " + m.libraryPath + ": " +
                                 loaded.error().message);
        return nullptr;
    }
    void* h = loaded.value();
    using CreateFn = IPlugin* (*)();
    using DestroyFn = void (*)(IPlugin*);
    auto createSym = lib.Symbol(h, "bps_plugin_create");
    auto destroySym = lib.Symbol(h, "bps_plugin_destroy");
    if (!createSym.ok() || !destroySym.ok()) {
        Logger::Instance().Error("PluginManager: missing bps_plugin_create/destroy symbols in " +
                                 m.libraryPath);
        (void)lib.Unload(h);
        return nullptr;
    }
    auto create = reinterpret_cast<CreateFn>(createSym.value());
    auto destroy = reinterpret_cast<DestroyFn>(destroySym.value());
    IPlugin* raw = create();
    if (!raw) {
        (void)lib.Unload(h);
        return nullptr;
    }
    *handleOut = h;
    // Capture the shared PAL so the deleter can unload even if the accessor is
    // re-installed later.
    return std::shared_ptr<IPlugin>(raw, [libShared, destroy, h](IPlugin* p) {
        destroy(p);
        (void)libShared->Library().Unload(h);
    });
}

Result<std::shared_ptr<IPlugin>> PluginManager::Load(std::string_view pluginId) {
    PluginManifest manifest;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (instances_.count(std::string(pluginId)) > 0)
            return Error::Make(Err::AlreadyExists, "PluginManager", "plugin already loaded: " + std::string(pluginId));
        auto it = std::find_if(discovered_.begin(), discovered_.end(),
                               [&](const PluginManifest& m) { return m.id == pluginId; });
        if (it == discovered_.end())
            return Error::Make(Err::NotFound, "PluginManager", "plugin not discovered: " + std::string(pluginId));
        manifest = *it;
    }

    if (manifest.abiVersion != kPluginAbiVersion)
        return Error::Make(Err::VersionMismatch, "PluginManager",
                           std::format("ABI mismatch for '{}': sidecar={} engine={}",
                                        manifest.id, manifest.abiVersion, kPluginAbiVersion));
    if (manifest.requiredCoreVersion > kEngineVersion)
        return Error::Make(Err::VersionMismatch, "PluginManager",
                           "plugin '" + manifest.id + "' requires core v" +
                               manifest.requiredCoreVersion.ToString() + " (engine is v" +
                               kEngineVersion.ToString() + ")");

    // Dependency resolution (09 §Dependencies): load declared dependencies first.
    // `loading_` guards against cycles (A depends on B depends on A).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loading_.count(manifest.id) > 0)
            return Error::Make(Err::Plugin_LoadFailed, "PluginManager",
                               "cyclic plugin dependency involving '" + manifest.id + "'");
        loading_.insert(manifest.id);
    }
    for (const auto& dep : manifest.dependencies) {
        bool loaded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loaded = instances_.count(dep) > 0;
        }
        if (!loaded) {
            auto r = Load(dep);
            if (!r.ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                loading_.erase(manifest.id);
                return Error::Make(Err::Plugin_LoadFailed, "PluginManager",
                                   "dependency '" + dep + "' of '" + manifest.id +
                                       "' failed to load",
                                   &r.error());
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loading_.erase(manifest.id);
    }

    // Crash protection (09 §Crash protection): a throwing plugin is quarantined
    // instead of taking the engine down.
    auto safe = [](const std::function<Result<void>()>& f) -> Result<void> {
        try {
            return f();
        } catch (const std::exception& ex) {
            return Error::Make(Err::Plugin_LoadFailed, "PluginManager",
                               std::string("plugin threw: ") + ex.what());
        } catch (...) {
            return Error::Make(Err::Plugin_LoadFailed, "PluginManager", "plugin threw (unknown)");
        }
    };

    void* handle = nullptr;
    auto instance = OpenLibrary(manifest, &handle);
    if (!instance)
        return Error::Make(Err::Plugin_LoadFailed, "PluginManager",
                           "failed to open library: " + manifest.libraryPath);

    auto r = safe([&]() { return instance->OnLoad(); });
    if (!r.ok()) {
        instance.reset();  // closes the handle
        return r.error();
    }
    auto r2 = safe([&]() { return instance->OnStart(); });
    if (!r2.ok()) {
        (void)safe([&]() { return instance->OnUnload(); });
        instance.reset();
        return r2.error();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_[manifest.id] = handle;
        instances_[manifest.id] = instance;
        states_[manifest.id] = 2;
    }
    (void)EventBus::Instance().Publish(events::PluginStateChanged{manifest.id, 0, 2});
    Logger::Instance().Info("Plugin loaded: " + manifest.id + " v" + manifest.version.ToString(),
                            "Plugins");
    return instance;
}

Result<void> PluginManager::Unload(std::string_view pluginId) {
    std::shared_ptr<IPlugin> instance;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(std::string(pluginId));
        if (it == instances_.end())
            return Error::Make(Err::NotFound, "PluginManager", "plugin not loaded: " + std::string(pluginId));
        instance = it->second;
        instances_.erase(it);
        handles_.erase(std::string(pluginId));
        states_[std::string(pluginId)] = 0;
    }
    if (instance) {
        try {
            (void)instance->OnUnload();
        } catch (...) {
            Logger::Instance().Error("PluginManager: " + std::string(pluginId) +
                                     " threw during unload (ignored)");
        }
    }
    // shared_ptr reset (deleter) runs when `instance` leaves scope — closes the library.
    (void)EventBus::Instance().Publish(events::PluginStateChanged{std::string(pluginId), 2, 0});
    Logger::Instance().Info("Plugin unloaded: " + std::string(pluginId), "Plugins");
    return Ok();
}

Result<void> PluginManager::Reload(std::string_view pluginId) {
    if (auto r = Unload(pluginId); !r.ok()) return r;
    auto loaded = Load(pluginId);
    if (!loaded.ok()) return loaded.error();
    return Ok();
}

Result<Version> PluginManager::Update(std::string_view pluginId,
                                      std::string_view stagedManifestPath) {
    PluginManifest fresh = ParseManifestFile(std::string(stagedManifestPath));
    if (fresh.id.empty())
        return Error::Make(Err::ParseError, "PluginManager",
                           "bad staged manifest: " + std::string(stagedManifestPath));
    if (fresh.id != pluginId)
        return Error::Make(Err::InvalidArgument, "PluginManager",
                           "staged manifest id '" + fresh.id + "' does not match '" +
                               std::string(pluginId) + "'");
    // Resolve relative library paths against the staged manifest's directory.
    if (!fresh.libraryPath.empty() && fresh.libraryPath.front() != '/') {
        auto& fsys = platform::PlatformAccessor::Get().Filesystem();
        std::string dir(stagedManifestPath);
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        if (auto full = fsys.Absolute(fsys.Join(dir, fresh.libraryPath)); full.ok())
            fresh.libraryPath = full.value();
    }

    Version oldVersion = fresh.version;
    bool wasLoaded = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(discovered_.begin(), discovered_.end(),
                               [&](const PluginManifest& m) { return m.id == fresh.id; });
        if (it == discovered_.end())
            return Error::Make(Err::NotFound, "PluginManager",
                               "plugin not discovered: " + std::string(pluginId));
        if (!(fresh.version > it->version))
            return Error::Make(Err::VersionMismatch, "PluginManager",
                               "staged version " + fresh.version.ToString() +
                                   " is not newer than current " + it->version.ToString());
        oldVersion = it->version;
        *it = fresh;                              // stage the new manifest
        wasLoaded = instances_.count(fresh.id) > 0;
    }
    if (wasLoaded) (void)Unload(pluginId);

    auto loaded = Load(pluginId);   // may fail if the new library is absent
    (void)EventBus::Instance().Publish(
        events::PluginUpdated{std::string(pluginId), oldVersion, fresh.version});
    Logger::Instance().Info("Plugin updated: " + std::string(pluginId) + " " +
                                oldVersion.ToString() + " -> " + fresh.version.ToString(),
                            "Plugins");
    if (!loaded.ok())
        return Error::Make(Err::Plugin_LoadFailed, "PluginManager",
                           "manifest staged at v" + fresh.version.ToString() +
                               " but the new library failed to load: " +
                               loaded.error().message,
                           &loaded.error());
    return fresh.version;
}

Result<void> PluginManager::SetSandbox(std::string_view pluginId, const SandboxProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(discovered_.begin(), discovered_.end(),
                           [&](const PluginManifest& m) { return m.id == pluginId; });
    if (it == discovered_.end())
        return Error::Make(Err::NotFound, "PluginManager",
                           "plugin not discovered: " + std::string(pluginId));
    sandboxes_[std::string(pluginId)] = profile;
    return Ok();
}

SandboxProfile PluginManager::SandboxOf(std::string_view pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sandboxes_.find(std::string(pluginId));
    return it == sandboxes_.end() ? SandboxProfile{} : it->second;
}

Result<void> PluginManager::CheckCapability(std::string_view pluginId,
                                            std::string_view capability) const {
    std::string reason;
    bool denied = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(discovered_.begin(), discovered_.end(),
                               [&](const PluginManifest& m) { return m.id == pluginId; });
        if (it == discovered_.end())
            return Error::Make(Err::NotFound, "PluginManager",
                               "plugin not discovered: " + std::string(pluginId));
        bool declared = std::find(it->capabilities.begin(), it->capabilities.end(),
                                  std::string(capability)) != it->capabilities.end();
        auto sbIt = sandboxes_.find(std::string(pluginId));
        SandboxProfile prof = sbIt == sandboxes_.end() ? SandboxProfile{} : sbIt->second;
        if (!declared)
            reason = "capability not declared in the plugin manifest";
        else if (prof.denyNetwork && capability == "network")
            reason = "network access denied by sandbox profile";
        else if (prof.denyFileWrite && capability == "file-write")
            reason = "file writes denied by sandbox profile";
        else if (prof.denyProcess && capability == "process")
            reason = "process spawning denied by sandbox profile";
        denied = !reason.empty();
        if (denied) denials_[std::string(pluginId)]++;
    }
    if (!denied) return Ok();
    // Publish outside the lock so a subscriber may call back into us.
    (void)EventBus::Instance().Publish(events::PluginSandboxDenied{
        std::string(pluginId), std::string(capability), reason});
    return Error::Make(Err::Plugin_CapabilityDenied, "PluginManager",
                       "sandbox denied '" + std::string(capability) + "' for '" +
                           std::string(pluginId) + "': " + reason);
}

Result<void> PluginManager::SetIsolationPolicy(IsolationPolicy policy) {
    if (policy == IsolationPolicy::OutOfProcess)
        return Error::Make(Err::Unsupported, "PluginManager",
                           "out-of-process plugin isolation is post-v1 (09 §12); v1 enforces "
                           "logical isolation (IPlugin boundary + crash quarantine) in-process");
    isolation_ = policy;
    return Ok();
}

uint64_t PluginManager::SandboxDenials(std::string_view pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = denials_.find(std::string(pluginId));
    return it == denials_.end() ? 0 : it->second;
}

Result<void> PluginManager::VerifySignature(std::string_view pluginId) {
    // v1 placeholder: signature verification ships with the plugin SDK tooling (09 §10).
    return Error::Make(Err::NotImplemented, "PluginManager",
                       "signature verification for '" + std::string(pluginId) +
                           "' is not available in v1 (09 §10)");
}

std::vector<PluginInfo> PluginManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginInfo> out;
    for (const auto& m : discovered_) {
        auto st = states_.find(m.id);
        out.push_back(PluginInfo{m.id, m.version, m.libraryPath,
                                 st == states_.end() ? 0 : st->second});
    }
    return out;
}

size_t PluginManager::DiscoveredCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discovered_.size();
}

size_t PluginManager::LoadedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instances_.size();
}

HealthReport PluginManager::GetHealth() const {
    HealthReport r;
    r.detail = std::format("{} discovered, {} loaded", DiscoveredCount(), LoadedCount());
    return r;
}

} // namespace bps
