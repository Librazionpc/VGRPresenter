# 27 — Phase 15: Extended Production Engine (Signal Graph Architecture)

Status: **implemented** — see `docs/architecture/Conformance.md §30`.

## Objective

Build the real-time production backbone for the platform: audio, video, graphics,
data and control all flow as **signals through a universal node graph** —
Sources → Processing → Virtual Sources → Buses → Processing → Buses → Outputs —
for audio and video independently, so an output can be a complete signal
destination (Video Bus + Audio Bus + Configuration + Health + Cost). The engine
coordinates buses, outputs, resources and health; it never renders pixels,
plays audio, or routes to screens itself (those stay with the Rendering, Media
and Display engines).

The boundary that must hold:

```text
Scene Engine      → composes
Rendering Engine  → draws
Display Engine    → sends
Media Engine      → plays
Production Engine → routes, mixes, plans, monitors
```

## Core Model

- **Everything is a signal.** `SignalType { Audio, Video, Data, Control }`.
- **Universal node graph.** `NodeKind { Source, Processor, Bus, Mixer, Scene,
  Output, VirtualSource, Data, Control }`. A `ProductionGraph` owns the nodes
  and the directed routing edges between them.
- **Virtual signals.** Any bus can become a virtual source (routable, mixable,
  processable, recordable, sent to another bus or output).
- **Bus hierarchy.** Buses may feed other buses (Vocal → Worship → Program →
  Stream). **Circular routing is rejected at `Connect` time** (DFS cycle check).
- **Independent audio/video.** Switching video does not change audio and vice
  versa: every output carries its own `videoBusId` and `audioBusId`.
- **Processing per node.** Sources, buses and outputs each hold a processing
  chain (Gate, EQ, Compressor, Limiter, ColorCorrect, Delay, Gain, custom).
- **Volume everywhere.** Gain / Mute / Solo / Pan / Balance on every node.
- **Channels.** Mono, Stereo, 2.1, 5.1, 7.1, Multi — never hard-coded to stereo.
- **Meters.** Peak, RMS, LUFS, clipping detection, headroom, per-channel levels.
- **Ducking.** A trigger source (e.g. speaker mic) attenuates a target bus while
  active; the bus returns when the trigger stops.
- **Clock.** A `MasterClock` with audio/video/network offsets and a sync flag;
  every signal carries `{timestamp, duration, format, sampleRate, fps}`.

## Production Graph

```text
AddSource / AddBus / AddProcessor / AddVirtualSource / AddScene / AddOutput
        │
        ▼
Connect(from, to, signalType)   ← rejects circular routes (DFS)
        │
        ▼
Validate()                       ← missing nodes, dangling edges, outputs
                                      without buses, overload, unsupported
        │
        ▼
Prepare() → Validate → Commit()  ← atomic live changes (no intermediate states)
```

- `WouldCreateCycle(from, to)` runs a DFS from `to`; if it reaches `from` the
  edge is rejected with `Err::Production_CircularRoute`.
- `Snapshot()`/`Restore()` capture the full routing state; `BusSnapshot`s are
  named bus scenes that automation can switch between.
- `TopologicalOrder()` yields a deterministic processing order used by the
  planner and the graph inspector.

## Outputs

- `OutputConfig { videoBusId, audioBusId, capabilities, priority, group,
  failoverOrder, encoder, networkTarget }`.
- Capabilities: `supportsVideo`, `supportsAudio`, `supportsSeparateAudio`,
  `supportsMultipleChannels`.
- Priorities: `Critical / High / Medium / Low`. During resource pressure the
  planner protects Critical/High and sheds Low first (safe degradation).
- Output groups (`broadcast`, `stage`, ...) and failover order: if an output
  fails, the engine switches to the next in `failoverOrder` and publishes
  `OutputFailed` + `OutputFailover`.

## Resources

- Every node declares a `ResourceCost { gpu, cpu, vramMb, ramMb }`.
- `ProductionPlanner` analyzes the graph: total cost, per-node cost, the
  bottleneck dimension, and a feasibility verdict against a given budget.
  `PlanProduction()` publishes `ProductionPlanned`.
- `CheckProduction()` is the one-click production check (sources, buses,
  outputs, routing, resources) returning a `ProductionHealth` score.
- `Simulate()` initializes the graph in a dry-run mode — validation and
  planning without going live.

## Scenes, Health, Fallback

- Scene states: `Preview / Program / Standby / Disabled / Emergency`.
- Source states: `Connected / Stable / Degraded / Disconnected / Failed`.
- Source fallback: `SetSourceFallback(sourceId, fallbackId)`; a failed source
  auto-switches to its fallback and publishes `SourceFailed` + `SourceFallback`.
- `EmergencyMode()` is the one-button fallback: shed non-critical effects,
  mute selected sources, switch the emergency scene, keep the stream running.

## Control Signals & Macro Recording

- Control inputs (MIDI, OSC, keyboard, remote, API, plugin) are first-class
  `SignalType::Control` signals routed to control nodes; each arrives as a
  `ControlSignalReceived` event.
- `StartMacroRecording()` captures API actions; `StopMacroRecording()` returns
  the recorded sequence ready to be registered as a Phase 14 Flow macro.

## AI-Readiness

The engine exposes **validated production commands** only:

```text
AI → Production API → Validated Command → Production Engine
```

AI never mutates the graph directly. `ApplyCommand()` parses, validates
(node/bus/output existence, signal types, cycle safety), and only then applies
the change — the single safe mutation path.

## EventBus

Publishes `production.*`: `ProductionStarted`, `ProductionStopped`,
`BusChanged`, `BusSceneApplied`, `OutputChanged`, `OutputFailed`,
`OutputFailover`, `SourceFailed`, `SourceFallback`, `SceneStateChanged`,
`ProductionValidated`, `ProductionSnapshotSaved`, `ProductionRestored`,
`ProductionEmergency`, `VirtualSourceCreated`, `ControlSignalReceived`,
`ClockSyncChanged`, `ProductionPlanned`. Consumes display/media events for
output and source health.

## Files

- `modules/include/modules/production/ProductionTypes.hpp`
- `modules/include/modules/production/ProductionGraph.hpp` (+ `.cpp`)
- `modules/include/modules/production/ProductionEngine.hpp` (+ `.cpp`)
- Tests: `tests/unit/tests_production.cpp` — `TestProductionGraph`,
  `TestProductionEngine` (suite `production`).
- Boot: Kernel step 27 (`ProductionEngine`, after FlowEngine 26).
