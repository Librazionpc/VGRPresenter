# 02 — Logger Specification

| Field | Value |
|---|---|
| **System** | Logger |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/logging/Logger.hpp`, `core/logging/Logger.cpp` |
| **Depends on** | Nothing (initialized first; must work standalone) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The Logger is the **single output path** for the entire engine. *Everything logs.
Nothing uses `std::cout`.* It exists so that every subsystem — core, modules, plugins,
drivers — reports through one uniform, thread-safe, async, filterable pipeline.

## 2. Responsibilities

- Accept log records from any thread with zero API friction.
- Format records uniformly (timestamp, thread ID, module name, category, level).
- Route records to **all active sinks** concurrently (console, file, JSON, debugger,
  remote, memory buffer).
- Buffer asynchronously so logging never blocks the caller beyond a bounded cost.
- Enforce level filtering per sink and per category.
- Rotate, compress, and (optionally) encrypt file output.
- Provide search/replay over the in-memory buffer (used by diagnostics and crash
  recovery, 01).
- Own log retention and disk-pressure policy (cooperates with ResourceManager 10).

## 3. Public API

```cpp
class Logger final {
public:
    static Logger& Instance();                     // available before any other system

    // Core logging — module + category tagged
    void Log(LogLevel level, std::string_view module,
             std::string_view category, std::string_view message);
    void Trace(...); void Debug(...); void Info(...);
    void Warning(...); void Error(...); void Fatal(...);   // convenience overloads

    // Sinks
    Result<void> AddSink(std::shared_ptr<ISink> sink);
    Result<void> RemoveSink(std::string_view sinkName);
    void         SetSinkLevel(std::string_view sinkName, LogLevel minLevel);
    void         SetGlobalLevel(LogLevel minLevel);        // filter for ALL sinks
    void         SetCategoryLevel(std::string_view category, LogLevel minLevel);

    // Rotation / retention
    Result<void> SetRotationPolicy(const RotationPolicy& policy);   // size/time/count
    Result<void> Flush();                                          // drain async queue, durable
    Result<void> Shutdown();                                       // flush + close sinks

    // Memory buffer / search
    const LogBuffer& Buffer() const noexcept;
    std::vector<LogRecord> Search(const LogQuery& query) const;    // level/module/category/text/time range

    // Diagnostics
    HealthReport GetHealth() const noexcept;                       // 00 §7
    LoggerMetrics Metrics() const noexcept;                        // 00 §4
};
```

`LogLevel`: `Trace < Debug < Info < Warning < Error < Fatal`.

## 4. Internal Components

| Component | Role |
|---|---|
| `Logger` | Facade; thread-safe API, global + per-category filters |
| `LogRecord` | Level, timestamp, thread ID, module, category, message, optional KV tags, error code |
| `AsyncQueue` | Bounded SPSC/MPSC queue between callers and logger thread (backpressure policy below) |
| `LoggerThread` | Single dedicated worker: formats, fans out to sinks, rotates |
| `ISink` / `ConsoleSink` / `FileSink` / `JsonSink` / `DebuggerSink` / `RemoteSink` / `MemorySink` | Output backends |
| `LogBuffer` | Bounded ring buffer for recent records; used by search, crash recovery, diagnostics |
| `RotationManager` | Size/time/count-based rotation; compression (zstd) and optional encryption |
| `FilterEngine` | Level + category matching, evaluated once per record |
| `Formatter` | Text/JSON formatters (`[INFO][Media][Thread-5] Video cache initialized.`) |

## 5. State Machine

```
Idle --> Running --> Flushing --> Shutdown
         |   ^
         +---+  (automatic: queue drains continuously)
```

| State | Meaning |
|---|---|
| `Idle` | Pre-init; calls are buffered in a small pre-queue or dropped per policy |
| `Running` | Logger thread active; records flow |
| `Flushing` | `Flush()` in progress; new records queue up |
| `Shutdown` | Sinks closed; further calls are no-ops returning `Error` in debug |

## 6. Threading Model

- **Multi-threaded, single-writer.** Any thread may call `Log*()`; callers enqueue into
  a bounded lock-free queue (fast path: one atomic store). A **single logger thread**
  owns formatting + sink fan-out, so sinks never need their own locks.
- **Backpressure:** if the queue is full, callers **drop records** (counted metric,
  `logger.dropped_records`) rather than block — logging must never stall the caller.
  `Fatal` records are enqueued synchronously with a bounded wait to guarantee delivery.
- `Flush()` blocks until all prior records are durable; used at shutdown and before
  crash dumps.

## 7. Events Published

| Event | Payload | When |
|---|---|---|
| `engine.logger.rotated` | `{old_path, new_path, reason}` | File rotation occurred |
| `engine.logger.level_changed` | global/per-category level | Filter changes |
| `engine.logger.buffer_trimmed` | dropped range | Memory buffer full (pressure) |
| `engine.logger.error` | `LogRecord` | An `Error`/`Fatal` record is emitted (for dashboards) |

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.resource.pressure_high` (10) | Reduce verbosity: raise global level, trim buffer, rotate+compress aggressively |
| `engine.config.hot_reload` (03) | Apply new log levels / rotation policy live |

## 9. Dependencies

- **Creates nothing.** Depends on nothing at init (must run before Config and EventBus).
- **Uses after init:** EventBus (05) to publish events; ResourceManager (10) for
  disk-pressure and retention policy; ConfigurationManager (03) for settings.
- **Constraint:** because Logger boots before the EventBus, pre-boot records are kept in
  the memory buffer and replayed after boot (01 §7).

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Sink write fails (disk full, permission) | Mark sink `Degraded`, count error, continue with other sinks; retry on next rotation interval; raise `engine.logger.error` |
| Queue overflow | Drop records (counted); never block callers; `Fatal` guaranteed delivery |
| Disk pressure | Raise global level to `Warning`, trim memory buffer, compress old segments (10) |
| Logger thread crash | Re-spawn once; if it crashes again within 1 s, fall back to synchronous minimal stderr logging and surface `Panic` via Kernel (01) |
| Corrupt log file at open | Rename to `.corrupt`, start a fresh file, report error |
| `Shutdown()` called twice | Idempotent, no-op |

## 11. Performance Goals

| Goal | Target |
|---|---|
| `Log*()` caller cost | **< 1 µs** fast path (queue enqueue only) |
| End-to-end latency (enqueue → sink) | p50 < 1 ms, p99 < 5 ms |
| Sustained throughput | **> 1 M records/s** (single logger thread, console+file) |
| Memory buffer | 32 MB ring default, configurable, bounded |
| File rotation overhead | < 1 ms amortized; compression off the caller path |

## 12. Future Extensions

- **Structured tracing** (spans/correlations) layered on the same pipeline.
- **Remote log streaming** (WebSocket/gRPC) via `RemoteSink` for the remote-control
  module — no redesign.
- **Per-module file sinks** and per-session log archives.
- **Anonymized crash telemetry** export (opt-in) reusing the JSON sink + rotation.
- **Log signing** for tamper-evident audit trails.

## 13. Testing Requirements

- **Unit:** level filtering, category filtering, all formatters, rotation boundaries,
  sink lifecycle, search.
- **Stress:** 10 M records from 8 threads — zero deadlock, bounded drop rate, no leaks.
- **Performance:** assert §11 budgets (caller latency, throughput).
- **Failure:** sink throwing, disk full, logger thread killed mid-queue, corrupt file —
  verify degradation and recovery per §10.
