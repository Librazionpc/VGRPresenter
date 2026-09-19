# 00 — Cross-System Rules

| Field | Value |
|---|---|
| **System** | Engine-wide conventions |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Applies to** | All systems (01–12) |

These rules apply everywhere. A spec or implementation that violates them is invalid,
regardless of any other property. They are listed in descending order of importance.

---

## 1. Error system — `Result<T>`

Every function that can fail returns `Result<T>`, never a raw `bool`, and never an
exception across system boundaries.

```cpp
template <typename T>
class Result {
  bool        ok() const;
  T&          value();            // UB if !ok()
  const T&    value() const;
  const Error& error() const;     // valid only if !ok()
};
```

An `Error` always carries:

| Field | Type | Notes |
|---|---|---|
| `code` | `ErrorCode` | Stable enum, namespaced per system (e.g. `ErrorCode::EventBus_SubscriberCrash`) |
| `message` | `std::string` | Human-readable, includes context |
| `module` | `ModuleId` | Which system/module produced it |
| `stack` | `std::vector<Frame>` | **Debug builds only** — resolved call stack; empty in release |
| `cause` | `std::optional<Error>` | Chained root cause |

Rules:

- **No exceptions across boundaries.** Systems may use exceptions internally only if
  they are fully contained; every public entry point must translate to `Result<T>`.
- **No silent failures.** Ignoring a `Result` is a compile-time warning target
  (`[[nodiscard]]` on `Result`).
- **No `std::cout` / `printf`** anywhere in the engine — the Logger (02) is the only
  output path.
- Failure handling must follow the **Failure Modes** section of the owning system's spec.

---

## 2. Dependency rules

- **No circular dependencies.** The dependency graph is a DAG. `docs/specs/README.md`
  defines the fixed initialization order, which is the topological order.
- **Depend on interfaces, never concrete implementations.** Systems reference
  `interfaces/` types (e.g. `IService`, `IModule`, `IPlugin`, `IDriver`, `IRenderer`,
  `IDisplay`, `IEvent`). Concrete types are registered into the ServiceManager (04) and
  resolved through it.
- **One direction of ownership.** A system that *owns* a resource must be the one to
  release it (see MemoryManager 11 and LifecycleManager 12 for the ownership rules).
- **Module isolation.** Modules (08) may depend on core systems and on other modules'
  *interfaces* only — never on another module's implementation.

---

## 3. Thread safety

Every public system must declare, in its spec, three things:

1. **Ownership** — which threads/objects own which state.
2. **Locking strategy** — mutex / RW-lock / lock-free / single-threaded, and the lock
   hierarchy (to prevent deadlock, all systems acquire locks in initialization order).
3. **Async behavior** — what is synchronous vs. posted to the ThreadPool (06) vs.
   scheduled by the TaskScheduler (07).

Global rules:

- The **EventBus (05)** is the only allowed *fire-and-forget* async communication.
- **No blocking on the render/UI threads** unless documented.
- All shared state is either (a) immutable after init, (b) owned by a single thread, or
  (c) protected by a documented lock with a documented lock hierarchy.

---

## 4. Metrics

Every manager exposes live metrics. The canonical metric set:

| Metric | Unit | Example |
|---|---|---|
| CPU | % or ns/sample | `logger.cpu_pct` |
| RAM | bytes | `eventbus.queue_bytes` |
| Latency | ns (p50/p95/p99) | `eventbus.dispatch_p99_ns` |
| Errors | counter + rate | `config.load_errors` |
| Queue length | count | `threadpool.queued_tasks` |
| Health | enum + detail | see §7 Diagnostics |

- Metrics are published on the EventBus (05) at a cadence owned by the system, and are
  always available synchronously for health checks (01 Kernel).
- Metric names are dot-namespaced: `<system>.<metric>`.

---

## 5. Hot reload

Everything should support `Reload()` without restarting the engine **whenever practical**.

- `Reload()` returns `Result<void>` and must be **safe to call while running**.
- Systems with active state must drain/quiesce first (see their spec's Failure Modes).
- Configuration changes propagate via events, not by polling (03, 05).

---

## 6. Versioning

Every versioned unit (module, plugin, service, driver) declares a manifest:

| Field | Required |
|---|---|
| Name | yes |
| Version | yes (semver) |
| Author | yes |
| Dependencies | yes (interface names + version ranges) |
| Required core version | yes |
| Capabilities | yes (machine-readable tag list) |

- The Kernel (01) publishes the **global engine version** and **minimum compatible core
  version**.
- Version mismatches are a load-time failure with a precise `Error` (see 08, 09).

---

## 7. Diagnostics

Every manager exposes:

```cpp
HealthReport GetHealth() const;   // always callable, never throws, never blocks long
```

`HealthReport` contains: overall state, per-subsystem health (Healthy / Degraded /
Failing), last error, uptime, and latency p95. The Kernel aggregates these into a global
health check and feeds the panic/health subsystem.

Two canonical health events are defined here and published by **every** system that has
a health report:

| Event | Payload | When |
|---|---|---|
| `engine.system.health_changed` | `{system, HealthReport}` | A system's health delta crosses a threshold |
| `engine.system.fatal_error` | `{system, Error}` | A system hits an unrecoverable failure |

The Kernel (01) consumes both to drive global health and panic policy.

---

## 8. Testing

Every manager **must** have:

| Category | Minimum |
|---|---|
| Unit tests | Every public API path, all state transitions, all error paths |
| Stress tests | Sustained load beyond design limits (e.g. 10× event rate) — no leaks, no crashes |
| Performance tests | Assert latency/memory budgets from the spec's Performance Goals |
| Failure tests | Inject failures (OOM, corrupt file, hung subscriber, crash) and verify specified recovery |

Tests live under `tests/unit/`, `tests/integration/`, `tests/performance/` and use the
engine's own diagnostics (02, 11) to assert invariants.

---

## 9. Initialization order (invariant)

```
01 Kernel
│
├── 02 Logger
├── 03 ConfigurationManager
├── 04 ServiceManager
├── 05 EventBus
├── 06 ThreadPool
├── 07 TaskScheduler
├── 10 ResourceManager
├── 11 MemoryManager
├── 09 PluginManager
├── 08 ModuleManager
└── 12 LifecycleManager
```

- Initialization follows this order; shutdown is the exact reverse.
- A system may only *use* systems above it in the list. Using a system lower in the list
  (e.g. Logger using the EventBus during startup) is forbidden except where a spec
  explicitly carves out a *post-init* path.
- This ordering is enforced at runtime by the Kernel's dependency graph (01).

**Ordering note (MemoryManager):** the original v1.0 boot-sequence sketch listed
"Initialize Memory" immediately after engine start, but the binding invariant is the
order diagram above (MemoryManager *after* ResourceManager). Consequently ResourceManager
reads memory telemetry from OS/platform probes and only cooperates with MemoryManager
post-init (see `10` §9).

---

## 10. Engine-wide manager contract

Every manager — core or planned — conforms to the same interface, defined in
`interfaces/include/interfaces/IService.hpp`. This is the *minimum* every manager supports:

```cpp
Result<void> Initialize();   // one-time setup; idempotent where possible
Result<void> Start();        // begin serving
Result<void> Stop();         // quiesce serving, keep loaded state
Result<void> Shutdown();     // full teardown, release resources
Result<void> Reload();       // hot reload (00 §5) — safe to call while running
Result<void> Reset();        // return to default configuration
HealthReport GetHealth() const;
Metrics      MetricsSnapshot() const;
```

**Reporting contract.** Every manager reports, via `IService`:

| Field | Accessor |
|---|---|
| Name | `ServiceName()` |
| Version | `ServiceVersion()` (defaults to engine version) |
| Uptime | `ServiceUptime()` (since construction) |
| Dependencies | `ServiceDependencies()` |
| Status / Health | `GetHealth().state` + `detail` |
| CPU / Memory / Threads / Errors / Queue | `MetricsSnapshot()` (`cpuPct`, `ramBytes`, `threadCount`, `errorCount`, `queueLength`) |
| Last error | `GetHealth().errorCount` + manager-specific detail |

**No exceptions.** Every method above returns `Result<T>` and never throws across the
boundary (00 §1). Default implementations on `IService` are safe no-ops so a manager can
adopt the contract incrementally.

**Integration rule.** Managers use the shared Logger (02), ConfigurationManager (03),
EventBus (05), and LifecycleManager (12) — none may implement its own version of those
systems.
