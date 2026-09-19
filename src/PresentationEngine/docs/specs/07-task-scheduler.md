# 07 — TaskScheduler Specification

| Field | Value |
|---|---|
| **System** | TaskScheduler |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/task_scheduler/TaskScheduler.hpp`, `core/task_scheduler/TaskScheduler.cpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04), ThreadPool (06) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The TaskScheduler is **responsible for time** in the engine. It is the single authority
for one-shot, recurring, delayed, cron, countdown, animation, and heartbeat tasks. It
provides millisecond and microsecond precision, and it is what every other system uses
to "do something later" — instead of spinning up their own timers or threads.

## 2. Responsibilities

- Own a centralized timeline of scheduled work.
- Support task kinds: **One Shot, Recurring, Delayed, Cron, Countdown, Animation,
  Heartbeat**.
- Provide `Schedule / Cancel / Pause / Resume / Reschedule` for every task kind.
- Deliver **microsecond** precision for animation/heartbeat; **millisecond** for
  ordinary timers.
- Hand task execution off to the ThreadPool (06) on the right class (`Realtime` for
  animation/heartbeat, `Background` for housekeeping).
- Expose a monotonic "now" (`engine time`) derived from the Kernel's global clock (01).
- Drive timeouts and periodic checks for other systems (EventBus 05, ThreadPool 06,
  ResourceManager 10, hot reload watching 03).

## 3. Public API

```cpp
class TaskScheduler final {
public:
    static TaskScheduler& Instance();

    // Scheduling
    Result<TaskId> Schedule(OneShot task, TimePoint at, ScheduleOptions opts = {});
    Result<TaskId> ScheduleDelayed(Task task, std::chrono::microseconds delay, ScheduleOptions opts = {});
    Result<TaskId> ScheduleRecurring(Task task, std::chrono::microseconds period, ScheduleOptions opts = {});
    Result<TaskId> ScheduleCron(Task task, const CronSpec& spec, ScheduleOptions opts = {});   // 5/6-field
    Result<TaskId> ScheduleCountdown(Task tick, std::chrono::microseconds total,
                                     std::chrono::microseconds interval, ScheduleOptions opts = {});
    Result<TaskId> ScheduleAnimation(AnimationFrame fn, Duration total, ScheduleOptions opts = {}); // vsync/µs ticks
    Result<TaskId> ScheduleHeartbeat(Task beat, std::chrono::microseconds period, ScheduleOptions opts = {});

    // Control
    Result<void> Cancel(TaskId id);
    Result<void> Pause(TaskId id);          // stop ticking, keep state
    Result<void> Resume(TaskId id);
    Result<void> Reschedule(TaskId id, std::chrono::microseconds newPeriodOrDelay);
    Result<void> PauseAll();  Result<void> ResumeAll();

    // Introspection
    TimePoint     Now() const noexcept;                     // engine monotonic time (01 Clock)
    size_t        ScheduledCount() const noexcept;
    std::vector<TaskInfo> Snapshot() const;                 // next-fire, kind, state

    // Diagnostics
    HealthReport  GetHealth() const noexcept;               // 00 §7
    SchedulerMetrics Metrics() const noexcept;              // 00 §4
};
```

`ScheduleOptions`: `{taskClass (default Background; Realtime for animation/heartbeat),
priority, jitter (for cron de-sync), name, cancelOnShutdown}`.

## 4. Internal Components

| Component | Role |
|---|---|
| `TaskScheduler` | Facade; task registry |
| `Timeline` | Monotonic min-heap of due events (µs resolution) |
| `Task` | Kind, callback, period, next-fire, state, class |
| `Timer` | The single timing primitive: adaptive sleep (polls) or OS timerfd/`CreateWaitableTimer` with wake on change |
| `CronEvaluator` | Next-fire computation from CronSpec; handles DST-less engine time + optional jitter |
| `AnimationDriver` | Tick generation for animations (60/120/144 Hz or µs), aligned to display refresh when available |
| `HeartbeatMonitor` | Periodic alive-beats for diagnostics (02, 10) |
| `HandoffQueue` | Bounded bridge to ThreadPool (06) per task class |

## 5. Task States

```
Scheduled --> Due --> Dispatched --> Running (on pool) --> Done
   |          |          |
   |          +----> Cancelled
   |          |
   +--> Paused --> Scheduled
   +--> Cancelled
```

Recurring/cron/heartbeat tasks loop back to `Scheduled` after each run; `Paused` keeps
`next-fire` frozen and restarts on `Resume`.

## 6. Threading Model

- **Multi-threaded, single timer thread.** One timer thread owns the `Timeline` and
  wakes the pool on due tasks. Submissions from any thread enqueue into the timeline
  under a short lock (or lock-free if a read-mostly structure is preferred).
- **Execution never happens on the timer thread** — all callbacks run on the ThreadPool
  (06), so a slow callback cannot stall the timeline.
- **Precision strategy:** sleep to the next due time; on `Realtime` tasks, the timer
  wakes early by a bounded margin (e.g. 1–2 ms) and the pool handles precise dispatch.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.scheduler.task_scheduled` | `TaskInfo` | New task registered |
| `engine.scheduler.task_cancelled` | `{id, kind}` | Task cancelled |
| `engine.scheduler.overrun` | `{id, kind, overdue}` | Task ran late vs. its budget |
| `engine.scheduler.clock_skew` | `{skew}` | Monotonic/wall clock drift detected (01) |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.kernel.state_changed` (01) | `Paused` → `PauseAll()` (heartbeats continue if allowed); `Resumed` → `ResumeAll()` |
| `engine.resource.mode_changed` (10) | Scale recurring intervals (e.g. `Battery` → lower polling frequency) |
| `engine.config.hot_reload` (03) | Re-read polling intervals / cron specs live |

## 9. Dependencies

- **Depends on:** Logger (02), ConfigurationManager (03), ServiceManager (04),
  ThreadPool (06).
- **Uses after init:** Kernel Clock (01) for `Now()`; EventBus (05) to publish.
- **Provides:** timekeeping for EventBus delayed delivery (05), ThreadPool timeouts
  (06), config watching (03), resource telemetry (10), and module/plugin timers.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Timer wake missed (OS starvation) | Overrun detection; tasks run on the next wake; `overrun` event with the measured delay |
| Callback overruns its schedule | Recurring tasks never pile up — skip missed occurrences (`skipIfLate` option) or catch up, per task |
| Handoff queue full (pool saturated) | Delay handoff, record `overrun`; never drop silently |
| `Cancel` of an already-dispatched task | Best-effort cancel on the pool (06); if running, completion is allowed |
| Clock skew / jumps | Re-anchor timeline to the Kernel clock; publish `clock_skew`; no task is lost (recomputed next-fire) |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Schedule*()` call overhead | **< 1 µs** |
| Timer wake accuracy, ms-class tasks | ± **1 ms** (p99) |
| Timer wake accuracy, µs-class (Realtime) | ± **100 µs** (p99) on idle CPU |
| Sustained timer load | 100 k scheduled tasks, 10 k due/s without overrun |
| Timer thread CPU | **< 1%** idle |

## 12. Future Extensions

- **Frame-aligned scheduling** for animations (align ticks to the Display's vsync, 10).
- **Distributed time sync** for multi-machine staging (network module) — timeline stays
  local, offsets applied at dispatch.
- **Cron with timezone/location support** for service events.
- **Rate-limiters / throttlers** as first-class task kinds (e.g. "at most once per N").
- **Idle-aware scheduling** — defer background tasks during service / performance mode.

## 13. Testing Requirements

- **Unit:** each task kind, pause/resume/reschedule, cron next-fire edge cases, cancel
  semantics, skipIfLate.
- **Stress:** 100 k scheduled tasks with 10 k due/s — no drift accumulation, no lost
  fires (within policy).
- **Performance:** assert §11 budgets (scheduling overhead, wake accuracy).
- **Failure:** timer starvation simulation, callback overrun, clock jump injection —
  verify overrun events and re-anchoring.
