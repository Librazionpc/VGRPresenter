# 09 — PluginManager Specification

| Field | Value |
|---|---|
| **System** | PluginManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/plugins/PluginManager.hpp`, `core/plugins/PluginManager.cpp`, `interfaces/include/interfaces/IPlugin.hpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), EventBus (05), ThreadPool (06), ResourceManager (10), MemoryManager (11) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The PluginManager loads **external native plugins** — shared libraries (DLL / SO /
DYLIB) that extend the engine with capabilities such as OBS, Bible providers, AI
inference, PowerPoint import, and NDI. It exists to make third-party extension **safe,
versioned, and isolated**: the engine must never crash, leak, or hang because of a
plugin, and plugins must never see each other's internals.

## 2. Responsibilities

- **Discovery** of plugin packages (manifests + native libraries) in search paths.
- **Loading / Unloading** of shared libraries with strict entry/exit contracts.
- **Version checking** (plugin manifest vs. engine core version and ABI version).
- **Signature verification** (optional but supported: code signing for shipped plugins).
- **Sandboxing** — where the platform allows, load plugins in a restricted context
  (separate heap policy, restricted OS handles) and always enforce logical isolation.
- **Dependencies** between plugins (DAG, topological load, cycle rejection).
- **Isolation** — a plugin can only reach engine APIs through the plugin API surface
  (`IPlugin` + resolved services from 04); no direct core internals.
- **Crash protection** — a crashing plugin must be contained (caught at the API
  boundary), reported, and disabled; in-process where unavoidable, out-of-process when
  the platform supports it.
- **Future plugin families**: OBS, Bible, AI, PowerPoint, NDI, Streaming, Remote.

## 3. Public API

```cpp
class PluginManager final {
public:
    static PluginManager& Instance();

    // Discovery & lifecycle
    Result<void>   Discover(std::string_view searchPath);
    Result<std::shared_ptr<IPlugin>> Load(std::string_view pluginId, LoadOptions opts = {});
    Result<void>   Unload(std::string_view pluginId);
    Result<void>   Reload(std::string_view pluginId);                // 00 §5

    // Verification & sandbox
    Result<void>   VerifySignature(std::string_view pluginId);       // optional, policy-gated
    Result<void>   SetSandbox(std::string_view pluginId, const SandboxProfile& profile);

    // Dependencies
    Result<void>   ResolveDependencies(std::string_view pluginId);   // topo order

    // Introspection / diagnostics
    std::vector<PluginInfo> Snapshot() const;
    HealthReport GetHealth(std::string_view pluginId) const noexcept;
    HealthReport GetHealth() const noexcept;                         // 00 §7
    PluginMetrics Metrics() const noexcept;                          // 00 §4

    // Crash containment policy
    Result<void>   SetIsolationPolicy(IsolationPolicy policy);       // InProcess | OutOfProcess(where available)
};
```

`IPlugin` (interface, `interfaces/include/interfaces/IPlugin.hpp`): `id()`, `manifest()` (00 §6),
`OnLoad()/OnUnload()`, `OnStart()/OnStop()`, `GetHealth()`, plus an opaque
capability/extension accessor (e.g. `RegisterCapability(name, handler)`).

## 4. Internal Components

| Component | Role |
|---|---|
| `PluginManager` | Facade; registry + policies |
| `PluginRegistry` | Manifest-backed registry (id → PluginInfo, state) |
| `LibraryLoader` | Platform loader (`dlopen` / `LoadLibrary`); symbol binding to the plugin ABI |
| `ABICheck` | Version + layout checks against the exported `plugin_abi()` symbol |
| `SignatureVerifier` | Signature/certificate verification (platform crypto) |
| `Sandbox` | Platform sandbox profiles; logical capability gate |
| `IsolationLayer` | API-boundary marshalling; crash containment (in-process try/catch at boundary, out-of-process via IPC) |
| `DependencyGraph` | Plugin DAG; topo order; cycle detection |
| `CrashGuard` | Watchdog + post-crash quarantine (a plugin that crashed is not auto-reloaded) |

## 5. Plugin States

```
Discovered --> Verified --> Loaded --> Started --> Running
   |             |            |         |           |
   |             +-- Failed --+---------+-----------+-> Quarantined (post-crash)
   +---> Disabled (policy/allowlist)                  |
                                                      v
   Running --> Stopped --> Unloaded --> Disabled   (or retry after version fix)
```

| State | Meaning |
|---|---|
| `Discovered` | Manifest found; not verified |
| `Verified` | Signature (if required) + ABI checks passed |
| `Loaded` | Library loaded, `OnLoad()` called |
| `Started` | `OnStart()` called |
| `Running` | Active |
| `Stopped` | Stopped, still loaded |
| `Unloaded` | Library unloaded |
| `Disabled` | Opted out or quarantined by policy |
| `Quarantined` | Crashed; blocked from auto-reload until explicitly re-enabled |
| `Failed` | Load/verification failed; blocked until fixed and re-discovered |

## 6. Threading Model

- **Load/unload serialized** on a single control path (never concurrent with itself).
- **Runtime:** plugins use the ThreadPool (06) for their work and the EventBus (05) for
  communication, exactly like modules (08). The PluginManager only polices the boundary.
- **Out-of-process isolation** (where enabled) uses the IPC channel for all plugin calls;
  in that mode plugin callbacks are marshalled and a plugin hang triggers a kill+quarantine.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.plugin.discovered` | `PluginInfo` | Manifest found |
| `engine.plugin.loaded` | `{id, version}` | Loaded |
| `engine.plugin.unloaded` | `{id, version}` | Unloaded |
| `engine.plugin.started` / `stopped` | `{id}` | Start/stop |
| `engine.plugin.signature_failed` | `{id, reason}` | Verification failure |
| `engine.plugin.crashed` | `{id, fault}` | CrashGuard containment |
| `engine.plugin.quarantined` | `{id, policy}` | Plugin quarantined |
| `engine.plugin.state_changed` | `{id, from, to}` | Any transition |
| `engine.plugin.updated` | `{id, from, to}` | Plugin updated to a new version |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.resource.pressure_high` (10) | Unload non-essential plugins (lowest priority first) |
| `engine.kernel.state_changed` (01) | `ShuttingDown` → unload plugins in reverse dependency order |
| `engine.config.hot_reload` (03) | Re-read allowlists / sandbox profiles / signatures policy |
| `engine.module.requested` (08) | Load a plugin that a module declares as a dependency |

## 9. Dependencies

- **Depends on:** 02, 03, 04, 05, 06, 10, 11.
- **Uses after init:** LifecycleManager (12) hooks for plugin lifecycle; Kernel (01)
  crash/panic reporting.
- **Provides:** native extension loading to ModuleManager (08) — a module may be backed
  by a plugin; both share the manifest rules (00 §6).

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Library fails to load (missing symbols, ABI mismatch) | `Error` with symbol/ABI detail; plugin stays `Failed`; engine unaffected |
| Signature verification fails | Refuse to load; `engine.plugin.signature_failed`; policy decides warn-vs-block |
| Plugin crashes in-process | CrashGuard catches at the boundary, marks `Quarantined`, publishes `engine.plugin.crashed`; **no other plugin is affected** |
| Plugin hangs | Watchdog timeout → if in-process: quarantine on the next API call; if out-of-process: kill the child + quarantine |
| Plugin tries to reach forbidden APIs | Sandbox denies at the capability gate; counted metric; warning logged |
| Unload fails (plugin holds resources) | Force-unload after grace period; leak-checked by 11 and reported |

## 11. Performance Goals

| Goal | Target |
|---|---|
| Load + verify a plugin | **< 20 ms** (typical, signature off) |
| In-process API boundary overhead | **< 1 µs** per call |
| Out-of-process call overhead | **< 100 µs** (IPC round-trip) |
| Crash containment recovery | quarantine decision **< 10 ms** after the fault |
| Idle overhead with 10 plugins loaded | < 0.5% CPU |

## 12. Future Extensions

- **Out-of-process plugins by default** (full sandbox) for all third-party code.
- **Plugin marketplace** with signed, versioned packages and automatic updates.
- **Plugin-to-plugin public API contracts** (like modules, still via the bus/services).
- **GPU/VRAM-aware plugins** (NDI, renderers) coordinated with ResourceManager (10).
- **Scripted plugins** (Lua/Python) layered over the same `IPlugin` facade.

## 13. Testing Requirements

- **Unit:** discovery, ABI checks, signature paths, dependency topo order, sandbox
  denial, state transitions.
- **Stress:** 50 plugins load/unload in a loop; crash one plugin mid-traffic — others
  unaffected; no leaks (11 asserts).
- **Performance:** assert §11 budgets (boundary overhead, load time).
- **Failure:** corrupt library, symbol mismatch, fake signature, hang injection,
  forbidden-API attempts — verify quarantine and engine survival.
