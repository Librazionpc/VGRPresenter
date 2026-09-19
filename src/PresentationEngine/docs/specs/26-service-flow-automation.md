# 26 — Service Flow, Playlist & Automation Engine

Phase 14. Builds on Phases 1–13 (core, PAL, CAMS, notifications, projects,
adaptive, rendering, display, presentation, search, media, VGR, scene, bible,
song). The engine that coordinates everything else — a deterministic,
event-driven automation system that runs an entire service from start to
finish without the UI coordinating transitions.

## 1. Purpose

Run a complete production (a church service, conference, concert) from a
single definition: a **Flow** of executable nodes with triggers, conditions,
actions, waits, branches, loops, parallel sections, macros, templates, and
variables. The Automation Engine decides *when and how* things happen; the
individual engines (Song, Bible, Media, Scene, Presentation, Display) remain
responsible for *doing* their own jobs. It is deliberately generic — built
around Trigger → Condition → Action → Event → Next Action, never around
"Songs, then videos" — so new content types and external systems (MIDI, DMX,
OBS, cameras) plug in without redesign.

The engine is UI-independent: the same flow runs from the CLI, a future WinUI
app, a remote controller, or an AI assistant. It never renders, never plays
media, never composes scenes itself.

## 2. Responsibilities

- Load, validate, and execute service flows (nodes, cues, timeline).
- Own triggers (manual, time, EventBus, completion, countdown, media) and
  conditional branching (IF/ELSE) with extensible conditions.
- Execute actions through a plugin registry; never hardcode engine calls.
- Support parallel execution with dependency/synchronization (WAIT FOR).
- Provide full manual override (pause/resume/skip/jump/stop/restart/override)
  and a dedicated emergency stop that works even mid-flow.
- Manage macros, flow templates, and `.vgr` document integration.
- Maintain variables with data binding; publish/subscribe through the
  EventBus; notify via the Notification service events.
- Record a complete execution history (flow/node/timestamp/state/result) and
  provide crash recovery (active flow, current node, pending/completed nodes,
  variables, timers, outputs).
- Guarantee deterministic execution and never block rendering, media, audio,
  EventBus, or UI (heavy work goes through ThreadPool/TaskScheduler).

## 3. Public API

```text
FlowEngine (IService facade, singleton via Instance())
├── Lifecycle: Initialize / Start / Stop / Shutdown / Reload / Reset
├── Health: GetHealth / MetricsSnapshot
├── Registry
│   ├── RegisterAction(name, factory) / UnregisterAction / ActionNames
│   ├── RegisterCondition(name, factory) / UnregisterCondition / ConditionNames
│   └── RegisterTrigger(name, factory) / UnregisterTrigger / TriggerNames
├── Flow management
│   ├── Load(source, format)  → FlowId            (validate + convert)
│   ├── Validate(flow)        → warnings
│   ├── Save(flow, format)    → .vgr / json text
│   ├── GetFlow(id) / FlowIds / FlowCount / RemoveFlow(id)
│   ├── Template: CreateTemplate(flow, name) / Templates / ApplyTemplate(templateId)
│   └── Macro: RegisterMacro(name, actions) / RunMacro(name, vars)
├── Execution
│   ├── Start(flowId, vars)              → ExecutionId
│   ├── Pause / Resume / Stop / Restart(execId)
│   ├── Skip / JumpTo(nodeId) / Override(action)
│   ├── EmergencyStop()
│   └── ExecuteAction(action, vars)      (manual single action)
├── State & history
│   ├── ExecutionState(execId)  → current node, next, waiting-for, conditions
│   ├── History(execId)         → ordered NodeRecord list
│   ├── Snapshot(execId)        → recovery JSON
│   ├── Recover(snapshot)       → new execution resuming from saved state
│   └── DebugInfo(execId)       → timings, waiting-for, active conditions, vars
├── Variables
│   ├── SetVariable(execId, name, value) / GetVariable / Variables(execId)
│   └── (data binding) bound widget consumers are notified via events
└── Events: WireEvents / UnwireEvents
```

Every public function returns `Result<T>`; errors carry module `"FlowEngine"`
and a `Flow_*` code from Common.hpp.

## 4. Internal Components

- **FlowModel** — canonical flow representation: id, name, version, nodes,
  variables, variablesAreBound, timeline entries, and origin (template).
- **FlowNode** — id, label, kind, action reference (or inline action), trigger,
  condition, children (branch paths), parallel group flag, cues
  (BeforeStart/OnStart/During/OnComplete/OnError/OnCancel), waitFor,
  timeOffsetMs (timeline), skip/retry/fallback policy.
- **ActionRegistry** — name → factory of `IAction`; built-ins: Scene,
  Presentation, Song, Bible, Media, Countdown, Timer, Audio, Video, Macro,
  Plugin, ExternalCommand, MidI, Output, Notification, Delay, SendEvent,
  NoOp. Each action: `Type()`, `Validate()`, `Execute(ctx) → Result<ActionOutcome>`
  where outcome may be `Completed`, `WaitingFor(event/timer/condition)`, `Failed`.
- **ConditionRegistry** — name → factory of `ICondition`; built-ins:
  VariableEquals, OutputIs, GpuAvailable, EventReceived, VariableNotSet,
  True/False. `ICondition::Evaluate(ctx) → bool`.
- **TriggerRegistry** — name → factory of `ITrigger`; built-ins: Manual, Time,
  EventBus, OnPreviousComplete, CountdownComplete, MediaComplete.
- **Executor** — a single execution context per run: an ordered engine that
  walks the node graph, honors branch/loop/parallel semantics, and yields to
  the ThreadPool for any non-trivial work.
- **EventMapper** — subscribes to EventBus events (VideoCompleted, SongStarted,
  SongCompleted, TimerFinished, DisplayConnected, MIDIReceived, RemoteCommand…)
  and feeds them to waiting nodes.
- **HistoryRecorder** — append-only per-execution records.
- **VariableStore** — per-execution variable map with `{NAME}` substitution
  and binding notifications.
- **RecoveryStore** — JSON snapshots of executions for crash recovery.
- **Validator** — structural checks (duplicate ids, dangling references,
  unknown action/condition/trigger types, cycles).

## 5. State Machine

```text
Idle ──Start──► Running ──Pause──► Paused ──Resume──► Running
  ▲               │  │
  │               │  └──EmergencyStop──► Stopped(emergency)
  │          Stop/Restart
  └───────Stopped ──── (terminal; Restart returns to Running)
```

Node-level states: `Pending → Running → Completed | Failed | Skipped | Cancelled`.
Flow-level states: `Idle, Validating, Running, Paused, Stopped, EmergencyStopped,
Recovered, Completed`. Manual overrides always take priority over automation.

## 6. Threading Model

The executor runs on its own serial context (single-threaded engine loop per
execution) so node ordering is deterministic. Actions that are slow or
blocking are dispatched to the ThreadPool/TaskScheduler and awaited through
futures/events; the engine never blocks a render/presentation/media thread.
The facade guards its maps with a mutex; parallel nodes fan out to pool tasks
but synchronize back to the serial loop before advancing. EventBus delivery
uses existing subscriber threads.

## 7. Events

Consumes (through EventMapper): `VideoCompleted`, `SongStarted`,
`SongFinished`, `BiblePassageLoaded`, `TimerFinished`, `DisplayConnected`,
`DisplayDisconnected`, `MIDIReceived`, `RemoteCommandReceived`,
`ResourceStateChanged`, `PresentationStateChanged`, `SceneComposed`, and the
engine's own published events (for chained flows).

Publishes: `FlowStarted`, `FlowPaused`, `FlowResumed`, `FlowCompleted`,
`FlowStopped`, `FlowInterrupted`, `FlowRecovered`, `FlowValidated`,
`NodeStarted`, `NodeCompleted`, `NodeFailed`, `NodeSkipped`,
`AutomationCancelled`, `AutomationEmergencyStopped`, `VariableChanged`.

Notifications are not generated inside the engine — the Notification service
consumes these events (started/completed/action-failed/device-unavailable/
interrupted/recovery-completed).

## 8. Dependencies

Depends on the Core Engine (EventBus, ThreadPool, TaskScheduler, Logger,
ConfigurationManager) and the `search` engine for indexing flow documents.
Interacts with Bible/Song/Media/Scene/Presentation/Display engines only through
registered actions (decoupled, plugin-style) — no direct includes of those
facades inside the executor core. `FlowEngine` is registered as a service in
the Kernel (boot step 26) and is consumed by the CLI app and tests; no UI code.

## 9. Failure Modes

- Node action fails → per-node policy: `Retry` (N attempts), `Skip`,
  `Fallback(nodeId)`, `Pause` (operator), `Notify` (event only), `Abort`.
- Wait-for-event never arrives → timeout policy per node (default: fail after
  node's `waitTimeoutMs`, default 0 = wait indefinitely with operator escape).
- Missing media/device/output → action returns `Flow_ActionFailed` with the
  reason; engine never crashes; recovery snapshot stays consistent.
- Corrupt flow document → validation error at Load time; never loaded.
- Crash mid-execution → snapshot persisted; `Recover()` restores state.
- Unknown action/condition/trigger type at load → validation error listing the
  unknown name.

## 10. Performance Goals

- Load + validate a 200-node flow < 5 ms.
- Node advance (no-op action) < 50 µs in the serial loop.
- Parallel fan-out of 8 nodes completes without blocking the loop.
- History capped per execution (configurable, default 4096 records).
- No allocation churn in the hot path; recovery snapshot < 1 ms for 100 nodes.
- Memory stays flat with large flows (shared templates, no node duplication).

## 11. Testing

Unit + failure + integration coverage mirrors the other phases:

- Model/validation: build a Sunday-Service flow, validate ok; reject duplicate
  ids, unknown action types, dangling wait targets, cycles.
- Executor: run a linear flow Start→…→Completed with event-driven waits
  (WAIT FOR VideoCompleted then Song starts automatically); manual skip/jump/
  pause/resume/stop; emergency stop mid-node; override executes an action.
- Conditions/branching: IF VariableEquals → path A ELSE path B.
- Parallel: fan-out group runs and all outcomes are recorded, loop exits when
  all branches complete.
- Macros + templates: create/reuse macro; create template, apply it, derived
  flow is independent.
- Variables: `{SPEAKER_NAME}` substitution and VariableChanged binding event.
- History: execution records ordered and complete (started/executed/completed).
- Recovery: snapshot → wipe → Recover → execution resumes at saved node.
- Failure: failing action with Retry/Skip/Fallback/Abort policies behaves per
  policy; missing media does not crash.
- Integration: FlowEngine boots with the Kernel (boot step 26) and the CLI
  loads + runs a Sunday-Service flow end to end.
