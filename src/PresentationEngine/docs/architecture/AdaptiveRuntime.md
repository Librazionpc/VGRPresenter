# Adaptive Runtime System (ARS)

> Phase 5. One authority for every resource/quality decision. Modules stop
> making isolated optimization decisions and ask the runtime instead.

## Layer position

```
Engine subsystems (renderer, AI, search, media, import, cache, streaming, …)
                  │  ask / report
                  ▼
          ┌─────────────────────────────┐
          │      AdaptiveRuntime        │   facade (IService) — the ONLY entry point
          └─────────────────────────────┘
             │            │            │
   Hardware Analyzer  Resource Analyzer  Preference Engine
   (Profiler,         (Profiler,         (UserMode, ConfigLayer,
    Capabilities)      Budgets, Pressure)  Preferences, Learning)
             │            │            │
             └────────────┼────────────┘
                          ▼
                   Runtime Optimizer     ← the brain: all Get* decisions
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   Rendering            AI / Search        Media / Cache / Streaming
```

The PAL (`IPlatform`, `Snapshot`, `MonitorInfo`, `PowerInfo`) is the only place
that touches the OS. The Adaptive Runtime is the only place that turns that raw
data into *decisions*.

## Decision flow

1. **Profile** — heartbeat collects `PerformanceProfiler` metrics + PAL
   `Snapshot` + monitor/power state (every few seconds, < 0.1 % CPU).
2. **Budget** — `ResourceBudgetManager` maps total RAM + active quality profile
   + pressure to per-subsystem budgets (renderer/AI/cache/search/video) with
   hard floors.
3. **Optimize** — `RuntimeOptimizer` answers the engine's questions:
   `GetRecommendedThreadCount()` (also applied live via `ThreadPool::Resize`),
   `GetTextureBudget()`, `ShouldUseHardwareDecoder()`, `GetCacheBytes()`,
   `GetBatchSize()`, `GetStreamingQuality()`, `GetAIModel()`,
   `GetThumbnailResolution()`, `GetQualityLevel()`.
4. **React** — pressure events (`ResourcePressureHigh`, `PowerChanged`,
   `BatteryLow`, `SystemSleep/Wake`) trigger immediate reactions: shrink
   caches (CAMS `ShrinkCache`), throttle background work, pause non-critical
   features — never the live presentation.
5. **Learn** — `UsageLearningEngine` records what the user actually opens
   (per kind + weekday) and persists it; `SmartCacheManager` turns the pattern
   into preload hints; `StartupOptimizer` turns it into a required-module set.
6. **Recommend** — `RecommendationEngine` surfaces suggestions
   ("Your PC supports GPU rendering — enable it?") without forcing anything.

## Module contracts

Every module declares `ModuleContract` (min/recommended/max RAM, GPU
optional/required, CPU threads, disk, estimated startup, lazy-loadable,
suspendable, capabilities). `FeatureManager` registers them and the runtime
uses them for budgets, startup and suspension decisions.

## Control layers

- **Automatic (default)** — the engine decides everything.
- **Assisted** — the user expresses *preferences* ("prefer performance",
  "prefer battery life", "prefer low memory"); the engine still decides detail.
- **Expert** — every knob is exposed and configurable.

## Zero-hardcoded-decisions rule

`if (gpu)`, `if (ram > 8)`, `threads = 8` must not appear outside
`modules/adaptive/`. Subsystems call
`AdaptiveRuntime::Instance().Optimizer()->Get…()` instead.

## Event wiring

Consumes: `resource.pressure_high`, `power.power_changed`,
`platform.battery_low`, `system.sleep`, `system.wake`, `config.hot_reload`,
`shutdown_started`.
Publishes: `adaptive.quality_changed`, `adaptive.module_suspended/resumed`,
`adaptive.memory_budget_changed`, `adaptive.optimization_applied`,
`adaptive.feature_enabled/disabled`, `adaptive.recommendation`,
`adaptive.dashboard_updated`.

See `docs/specs/16-adaptive-runtime.md` for the full spec, and
`docs/architecture/Conformance.md` §18 for the Definition of Done audit.
