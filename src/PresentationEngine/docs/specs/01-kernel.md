# 01 — Kernel Specification

| Field | Value |
|---|---|
| **System** | Kernel |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/kernel/Kernel.hpp`, `core/kernel/Kernel.cpp` |
| **Depends on** | Nothing at boot (it *creates* everything else in order) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The Kernel is the **operating system of the engine**. It is the first thing that runs
and the last thing that stops. It owns the boot/shutdown sequence, the global engine
state machine, crash recovery, panic mode, health checks, the engine version/uptime,
the global clock, and the global context that binds every subsystem together.

**Nothing in the engine initializes before the Kernel. Everything goes through it.**

## 2. Responsibilities

- **Boot sequencing** — instantiate and initialize all core systems in the fixed order
  (`docs/specs/README.md`).
- **Shutdown sequencing** — tear everything down in exact reverse order.
- **Dependency graph** — hold the directed acyclic graph of systems and refuse to start
  with missing, duplicated, or cyclic dependencies; also verify initialization order at
  runtime so a system that uses an uninitialized dependency fails fast.
- **Global state machine** — own the single `KernelState` for the process.
- **Crash recovery** — detect an unclean shutdown/startup and run recovery on next boot.
- **Panic mode** — a last-resort, fail-safe path that flushes logs, releases memory, and
  exits deterministically with a fatal diagnostic.
- **Health checks** — aggregate `GetHealth()` from every manager (00 §7) and expose a
  global health report; drive `Paused` transitions when the system is degraded.
- **Global identity** — engine version, engine uptime, global monotonic clock, global
  context (startup args, platform info, environment handles).

## 3. Public API

```cpp
class Kernel final {                       // Singleton: Kernel::Instance()
public:
    static Kernel& Instance();

    Result<void>   Boot(const BootOptions& options);        // idempotent: returns ok if already running
    Result<void>   Shutdown();                              // graceful, reverse order
    Result<void>   Pause();                                 // enter Paused (quiesce systems)
    Result<void>   Resume();

    KernelState    State() const noexcept;                  // thread-safe read
    Version        EngineVersion() const noexcept;
    std::chrono::microseconds Uptime() const noexcept;      // since Boot()
    const GlobalClock& Clock() const noexcept;              // monotonic + wall clock
    GlobalContext& Context() noexcept;                      // args, platform, env

    Result<void>   Panic(const Error& reason);              // deterministic fail-safe exit
    HealthReport   GetHealth() const noexcept;              // 00 §7

    // Dependency graph introspection (diagnostics/tests)
    const std::vector<SystemNode>& DependencyGraph() const noexcept;
};
```

`BootOptions` carries: configuration file paths, log level overrides, thread pool
sizing, resource mode (10), memory limits (11), plugin/module allowlists, and the
`--safe-mode` / `--headless` flags.

## 4. Internal Components

| Component | Role |
|---|---|
| `Kernel` | Facade singleton; owns sequencing and state |
| `BootSequence` | Executes the ordered init/shutdown lists; supports dependency verification |
| `DependencyGraph` | DAG of systems + version manifests; rejects cycles/missing deps |
| `StateMachine` | Enforces legal `KernelState` transitions |
| `HealthAggregator` | Polls `GetHealth()` of all systems; produces global report |
| `CrashRecovery` | Detects unclean state, runs recovery before normal boot |
| `PanicHandler` | Deterministic fail-safe: flush logs → release memory → exit |
| `GlobalClock` | Monotonic clock + wall clock + uptime (single source of time) |
| `GlobalContext` | Startup args, platform info, environment, app paths |

## 5. State Machine

```
                    +---------------------------------------------+
                    v                                             |
   Booting --> Starting --> Running --> Paused <---> Resumed ...  |  (Paused is a
      |          |           |    ^        |                      |   sub-state of
      |          |           |    |        +----------------------+   Running)
      |          v           v    |                                 |
      +-----> CrashRecovery ---+ (detected unclean state)           |
                    |                                               |
                    v                                               |
              ShuttingDown --> Stopped --> (process exit)           |
                    |                                               |
                    +--> (failure) --> PanicMode --> exit           |
                                                                    |
   CrashRecovery on next boot -------------------------------------+
```

| State | Meaning | Entry conditions |
|---|---|---|
| `Booting` | Pre-init; systems being constructed | `Boot()` called |
| `Starting` | Systems initializing in fixed order | Boot sequence begins |
| `Running` | All systems ready; normal operation | Boot completes |
| `Paused` | Degraded/throttled; systems quiesced | Health below threshold, or explicit `Pause()` |
| `ShuttingDown` | Reverse-order teardown in progress | `Shutdown()` called |
| `Stopped` | Clean exit complete | Teardown finishes |
| `CrashRecovery` | Recovery pass before normal boot | Previous run ended uncleanly |

Legal transitions are enforced by `StateMachine`; illegal transitions return
`ErrorCode::Kernel_InvalidStateTransition`.

## 6. Threading Model

- **Single-threaded boot and shutdown.** All initialization/teardown runs on the calling
  thread; no worker threads are usable until the ThreadPool (06) initializes, and none
  may remain after it shuts down.
- **After `Running`:** the Kernel itself is passive. Worker work is delegated to the
  ThreadPool (06) and TaskScheduler (07). The Kernel's own state is read under an
  atomic/RW-lock; health polling is scheduled work.
- `State()` and `Uptime()` are **lock-free** (atomics); `GetHealth()` aggregates under a
  short read lock and must never block long (00 §7).

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.kernel.booted` | boot timing breakdown | Boot completes |
| `engine.kernel.state_changed` | `{from, to}` | Any state transition |
| `engine.kernel.shutdown_started` | reason | Shutdown begins |
| `engine.kernel.shutdown_complete` | uptime, exit summary | Clean stop |
| `engine.kernel.health_changed` | `HealthReport` | Global health crosses a threshold |
| `engine.kernel.panic` | `Error` | Panic mode entered |

> **Note:** the Kernel may not publish these during boot — the EventBus (05) is not yet
> initialized. Boot events are queued by the Logger (02) / memory buffer and replayed
> through the EventBus (05) once it is online.

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.system.health_changed` (any system) | Recompute global health; possibly `Pause()` |
| `engine.config.hot_reload` (03) | Re-evaluate boot-related settings where applicable |
| `engine.system.fatal_error` | Escalate to panic if the failing system requests it |

## 9. Dependencies

- **Creates (in order):** Logger (02) → ConfigurationManager (03) → ServiceManager (04)
  → EventBus (05) → ThreadPool (06) → TaskScheduler (07) → ResourceManager (10) →
  MemoryManager (11) → PluginManager (09) → ModuleManager (08) → LifecycleManager (12).
- **Depends on:** none at construction; everything it needs is passed in via
  `BootOptions` and `GlobalContext`. After boot it *uses* all systems (health, events,
  scheduler).
- **Dependency graph invariant:** systems may only depend on systems earlier in the
  boot order (enforced at runtime, 00 §9).

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| System fails to initialize during boot | Abort boot, run reverse-order shutdown of already-initialized systems, return `Result` with the causing `Error`, enter `Stopped` (or `CrashRecovery` state recorded for next boot) |
| Dependency missing/cyclic/out-of-order | Fail fast before any system initializes; emit `engine.kernel.panic` |
| Unclean shutdown (crash/power loss) | On next boot, enter `CrashRecovery`: scan memory/log buffers, replay last log segment, report recovery to the user, then boot normally in `--safe-mode` if requested |
| Health degradation | Auto-transition to `Paused`; resume when health recovers |
| Unrecoverable subsystem failure | `Panic()`: flush logs (02) → release memory (11) → stop threads (06) → exit with code mapped from `Error`; target: deterministic within bounded time |
| Double `Boot()` / `Shutdown()` | Idempotent; second call returns `Ok` with no-op or an `Error` describing the invalid state, never undefined behavior |

## 11. Performance Goals

| Goal | Target |
|---|---|
| Boot to `Running` | **< 200 ms** on a mid-range desktop (excluding module/plugin load, which is deferred) |
| Shutdown | **< 100 ms** |
| `State()` / `Uptime()` reads | lock-free, < 1 µs |
| Global health aggregate | < 1 ms, non-blocking |
| Boot memory overhead | < 8 MB before modules load |

## 12. Future Extensions

These must be achievable **without redesigning the Kernel**:

- **Hot-restart / in-place upgrade** of the core (boot new core into an arena while the
  old one drains — MemoryManager (11) shared arenas).
- **Multiple engine instances** in one process (e.g. sandboxed preview) — Kernel is a
  singleton per *instance*, not per *process*.
- **Remote kernel control** (pause/resume/panic over the remote module) via the same
  public API.
- **Fault injection hooks** (`--fault-inject=<list>`) for failure testing (00 §8).
- **Persistence of boot diagnostics** (last-boot report surfaced in-app).

## 13. Testing Requirements

- **Unit:** every state transition; illegal transitions; double boot/shutdown;
  dependency-graph validation (missing/cyclic/out-of-order).
- **Stress:** 100 consecutive boot/shutdown cycles — no leaks, stable memory.
- **Performance:** boot/shutdown budgets from §11 asserted in CI.
- **Failure:** injected init failure at each position in the boot order → verify reverse
  teardown and `CrashRecovery` on the next boot.
