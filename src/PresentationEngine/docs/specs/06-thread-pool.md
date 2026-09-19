# 06 — ThreadPool Specification

| Field | Value |
|---|---|
| **System** | ThreadPool |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/threading/ThreadPool.hpp`, `core/threading/ThreadPool.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The ThreadPool is the engine's **central execution fabric**. **No module owns threads** —
all concurrency that isn't explicitly realtime-synchronous is expressed as tasks
submitted here. Centralizing threads gives the engine CPU balancing, priority control,
affinity, cancellation, and clean shutdown — and it is what ResourceManager (10) uses to
scale concurrency with pressure.

## 2. Responsibilities

- Own the worker thread count and lifecycle (dynamic resize).
- Execute tasks from a **priority queue** with fair scheduling across task classes.
- Support **affinity** (pin task streams to cores), **cancellation**, and **timeouts**.
- Classify work as `Foreground`, `Background`, or `Realtime` with distinct budgets.
- Track task states for diagnostics and leak detection.
- Provide CPU balancing (grow/shrink workers with load, respecting the resource mode 10).
- Guarantee graceful shutdown: drain foreground, cancel/discard background, join all.

## 3. Public API

```cpp
class ThreadPool final {
public:
    static ThreadPool& Instance();

    // Submission
    Result<TaskHandle> Submit(Task task, TaskOptions opts = {});      // fire-and-forget
    template <class F, class... Args>
    Result<TaskHandle> SubmitAsync(TaskClass klass, int priority, F&& f, Args&&... args);
    template <class F, class... Args>                                  // returns value via future
    Result<std::future<R>> SubmitFuture(TaskClass klass, int priority, F&& f, Args&&... args);

    // Control
    Result<void> Cancel(TaskHandle handle);                            // best-effort; see states
    Result<void> SetTimeout(TaskHandle handle, std::chrono::microseconds timeout);
    Result<void> Resize(size_t workers);                               // dynamic grow/shrink
    Result<void> SetAffinity(TaskHandle handle, CpuSet cpus);

    // Class budgets
    Result<void> SetClassLimits(TaskClass klass, const ClassLimits& limits);  // max workers %, concurrency

    // Shutdown
    Result<void> Shutdown(ShutdownPolicy policy);                      // Drain | CancelBackground | Immediate
    size_t       PendingCount() const noexcept;
    size_t       WorkerCount() const noexcept;

    // Diagnostics
    HealthReport GetHealth() const noexcept;                           // 00 §7
    PoolMetrics  Metrics() const noexcept;                             // 00 §4
};
```

`TaskClass`: `Realtime` (highest priority, bounded, low-latency) > `Foreground` >
`Background` (throttled, preemptable). `TaskOptions`: `{class, priority, affinity,
timeout, cancellable, name}`.

## 4. Internal Components

| Component | Role |
|---|---|
| `ThreadPool` | Facade; config + class budgets |
| `TaskQueue` | Three class-tiered priority queues (or one queue with class-weighted scheduling) |
| `Worker` | OS thread + local run loop; affinity, wake/sleep |
| `Scheduler` | Class-aware pop: realtime first, foreground by priority, background throttled by CPU budget |
| `CancellationRegistry` | Cooperative cancel flags + timed-abort for blocking tasks |
| `TimeoutTracker` | Scheduler (07)-driven timeout reaping |
| `LoadMonitor` | Per-worker utilization; drives `Resize()` and 10-pressure reporting |
| `TaskStateTable` | Tracked states for diagnostics (below) |

## 5. Task States

```
Queued --> Running --> Finished
           |   ^
           |   +----> Blocked (awaiting IO/subtask) --> Running
           |
           +--> Cancelled   (cooperative cancel before start, or abort mid-run)
           +--> TimedOut    (timeout fired)
```

| State | Meaning |
|---|---|
| `Queued` | Waiting in a class/priority queue |
| `Running` | Executing on a worker |
| `Blocked` | Waiting on an internal dependency (IO, future) — counted separately for load |
| `Cancelled` | Cooperative cancel acknowledged; or cancelled before start |
| `TimedOut` | Budget exceeded; task aborted by `TimeoutTracker` |
| `Failed` | Task raised an uncaught exception; worker reaped it and reported via 02 |
| `Finished` | Completed; handle invalidated after a grace period |

Cancellation is **cooperative** for running tasks (a flag checked at yield points);
only tasks explicitly registered as abortable may be force-terminated (with
`Cancelled` + a diagnostic).

## 6. Threading Model

- **Multi-threaded.** `Submit*` is lock-free-enough (MPMC queue with bounded CAS); any
  thread may submit at any time.
- **Workers:** one OS thread per worker; `Realtime` workers can be pinned to dedicated
  cores via affinity.
- **Blocking policy:** `Blocked` tasks release their worker to other work (continuation
  hand-off) so a blocked task never pins a worker — this is what keeps the pool
  efficient with IO-heavy modules (media decoding, network).
- **No nested pool submissions that deadlock:** a task awaiting a future from the same
  pool uses continuation-based waiting, never `wait()` on a busy worker.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.threadpool.worker_added/removed` | `{count, reason}` | Dynamic resize |
| `engine.threadpool.task_cancelled` | `{handle, class}` | Cancellation applied |
| `engine.threadpool.task_timeout` | `{handle, class, budget}` | Timeout fired |
| `engine.threadpool.starvation` | `{class, wait_p95}` | Class starvation detected |
| `engine.threadpool.cpu_pressure` | `{utilization_pct}` | Sustained > 90% utilization |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.resource.mode_changed` (10) | Adjust class budgets and worker range per mode (e.g. `Battery` shrinks background) |
| `engine.kernel.state_changed` (01) | On `Paused`: pause background class; on shutdown: run policy drain |
| `engine.config.hot_reload` (03) | Apply new worker range / class limits live |

## 9. Dependencies

- **Depends on:** Logger (02), ConfigurationManager (03), ServiceManager (04).
- **Uses after init:** TaskScheduler (07) for timeouts/starvation checks; EventBus (05)
  to publish; ResourceManager (10) to report and receive CPU pressure.
- **Provides:** execution for EventBus async fan-out (05), TaskScheduler timers (07),
  module/plugin background work (08, 09), and every other concurrent workload.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Worker crashes (uncaught exception) | Catch, log `Fatal` via 02, reap the worker, replace it; task marked `Failed`; pool stays healthy |
| Task exceeds timeout | Cooperative abort → `TimedOut`; if uncooperative, isolate and report (never kill the process) |
| Queue saturation | Apply class throttling (background first), then surface `starvation`; `Submit` returns backpressure `Error` at the configured hard cap |
| `Resize(0)` mid-shutdown | Treated as shutdown request; policy applied |
| Cancellation of a non-cancellable task | `Error` returned; task continues |
| OS thread creation failure | Degrade: reduce target workers, report health `Degraded`, retry with backoff |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Submit` enqueue | **< 500 ns** |
| `Realtime` dispatch latency (queued → start) | **< 50 µs** p99 |
| `Foreground` wait p99 | **< 1 ms** at 8× core load |
| Worker overhead | idle workers: < 1% CPU total (sleep-poll with adaptive wake) |
| Graceful shutdown | drain + join **< 50 ms** (foreground), background cancelled |

## 12. Future Extensions

- **Work-stealing pool** variant for CPU-bound graph work (rendering, AI) behind the
  same API.
- **Fiber-based tasks** (cooperative coroutines) for IO-heavy modules — transparently
  replaces `Blocked` states without API change.
- **Per-module worker isolation** (a module's tasks can't starve the core) via class +
  budget extensions.
- **Remote task dispatch** for distributed rendering later.

## 13. Testing Requirements

- **Unit:** all states (incl. Blocked), cancellation semantics, timeouts, affinity,
  resize, class budgets, priority order.
- **Stress:** 1 M tasks across 16 producers with mixed classes — no lost tasks, no
  deadlock, bounded queues.
- **Performance:** assert §11 budgets under 8× core load.
- **Failure:** worker crash injection, OS thread failure, queue saturation, timeout
  storms — verify reaping, throttling, and health reporting.
