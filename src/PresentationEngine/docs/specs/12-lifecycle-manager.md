# 12 — LifecycleManager Specification

| Field | Value |
|---|---|
| **System** | LifecycleManager |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | Complete (v1.0.0 — `core/lifecycle/`; unit-verified) |
| **Source files** | `core/include/core/lifecycle/LifecycleManager.hpp`, `core/lifecycle/LifecycleManager.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), EventBus (05), ThreadPool (06), TaskScheduler (07) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The LifecycleManager is the **canonical lifecycle engine** that every subsystem obeys.
Modules, plugins, services, drivers, and core systems all move through the *same* state
machine (below) with the same hooks, rollback, recovery, and health rules. Instead of
each system inventing its own start/stop semantics, one manager owns the transitions,
dependencies, and failure handling for everything that has a lifecycle.

## 2. Responsibilities

- Own the **13-state lifecycle** (Installed → … → Destroyed) for every registered
  entity (core systems, modules, plugins, services, drivers).
- Enforce **dependencies** during transitions (an entity only starts after its
  dependencies are in the required state; only stops before them).
- Provide **hooks** (`Before*` / `On*` / `After*`) for entities to run work around
  transitions.
- Publish **events** for every transition (drives the EventBus, 05).
- Perform **rollback** — if `OnStart` fails, unwind through the reverse transition
  hooks into a consistent prior state.
- Provide **recovery** policies (retry, backoff, disable, restart-after-crash).
- Track **health** per entity during its lifecycle.
- Handle **version migration** (an entity upgrading must pass through the lifecycle with
  migration hooks before `Started`).

## 3. Public API

```cpp
class LifecycleManager final {
public:
    static LifecycleManager& Instance();

    // Registration
    template <class Entity>
    Result<void> Register(Entity& entity, const LifecycleManifest& manifest);   // 00 §6

    // Transitions (the ONLY way to move an entity between states)
    Result<void> Transition(EntityId id, LifecycleState target, TransitionOptions opts = {});
    Result<void> Start(EntityId id);      // convenience → Transition(id, Started)
    Result<void> Stop(EntityId id);       // convenience → Transition(id, Stopped)
    Result<void> Pause(EntityId id);
    Result<void> Resume(EntityId id);
    Result<void> Suspend(EntityId id);    // resource-driven, reversible
    Result<void> Destroy(EntityId id);

    // Hooks
    Result<void> AddHook(EntityId id, LifecycleHook hook);      // Before/On/After per transition

    // Dependency-aware transitions
    Result<void> TransitionDependent(EntityId id, LifecycleState target);   // starts deps first, stops deps last

    // Recovery / migration
    Result<void> SetRecoveryPolicy(EntityId id, RecoveryPolicy policy);     // RetryN | Disable | RestartOnCrash
    Result<void> RunMigrations(EntityId id, Version from, Version to);

    // Introspection / diagnostics
    LifecycleState State(EntityId id) const noexcept;
    std::vector<EntityLifecycle> Snapshot() const;
    HealthReport GetHealth() const noexcept;                  // 00 §7
    LifecycleMetrics Metrics() const noexcept;                // 00 §4
};
```

`LifecycleHook`: `{transition, phase: Before|On|After, callback}`.
`TransitionOptions`: `{reason, timeout, allowRollback, skipVersionCheck}`.

## 4. Internal Components

| Component | Role |
|---|---|
| `LifecycleManager` | Facade; per-entity transition orchestration |
| `LifecycleGraph` | Entity DAG (dependencies) for transition ordering |
| `StateTable` | Current state per entity; legal-transition matrix |
| `HookRegistry` | Per-entity hooks, ordered by phase and priority |
| `TransitionRunner` | Executes a transition: run hooks → change state → publish event; enforces timeouts |
| `RollbackEngine` | Reverse-hook unwinding on failure |
| `RecoveryPolicy` | Retry/disable/restart strategies with backoff |
| `MigrationRunner` | Version migration hooks between `Initialized` and `Started` |

## 5. State Machine (the canonical lifecycle)

```
Installed
   │  (discovered)
   ▼
Discovered
   │  (loaded)
   ▼
Loaded
   │  (initialized)
   ▼
Initialized
   │  (started)
   ▼
Started ──────────────► Running  ◄──────────────┐
   │                       │  ▲                  │
   │                       │  │ (resumed)        │
   │                       ▼  │                  │
   │                    Paused ──► Suspended ─────┘
   │                       │        (resource-driven)
   │                       │
   │                       ▼
   │                   Stopping
   │                       │
   ▼                       ▼
Stopped
   │  (unloaded)
   ▼
Unloaded
   │  (destroyed)
   ▼
Destroyed
```

Every state transition is legal only if the entity's dependencies are in a compatible
state (verified by `LifecycleGraph`).

> **Note on the state machine revision (architecture brief).** The brief specifies a
> 12-state variant — `Discovered → Created → Registered → Initialized → Started →
> Running → Paused → Suspended → Resumed → Stopping → Stopped → Destroyed` — which drops
> `Installed`/`Loaded`/`Unloaded` and adds `Created`/`Registered`. v1.0 implements the
> 13-state machine above (it is the ratified, tested contract). Migrating to the 12-state
> variant is a tracked decision (`docs/architecture/Conformance.md` §12.1); when taken,
> update this section, `LifecycleState` in `core/include/core/lifecycle/LifecycleManager.hpp`, the
> `ModuleManager::ToLifecycle` mapping, and the tests together.

## 6. Threading Model

- **Single-threaded transition execution.** All transitions run on one control path (the
  boot thread, or scheduler-queued control tasks). This makes rollback and dependency
  ordering deterministic and deadlock-free.
- **Entity runtime is concurrent** — once `Running`, entities execute on the ThreadPool
  (06) and communicate via the EventBus (05); the LifecycleManager is not involved until
  the next transition.
- `State()` / `Snapshot()` are lock-free reads of an atomic state table.
- Long-running `On*` hooks must be cooperative: hooks that exceed the transition timeout
  are aborted via the recovery policy (watchdog).

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.lifecycle.registered` | `{id, manifest}` | Entity registered |
| `engine.lifecycle.state_changed` | `{id, from, to, reason}` | Every legal transition |
| `engine.lifecycle.rollback` | `{id, from, to, cause}` | Rollback executed |
| `engine.lifecycle.recovering` | `{id, attempt, policy}` | Recovery attempt started |
| `engine.lifecycle.migrated` | `{id, from, to}` | Version migration ran |
| `engine.lifecycle.transition_failed` | `{id, target, error}` | Transition failed (pre-rollback) |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.kernel.state_changed` (01) | Cascade: Kernel `ShuttingDown` → stop entities in reverse dependency order |
| `engine.resource.pressure_high` (10) | Suspend low-priority entities (reversible); `Critical` → stop background entities |
| `engine.config.hot_reload` (03) | Trigger `Reload()` transitions for affected entities (00 §5) |
| `engine.module.updated` / `engine.plugin.updated` (08, 09) | Run `RunMigrations` + restart through the lifecycle |

## 9. Dependencies

- **Depends on:** 02, 03, 04, 05, 06, 07.
- **Uses after init:** ModuleManager (08), PluginManager (09), ServiceManager (04) — all
  delegate their state machines to this manager's canonical machine.
- **Provides:** the lifecycle contract that every system in the engine follows.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| `OnStart` (or any hook) fails | **Rollback** via `RollbackEngine` (reverse hooks), entity returns to the prior consistent state, `transition_failed` + `rollback` events |
| Hook hangs past timeout | Watchdog aborts the transition, applies recovery policy, entity marked `Failed`-equivalent state (remains `Stopped` with error report) |
| Dependency not in required state | Transition refused with `Error` naming the dependency and its state |
| Recovery policy `RetryN` exhausted | Entity disabled + `recovering` event with the final error; engine continues |
| Migration fails mid-upgrade | Rollback to the previous version (migration hooks are transactional); entity stays on the old version |
| Entity crashes while `Running` | Recovery policy `RestartOnCrash` → re-enter at `Initialized` (not `Installed`), preserving state where the entity allows |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `State()` read | **< 100 ns** |
| Transition overhead (hooks excluded) | **< 50 µs** |
| Dependency check for a transition | **< 10 µs** (cached graph) |
| Rollback of a failed start | **< 5 ms** (typical entity) |
| Lifecycle events during a full boot | < 1% of boot time (01 budget: 200 ms) |

## 12. Future Extensions

- **Lifecycle visualization UI** (live state machine inspector) from `Snapshot()`.
- **Paused-to-hibernate** for an entire entity subtree (all deps of a module together).
- **Transactional multi-entity transitions** ("start these 5 entities or none") with
  coordinated rollback.
- **Lifecycle scripting hooks** (define hooks in a script/plugin) — same hook points.
- **Deadline-aware transitions** ("finish stopping within this render frame").

## 13. Testing Requirements

- **Unit:** every transition in the matrix, dependency gating, hook ordering, rollback
  correctness, migration transactions.
- **Stress:** 1 k entities churn through start/stop/suspend cycles concurrently with
  event traffic — no state corruption, no deadlock.
- **Performance:** assert §11 budgets.
- **Failure:** hook throws, hook hangs, dependency missing, migration aborts mid-way,
  crash-while-running — verify rollback, recovery policies, and backoff.
