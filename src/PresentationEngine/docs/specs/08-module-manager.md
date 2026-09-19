# 08 — ModuleManager Specification

| Field | Value |
|---|---|
| **System** | ModuleManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/modules/ModuleManager.hpp`, `core/modules/ModuleManager.cpp`, `interfaces/include/interfaces/IModule.hpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), EventBus (05), ThreadPool (06), TaskScheduler (07), ResourceManager (10), MemoryManager (11), PluginManager (09) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The ModuleManager **controls the engine's modules** — the feature packages that turn the
core into a presentation application (Presentation, Bible, Media, Songs, Streaming,
NDI, MIDI, AI, Cloud, Remote). It owns their full lifecycle, their inter-module
dependencies, hot reload, versioning, health, and resource limits, so that features are
additive and individually replaceable without touching the core.

## 2. Responsibilities

- Discover, load, and instantiate modules from their manifests (00 §6).
- Drive the module state machine (below) on behalf of LifecycleManager (12).
- Enforce **dependencies** between modules (DAG; resolve in topological order).
- Support **lazy loading** (a module starts only when first used or when an event
  requires it).
- Own **versioning and updates** — reject incompatible modules; support in-place
  updates where practical.
- Monitor **health** of every module and take configured action on failure.
- Enforce **resource limits** per module (CPU/RAM budgets via 10 and 11) and isolate
  misbehaving modules.
- Provide **hot reload** of modules without restarting the engine (00 §5).

## 3. Public API

```cpp
class ModuleManager final {
public:
    static ModuleManager& Instance();

    // Discovery & loading
    Result<void>   Discover(std::string_view searchPath);            // scan manifests
    Result<std::shared_ptr<IModule>> Load(std::string_view moduleId, LoadOptions opts = {});
    Result<void>   Unload(std::string_view moduleId);

    // Lifecycle (delegates to 12, but is the per-module entry point)
    Result<void>   Start(std::string_view moduleId);
    Result<void>   Pause(std::string_view moduleId);
    Result<void>   Resume(std::string_view moduleId);
    Result<void>   Suspend(std::string_view moduleId);               // resource-driven
    Result<void>   Stop(std::string_view moduleId);

    // Dependencies / lazy
    Result<void>   ResolveDependencies(std::string_view moduleId);   // topo order, cycle check
    Result<void>   SetLazyLoad(std::string_view moduleId, bool lazy);

    // Versioning / updates / hot reload
    Result<void>   CheckUpdates(std::string_view moduleId);
    Result<void>   ApplyUpdate(std::string_view moduleId);
    Result<void>   Reload(std::string_view moduleId);                // 00 §5

    // Resource limits & health
    Result<void>   SetResourceLimit(std::string_view moduleId, const ResourceBudget& budget);
    HealthReport   GetHealth(std::string_view moduleId) const noexcept;   // per module
    std::vector<ModuleInfo> Snapshot() const;                        // states + versions

    // Diagnostics
    HealthReport GetHealth() const noexcept;                         // aggregate, 00 §7
    ModuleMetrics Metrics() const noexcept;                          // 00 §4
};
```

`IModule` (interface, `interfaces/include/interfaces/IModule.hpp`): `id()`, `manifest()`,
`OnLoad()/OnUnload()/OnStart()/OnStop()/OnPause()/OnResume()`, `Reload()`,
`GetHealth()`, and an optional `OnSuspend()/OnResume()` for resource pressure.

## 4. Internal Components

| Component | Role |
|---|---|
| `ModuleManager` | Facade; per-module state coordination |
| `ModuleRegistry` | Manifest-backed registry (id → ModuleInfo) |
| `ModuleLoader` | Instantiation from manifests; version + core-version checks |
| `DependencyGraph` | Module DAG; topo ordering; cycle detection (00 §2) |
| `LazyLoader` | Event-driven on-demand start (`start-on-event` rules) |
| `UpdateEngine` | Version checks + in-place update path (drain → swap → start) |
| `ResourceGovernor` | Per-module budgets; talks to 10/11; suspends offenders |
| `HealthMonitor` | Periodic per-module `GetHealth()` (via 07 heartbeat) |

## 5. Module States

```
Installed --> Loaded --> Initialized --> Running <--> Paused
  ^            |            |             |  ^          |
  |            |            |             v  |          v
  |            +---> Failed -+         Suspended <------+ (resource pressure)
  |                  |       |            |  |
  +------------------+-------+            v  v
                    (recovery/           Stopped --> Unloaded --> Disabled
                     re-install)                       ^
                                                       + (hot reload path: drain here, re-Load)
```

| State | Meaning |
|---|---|
| `Installed` | Discovered, manifest valid, not loaded |
| `Loaded` | Code loaded, not initialized |
| `Initialized` | Init done, not started |
| `Running` | Producing/consuming events normally |
| `Paused` | User-driven or config pause (keeps state) |
| `Suspended` | Resource-driven (10/11): released caches, stopped background work, kept resident |
| `Stopped` | Stopped cleanly, still loaded |
| `Unloaded` | Code unloaded |
| `Disabled` | Installed but opted out (config/allowlist) |
| `Failed` | Load/init/start failure; recovery path available |

> **Mapping to the canonical lifecycle (12):** module states are a projection of
> LifecycleManager's machine — `Loaded` ≈ `Discovered → Loaded`, `Initialized` ≈
> `Initialized → Started`, `Running` ≈ `Started → Running`; `Stopping`/`Stopped` collapse
> to `Stopped`, and `Resumed` is represented by returning to `Running`. `Disabled` and
> `Failed` are ModuleManager policy states.

## 6. Threading Model

- **Single-threaded lifecycle transitions.** All state transitions are serialized on one
  control thread (the Kernel's boot thread + scheduler-driven transitions). No two
  transitions for the same module overlap.
- **Concurrent runtime.** Once `Running`, modules execute their own work via the
  ThreadPool (06) and communicate via the EventBus (05); the ModuleManager does not
  police that beyond resource budgets.
- `Snapshot()` and `GetHealth()` are lock-free reads of an atomic state snapshot.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.module.discovered` | `ModuleInfo` | Manifest found |
| `engine.module.requested` | `{id, requester}` | A dependency or matching event requests a lazy module (LazyLoader) |
| `engine.module.loaded` | `{id, version}` | Loaded |
| `engine.module.unloaded` | `{id, version}` | Unloaded |
| `engine.module.state_changed` | `{id, from, to, reason}` | Any state transition |
| `engine.module.health_changed` | `{id, HealthReport}` | Per-module health delta |
| `engine.module.suspended` / `resumed` | `{id, budget}` | Resource governor action |
| `engine.module.updated` | `{id, from, to}` | Update applied |

Modules also subscribe/publish their own domain events (e.g. `presentation.slide_changed`,
`media.video_state`) — those are module concerns, not ModuleManager concerns.

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.resource.pressure_high` (10) | Suspend low-priority modules per smart rules |
| `engine.kernel.state_changed` (01) | `Paused` → pause non-critical modules |
| `engine.config.hot_reload` (03) | Re-check `Disabled` allowlists, module settings |
| Domain events | `LazyLoader`: start a lazy module when a matching event is published (e.g. `ai.inference_requested`) |

## 9. Dependencies

- **Depends on:** 02, 03, 04, 05, 06, 07, 10, 11, and 09 (for plugin-backed modules).
- **Uses after init:** LifecycleManager (12) to run the canonical lifecycle state machine
  and rollback hooks.
- **Provides:** module lifecycle to the Kernel (01) shutdown path and to every module.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Module load fails (bad manifest, version mismatch) | `Error` with manifest details; module stays `Installed`/`Failed`; engine continues |
| Module crashes at runtime | Catch → mark `Failed` → per policy: restart with backoff (max N times), or disable and notify via `engine.module.health_changed` |
| Module exceeds resource budget | ResourceGovernor suspends it (10); if it violates the suspension deadline, it is stopped and reported |
| Circular module dependency | Refuse load set; `Error` listing the cycle |
| Hot reload failure (new version broken) | **Rollback to the previous version** (drain → restore → verify); engine never left without the module |
| Module blocks startup (timeout) | Init watchdog aborts → `Failed` → continue boot with the module disabled and a report |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Load + Start` of a typical module | **< 10 ms** |
| Module state transition | **< 1 ms** (control thread) |
| Per-module overhead when suspended | ~0 CPU, < 1 MB retained |
| Health aggregate (100 modules) | **< 1 ms** |

## 12. Future Extensions

- **Module marketplace / signed updates** (with 09 signing infrastructure).
- **Multi-version A/B** of a module in separate sandboxes.
- **Module-to-module private channels** (still on the EventBus, scoped topics).
- **Zero-downtime update of a module family** (update Presentation while Media keeps
  running).
- **Remote module hosting** (a module running on another machine, bridged by the
  network module).

## 13. Testing Requirements

- **Unit:** every state transition, dependency topo order, cycle rejection, lazy
  start-on-event, resource-limit enforcement.
- **Stress:** 50 modules loaded/unloaded repeatedly; sustained event traffic during a
  mid-run `Reload()` — no leaks, no deadlock.
- **Performance:** assert §11 budgets.
- **Failure:** crash injection at each state, init watchdog timeout, budget violation,
  broken update → verify rollback and isolation.
