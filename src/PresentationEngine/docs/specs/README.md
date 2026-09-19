# Core Specification Set — v1.0

This directory is the **contract** for the Believers Presentation Software engine core.
It exists to answer *"what must this system do, and how must it behave?"* **before** any
C++ is written. The engine is designed as a long-lived, engine-grade platform (in the
spirit of Unreal Engine / Godot / Chromium), not merely a church presentation app —
presentation features are modules that plug into this core.

> **Reading order:** start with [`00-cross-system-rules.md`](./00-cross-system-rules.md),
> then read the system specs in numeric order. System specs cross-reference each other by
> number (e.g. "depends on `01 Kernel`").

## Spec inventory

| # | System | Source files | Key capabilities |
|---|--------|--------------|------------------|
| 01 | [Kernel](./01-kernel.md) | `core/kernel/` | Boot/shutdown ordering, states, panic mode, crash recovery, uptime, global clock |
| 02 | [Logger](./02-logger.md) | `core/logging/` | 6 levels, async, 6 sinks, rotation, filtering, JSON/compressed output |
| 03 | [ConfigurationManager](./03-configuration-manager.md) | `core/config/` | 6 scopes, JSON/TOML/YAML, validation, migration, hot reload, encryption |
| 04 | [ServiceManager](./04-service-manager.md) | `core/services/` | DI container: singleton/scoped/transient/lazy lifetimes |
| 05 | [EventBus](./05-event-bus.md) | `core/events/` | Pub/sub, request/response, priority, sticky, history, replay |
| 06 | [ThreadPool](./06-thread-pool.md) | `core/threading/` | Dynamic workers, priority queues, affinity, cancellation, 3 task classes |
| 07 | [TaskScheduler](./07-task-scheduler.md) | `core/task_scheduler/` | One-shot/recurring/cron/animation/heartbeat, µs precision |
| 08 | [ModuleManager](./08-module-manager.md) | `core/modules/` | 10 module states, hot reload, versioning, resource limits |
| 09 | [PluginManager](./09-plugin-manager.md) | `core/plugins/` | DLL/SO/DYLIB loading, signatures, sandboxing, isolation |
| 10 | [ResourceManager](./10-resource-manager.md) | `core/resources/` | RAM/VRAM/CPU/GPU/Disk/Network telemetry, pressure, 5 modes, smart rules |
| 11 | [MemoryManager](./11-memory-manager.md) | `core/memory/` | Pools, arenas, stack/heap allocators, tags, leak tracking |
| 12 | [LifecycleManager](./12-lifecycle-manager.md) | `core/lifecycle/` | 13-state lifecycle, hooks, rollback, version migration |

Every spec answers the same eleven questions: **Purpose, Responsibilities, Public API,
Internal Components, State Machine, Threading Model, Events Published, Events Consumed,
Dependencies, Failure Modes, Performance Goals** — plus **Future Extensions** and a
**Testing Requirements** section. Consistency between answers is mandatory; the
[Cross-System Rules](./00-cross-system-rules.md) document defines the rules all specs
inherit (Result\<T\>, interface-only dependencies, metrics, hot reload, versioning, etc.).

## Immutable initialization order

This order is a hard engine invariant. Nothing in the engine initializes before the
Kernel, and no system may be reordered.

```
Kernel
├── Logger
├── ConfigurationManager
├── ServiceManager
├── EventBus
├── ThreadPool
├── TaskScheduler
├── ResourceManager
├── MemoryManager
├── PluginManager
├── ModuleManager
└── LifecycleManager
```

Shutdown is the exact reverse (see `01 Kernel`, §Shutdown sequence).

> **Ordering note:** the original boot-sequence sketch listed "Initialize Memory" right
> after engine start. The binding invariant is the order diagram above (MemoryManager
> after ResourceManager), so ResourceManager reads memory telemetry from OS probes and
> cooperates with MemoryManager only after both are initialized (see `00 §9`).

## Status legend

Every spec carries two status fields in its header table:

- **Spec status** — `Draft` / `Approved` / `Implemented` / `Revised`. A system may be
  implemented only once its spec is `Approved`.
- **Implementation status** — tracks the actual code: `Scaffold` (empty placeholder
  files exist), `Partial`, `Complete`.

**v1.0.0 status: all 12 systems are `Complete`** — implemented under `core/<system>/`,
verified by the unit suite (`tests/unit/main.cpp`, 195 checks passing) and exercised by
the CLI demo (`apps/cli/`). The full audit of the architecture brief's expectations
against this implementation lives in
[`docs/architecture/Conformance.md`](../architecture/Conformance.md).

## Architecture

The engine is layered — Frontends → Communication → Core → Managers → Feature Modules →
Platform — and the core **never knows about Presentation/Bible/Songs/AI** (those are
feature modules). See [`docs/architecture/SystemArchitecture.md`](../architecture/SystemArchitecture.md)
for the layer boundaries and the engine-wide manager contract, and
[`docs/architecture/Conformance.md`](../architecture/Conformance.md) for the
expectation-vs-implementation audit.

## How these specs govern code

1. **Interfaces first.** Each spec's *Public API* section is the source of truth for the
   interface headers (e.g. `interfaces/include/interfaces/IService.hpp`, `interfaces/include/interfaces/IModule.hpp`) and the
   concrete headers under `core/<system>/`.
2. **Tests prove the spec.** Each spec ends with mandatory test categories (unit, stress,
   performance, failure). A spec is not `Implemented` until those tests exist and pass.
3. **Architecture docs derive from specs.** `docs/architecture/*.md` currently exist as
   empty placeholders; they should be authored *from* these specs, not the other way
   around. `docs/api/` and `docs/design/` likewise.

## Change process

Any change to a spec requires:

1. Bump the **Spec version** (semver) of the affected document(s).
2. Update every cross-referencing spec — dependencies, events, and initialization order
   are global invariants.
3. Update this README's inventory table if capabilities change.
