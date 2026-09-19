#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IPlugin.hpp"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bps {

struct PluginInfo {
    std::string id;
    Version version;
    std::string libraryPath;
    int state = 0;   // 0 = Discovered, 1 = Loaded, 2 = Running, 3 = Quarantined
};

// Logical sandbox profile (09 §Sandbox). Enforcement is at the plugin API
// boundary: the engine never lets a plugin reach core internals directly
// (everything goes through IPlugin + resolved services), and CheckCapability()
// gates declared capabilities against the profile. OS-level isolation
// (seccomp / subprocess host) is the documented post-v1 extension (09 §12).
struct SandboxProfile {
    std::string name;          // e.g. "default" / "lockdown"
    bool denyNetwork = false;
    bool denyFileWrite = false;
    bool denyProcess = false;
    bool readOnly = false;     // read-only view of engine resources
};

// Where plugin code executes (09 §Isolation). v1 runs plugins in-process with
// a crash boundary + quarantine; OutOfProcess is post-v1.
enum class IsolationPolicy : int { InProcess = 0, OutOfProcess };

// Native shared-library loader (docs/specs/09).
class PluginManager final : public IService {
public:
    static PluginManager& Instance();

    // Scans <searchPath> for plugin sidecar manifests (<id>.json).
    Result<void> Discover(std::string_view searchPath);
    Result<std::shared_ptr<IPlugin>> Load(std::string_view pluginId);
    Result<void> Unload(std::string_view pluginId);
    using IService::Reload;
    Result<void> Reload(std::string_view pluginId);
    Result<void> VerifySignature(std::string_view pluginId);   // v1 placeholder (09 §3)

    // Update (09 §Update): stages a newer sidecar manifest (id must match,
    // version must be strictly newer), unloads the old build, replaces the
    // registry entry, and (re)loads the new library. Returns the new version.
    Result<Version> Update(std::string_view pluginId, std::string_view stagedManifestPath);

    // Sandbox (09 §Sandbox): per-plugin logical profiles + capability gate.
    Result<void> SetSandbox(std::string_view pluginId, const SandboxProfile& profile);
    SandboxProfile SandboxOf(std::string_view pluginId) const;
    Result<void> CheckCapability(std::string_view pluginId, std::string_view capability) const;
    Result<void> SetIsolationPolicy(IsolationPolicy policy);
    IsolationPolicy Isolation() const noexcept { return isolation_; }
    uint64_t SandboxDenials(std::string_view pluginId) const;

    std::vector<PluginInfo> Snapshot() const;
    size_t DiscoveredCount() const;
    size_t LoadedCount() const;

    const char* ServiceName() const noexcept override { return "PluginManager"; }
    HealthReport GetHealth() const override;

private:
    PluginManager() = default;

    static PluginManifest ParseManifestFile(const std::string& path);   // single sidecar
    std::vector<PluginManifest> LoadManifests(std::string_view searchPath);
    std::shared_ptr<IPlugin> OpenLibrary(const PluginManifest& manifest, void** handleOut);

    mutable std::mutex mutex_;
    std::vector<PluginManifest> discovered_;                                     // guarded
    std::unordered_map<std::string, void*> handles_;                             // guarded
    std::unordered_map<std::string, std::shared_ptr<IPlugin>> instances_;        // guarded
    std::unordered_map<std::string, int> states_;                                // guarded
    std::unordered_map<std::string, SandboxProfile> sandboxes_;                  // guarded
    mutable std::unordered_map<std::string, uint64_t> denials_;                  // guarded
    std::unordered_set<std::string> loading_;    // cycle guard for dependency resolution
    IsolationPolicy isolation_{IsolationPolicy::InProcess};
};

} // namespace bps
