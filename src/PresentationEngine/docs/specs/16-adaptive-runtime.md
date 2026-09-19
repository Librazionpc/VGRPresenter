# 16. Adaptive Runtime System (ARS)

> Phase 5. The engine stops being a *fixed* engine and becomes a *living* one:
> it continuously observes hardware, resources, performance, user preferences
> and the active workload, and configures itself. The engine never assumes
> high-end hardware — it always asks *"what do I currently have, and what is
> the best way to use it?"*

## Purpose

Make every subsystem answer resource/quality questions through one authority so
there are **zero hardcoded decisions** (`threads = 8`, `if (ram > 8)`) anywhere
in the codebase. Instead: `Runtime->GetRecommendedThreadCount()`,
`Runtime->GetTextureBudget()`, `Runtime->ShouldUseHardwareDecoder()`.

## Responsibilities

- Detect hardware capabilities (never device names — *capabilities*).
- Continuously measure CPU / GPU / RAM / VRAM / disk / frame time / cache hit
  ratio / import speed / decode latency.
- Compute per-subsystem resource budgets (renderer, AI, cache, search, video).
- Scale quality automatically under performance / memory / thermal / battery
  pressure (dynamic quality scaling).
- Decide thread counts, worker counts, GPU usage, cache sizes, preload and
  batch sizes, compression, streaming quality, AI model, thumbnail resolution.
- Enable / disable / suspend / resume / throttle features from module resource
  contracts.
- Learn usage patterns (frequency, weekday) and preload what the user actually
  uses; boot only the modules a user needs (startup optimizer).
- Expose three configuration layers: Automatic (default), Assisted
  (preferences like "prefer performance"), Expert (everything exposed).
- Publish recommendations and adaptation events; never force anything — the
  runtime *recommends*, the UI/user decides.
- Provide a runtime dashboard snapshot for users and debugging.

## Public API (facade: `bps::adaptive::AdaptiveRuntime`)

| Method | Meaning |
|---|---|
| `Initialize/Start/Stop/Shutdown/Reload/Reset` | IService lifecycle |
| `GetHealth/MetricsSnapshot` | diagnostics |
| `Hardware()->Profiler()/Capabilities()` | hardware access |
| `Profiler()->Snapshot()` | latest measured metrics |
| `Optimizer()->GetRecommendedThreadCount()` | thread recommendation |
| `Optimizer()->GetTextureBudget()` | texture memory budget |
| `Optimizer()->ShouldUseHardwareDecoder()` | GPU/CPU decode decision |
| `Optimizer()->GetCacheBytes()` | total cache budget |
| `Optimizer()->GetBatchSize()` | import/thumbnails batch |
| `Optimizer()->GetQualityLevel()` | active quality level |
| `Budgets()->Get(subsystem)` | per-subsystem budget |
| `Features()->Register/SetEnabled/SetSuspended/IsEnabled` | feature control |
| `Features()->RegisterFeatureContract(module, ModuleContract)` | module contract |
| `Power()->State()` / `Thermal()->Temperature()` | power/thermal state |
| `Memory()->OnPressure(level)` | memory pressure reaction |
| `Learning()->RecordUsage(kind, id)` / `ShouldPreload(kind, id)` | usage learning |
| `Startup()->RecommendedModules()` | required-modules set |
| `Recommend()->List()` | recommendation list |
| `SetMode(UserMode)` / `SetConfigLayer(Layer)` / `SetPreference(Preference)` | control |
| `Dashboard()` | runtime dashboard snapshot |

## Internal components

- `HardwareProfiler` — gathers `SystemInfo`, `Snapshot`, `MonitorInfo`,
  `PowerInfo` from the PAL into one `HardwareInfo` view.
- `CapabilityDetector` — `Supports(HardwareVideoDecode | Vulkan | AVX2 | …)`.
- `PerformanceProfiler` — rolling-window metrics with `Record*` + `Snapshot()`.
- `ResourceBudgetManager` — per-subsystem budgets; rescales on profile/pressure.
- `RuntimeOptimizer` — the brain; all `Get*` recommendations derive from
  hardware + budget + profile + pressure + preference.
- `FeatureManager` — registry of `FeatureDef` (each carries a `ModuleContract`).
- `QualityProfile` — level ↔ settings mapping (Ultra … Minimal, Custom).
- `UserModeManager` / `ConfigLayer` — mode + control-layer state.
- `PowerManager` / `ThermalManager` / `MemoryPressureManager` — pressure
  reactions (throttle background work, shrink caches, never the live
  presentation).
- `GPURuntime` — GPU vs CPU decisions for text/decode/blur/scale/compose.
- `UsageLearningEngine` — frequency tables (per weekday) persisted via the
  DatabaseManager.
- `SmartCacheManager` — preload policy from learned patterns.
- `StartupOptimizer` — required-module set for a given user/mode.
- `RecommendationEngine` — non-forcing recommendations.
- `AdaptiveRuntime` — facade, EventBus wiring, adaptation heartbeat.

## State machine

`Idle → Profiling → Analyzing → Optimizing → Adapted → (pressure) → Reacting →
Adapted`. The heartbeat runs every few seconds (not aggressively), plus
event-driven reactions on `ResourcePressureHigh`, `PowerChanged`, `BatteryLow`,
`SystemSleep/Wake`, `ConfigHotReload`.

## Threading model

Single adaptation thread owned by the Core TaskScheduler (heartbeat). All state
is mutex-guarded; the facade is safe for concurrent readers (modules call
`Get*` from any thread).

## Events published

`adaptive.quality_changed`, `adaptive.module_suspended/resumed`,
`adaptive.memory_budget_changed`, `adaptive.optimization_applied`,
`adaptive.feature_enabled/disabled`, `adaptive.recommendation`,
`adaptive.dashboard_updated`.

## Events consumed

`resource.pressure_high`, `power.power_changed`, `platform.battery_low`,
`system.sleep`, `system.wake`, `config.hot_reload`, `shutdown_started`.

## Dependencies

PAL (`IPlatform` — Info/Snapshot/Monitor/Power), `ConfigurationManager`,
`EventBus`, `TaskScheduler`, `ThreadPool` (dynamic `Resize`),
`ResourceManager` (pressure/mode), `DatabaseManager` (learning persistence),
`ModuleManager` (startup/lazy decisions), `ContentManager` (cache budget +
pressure shrink), `ServiceManager`.

## Failure modes

- PAL backend missing → degrade to safe defaults (single worker, minimal
  budgets, CPU-only), never crash.
- Pressure events missing → fall back to periodic profiling.
- Learning DB corrupt → start empty.
- Budget math must be monotonic-safe (never below floor values).

## Performance goals

- Profiling overhead < 0.1% CPU.
- Adaptation reaction < 1 s after a pressure event.
- Startup optimizer saves real boot time by skipping unused modules.
- No allocation spikes during budget recomputation.

## Future extensions

- Real GPU VRAM/thermal telemetry on Windows/macOS (PAL `Snapshot` fields
  already reserved).
- AI-driven workload prediction.
- Per-workspace profiles and cloud-synced preferences.
- Adaptive rendering pipeline knobs consumed by the Phase 6 renderer.
