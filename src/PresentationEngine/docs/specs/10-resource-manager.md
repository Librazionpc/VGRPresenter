# 10 — ResourceManager Specification

| Field | Value |
|---|---|
| **System** | ResourceManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/resources/ResourceManager.hpp`, `core/resources/ResourceManager.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), EventBus (05), ThreadPool (06), TaskScheduler (07), MemoryManager (11) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The ResourceManager is the engine's **awareness and policy layer over every resource** —
RAM, VRAM, CPU, GPU, disk, network, battery, displays/monitors, idle modules, thread and
GPU usage, and pressure signals. It is probably the engine's biggest innovation: instead
of each module guessing, one system knows the machine's state, declares pressure, and
applies a **resource mode** with smart rules so the engine degrades gracefully on a weak
laptop and fully exploits a powerful staging PC.

## 2. Responsibilities

- **Track** live telemetry for RAM, VRAM, CPU, GPU, disk, network, battery, displays,
  monitors, per-module CPU/RAM, thread usage, and GPU usage.
- **Measure pressure** (memory, disk, network) and publish pressure levels.
- Own the **resource modes** (`Balanced`, `Performance`, `Strict`, `Battery`,
  `Developer`) and apply their policies engine-wide.
- Expose **Allocate / Release / Suspend / Resume / Compress / Prefetch / Throttle /
  Unload** primitives for modules to cooperate.
- Enforce **smart rules**: suspend unused modules, unload caches, reduce threads, lower
  polling frequency, pause background indexing.
- Provide budgets to ModuleManager (08) and PluginManager (09) for per-entity limits.

## 3. Public API

```cpp
class ResourceManager final {
public:
    static ResourceManager& Instance();

    // Telemetry
    MachineTelemetry Telemetry() const noexcept;              // snapshot: ram/vram/cpu/gpu/disk/net/battery
    std::vector<DisplayInfo> Displays() const noexcept;       // monitors, refresh, resolution
    PerModuleUsage Usage(std::string_view moduleId) const;    // cpu/ram/threads/gpu

    // Pressure
    PressureLevel MemoryPressure() const noexcept;
    PressureLevel DiskPressure() const noexcept;
    PressureLevel NetworkPressure() const noexcept;

    // Modes
    Result<void> SetMode(ResourceMode mode);
    ResourceMode Mode() const noexcept;
    Result<void> RegisterModePolicy(ResourceMode mode, const ModePolicy& policy);

    // Cooperative primitives
    Result<void> Allocate(ResourceClaim claim);               // reserve budget (fails if over)
    Result<void> Release(ResourceClaim claim);
    Result<void> Suspend(std::string_view ownerId);
    Result<void> Resume(std::string_view ownerId);
    Result<void> Compress(std::string_view ownerId, bool aggressive);
    Result<void> Prefetch(std::string_view ownerId, const std::vector<AssetKey>& assets);
    Result<void> Throttle(std::string_view ownerId, const ThrottleSpec& spec);
    Result<void> Unload(std::string_view ownerId);

    // Diagnostics
    HealthReport GetHealth() const noexcept;                  // 00 §7
    ResourceMetrics Metrics() const noexcept;                 // 00 §4
};
```

`PressureLevel`: `None < Low < Medium < High < Critical`.

## 4. Internal Components

| Component | Role |
|---|---|
| `ResourceManager` | Facade; mode policy application |
| `TelemetryCollector` | Periodic sampler (via 07) for RAM/VRAM/CPU/GPU/disk/network/battery |
| `PressureMonitor` | Thresholds → pressure levels; publishes transitions |
| `ModeEngine` | Applies the active mode's policy (budgets, throttles, allowed work) |
| `BudgetTable` | Per-owner claims and limits; arbitration on `Allocate` |
| `SmartRuleEngine` | The rules: suspend idle modules, unload caches, reduce threads, lower polling, pause indexing |
| `DisplayRegistry` | Display/monitor enumeration + hot-plug updates |
| `GpuProbe` | VRAM + GPU utilization (platform backends) |

## 5. State Machine

```
Uninitialized --> Monitoring --> ApplyingPolicy --> Monitoring
                     |    ^            |
                     +----+            +--> (mode/priority change)
                     |
                     +--> SuspendedByOS (system sleep) --> Monitoring
```

The manager is mostly *reactive*: it continuously monitors and applies the current
mode's policy. Its states represent the policy loop, not long-lived phases.

## 6. Threading Model

- **Single sampler thread** (scheduler-owned task, 07) collects telemetry at a cadence
  that varies by mode (e.g. 1 s in `Balanced`, 5 s in `Battery`, 100 ms in `Strict`).
- **Telemetry reads are lock-free** (atomic snapshots published by the sampler).
- **Mode changes and budget arbitration** are serialized; `Allocate`/`Release` are quick
  atomic bookkeeping on the hot path.
- No sampling work ever blocks the caller; `Telemetry()` returns the last snapshot.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.resource.pressure_changed` | `{resource, from, to}` | Any pressure level crosses a threshold |
| `engine.resource.pressure_high` | `{resource, level}` | A resource enters `High`/`Critical` pressure — convenience topic consumed by 02, 03, 05, 08, 09, 11, 12 |
| `engine.resource.mode_changed` | `{mode, reason}` | Mode switched (manual or automatic) |
| `engine.resource.budget_violated` | `{ownerId, budget, actual}` | Owner exceeded a claim |
| `engine.resource.display_changed` | `DisplayInfo[]` | Display hot-plug / resolution change |
| `engine.resource.battery_low` | `{percent, modeSwitch}` | Battery threshold crossed |
| `engine.resource.telemetry` | `MachineTelemetry` | Periodic snapshot (throttled, e.g. 1 Hz) |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.module.state_changed` (08) | Track idle modules → apply "suspend unused" smart rule |
| `engine.plugin.state_changed` (09) | Unload non-essential plugins on high pressure |
| `engine.threadpool.cpu_pressure` (06) | Reduce thread pool workers per mode |
| `engine.display.output_changed` (Display module) | Re-enumerate displays |
| `engine.config.hot_reload` (03) | Re-apply thresholds and mode defaults |

## 9. Dependencies

- **Depends on:** 02, 03, 04, 05, 06, 07. Memory telemetry is read from **OS/platform
  probes**, not from MemoryManager — MemoryManager (11) initializes *after* this system
  (00 §9), so it cannot be an init-time dependency.
- **Uses after init:** MemoryManager (11) to cross-check attributed memory and trigger
  `Cleanup()` on owners; ModuleManager (08) and PluginManager (09) to suspend/unload
  owners; ThreadPool (06) to resize; TaskScheduler (07) to schedule sampling.
- **Provides:** budgets and pressure signals to every system (each system's spec's
  "Events Consumed" table references this spec's pressure events).

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Telemetry probe fails (platform API missing) | Degrade gracefully: estimate from other sources, mark metric `unknown`, never fail the engine |
| Budget arbitration conflicts | Highest-priority owner wins; conflicting owner gets `Error` + `budget_violated`; no deadlock (single serialized arbiter) |
| Pressure spikes to `Critical` | Automatic mode downgrade (e.g. → `Strict`); smart rules act immediately; `pressure_changed` published |
| GPU/VRAM probe hang | Timeout the probe; report VRAM `unknown`; continue with CPU/disk telemetry |
| OS sleep / resume | On resume: re-run full telemetry + re-evaluate mode (battery may have changed) |
| Owner ignores suspend request | Escalate to ModuleManager (08) watchdog → stop the module; ResourceManager never force-kills threads itself |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Telemetry()` snapshot read | **< 1 µs** |
| Sampler overhead | **< 1% CPU** across all probes |
| Pressure transition latency | **< 1 s** from crossing to `pressure_changed` |
| Mode switch end-to-end | **< 10 ms** (rule application) |
| Memory overhead | < 1 MB for tables/snapshots |

## 12. Future Extensions

- **Predictive pressure** (trend-based pre-warning before thresholds hit).
- **Heat/power-aware modes** on laptops (integrated with battery and CPU governor).
- **Per-output GPU budgets** for multi-projector presentation rigs.
- **Energy budget allocation** — "this show must run 6 h on battery": the mode engine
  plans power spend for modules.
- **Cloud/remote resource pooling** — offload GPU work to a render server (network
  module) when local pressure is `Critical`.

## 13. Testing Requirements

- **Unit:** thresholds → pressure mapping, mode policies, budget arbitration, smart
  rule triggers, display hot-plug.
- **Stress:** continuous mode flapping + owner churn (1 k owners) — stable, no leaks.
- **Performance:** assert §11 budgets (sampler CPU, snapshot latency).
- **Failure:** probe failures, owner ignoring suspend, pressure `Critical` storm — verify
  graceful degradation and escalation.
