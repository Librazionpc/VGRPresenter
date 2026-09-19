# 28 — Phase 16: Recording, Replay & Media Capture Engine

Status: **implemented** — see `docs/architecture/Conformance.md §31`.

## Objective

Make recording a **first-class output consumer of the Phase 15 production
graph** — not a separate rendering/audio pipeline. Anything that exists as a
valid signal in the graph (source, bus, virtual source, scene, program, output)
can be recorded independently or as part of a production output, with its own
bus selection, tap point, format, quality, resource policy and lifecycle.

```text
Source → Bus → Production Graph → Display | Stream | Recording
```

Recording, replay and capture consume the graph; they never rebuild it.

## Core Model

- **Record any signal.** A recording attaches to a graph node via
  `StartRecording(profileId, nodeId, tapPoint)`. Valid targets: sources
  (ISO), buses (bus recording), scenes, virtual sources, and outputs
  (output-specific recording — TV / Stage / Stream independently).
- **Tap points** (user brief §8-§10): `PreProcessing` (RAW/ISO), `PostProcessing`
  (program), `PreOutput` (master, before output limits), `PostOutput` (stream
  archive — preserve exactly what was sent).
- **Profiles** (`RecordingProfile`): container, video/audio codec, resolution,
  FPS, bitrate, audio channels, encoder preference, quality preset, segment
  duration, storage policy, priority. Templates group profiles (Sunday Service
  = Master + ISO 1..3 + Program Audio + Stream Archive).
- **Quality presets** (user brief §14): Proxy / Low / Standard / High /
  Broadcast / Master / Lossless / Custom → translated to bitrate/fps.
- **Formats & codecs are abstractions.** `IRecordingContainer` (mkv/mp4/mov/
  webm/wav/flac), `IVideoEncoder` (H.264/HEVC/AV1/VP9), `IAudioEncoder`
  (PCM/AAC/Opus/FLAC) are registry-based — new formats/codecs are registered,
  never added to the engine core.
- **Encoder selection.** `DetectEncoders()` consults the ConfigurationManager
  (`recording.encoders`) for available hardware encoders (NVENC/AMF/QuickSync)
  and always includes Software. `SelectEncoder(profile)` picks the best
  available (hardware preferred, user override via `SetPreferredEncoder`).

## Recording Lifecycle & Queue

States: `Preparing → Recording ⇄ Paused → Finalizing → Completed`, plus
`Failed`, `Recovering`, `Archived`. The queue is queryable per state.

- **Segmented recording**: `segmentDurationMs > 0` closes the current segment
  and opens the next (`SegmentCreated`), treating the whole as one logical
  recording.
- **Markers**: `AddMarker(recordingId, label)` stamps `{timeMs, label}` — the
  searchable timeline (Sermon Started, Prayer, Worship...).
- **Metadata & semantic metadata**: production, date, time, duration,
  resolution, fps, codecs, sources/scenes/outputs, bus, plus semantic fields
  (song, scripture, speaker, event) for content search.
- **Journal & crash recovery**: every lifecycle transition appends a
  `JournalEntry` (started / source attached / segment created / encoder
  initialized / marker / segment closed / finalized). `Recover()` finalizes
  any recording left in Preparing/Recording and publishes `RecordingRecovered`
  — a 3-hour recording is not lost by a crash at minute 179.
- **Scheduling**: `ScheduleStart`/`ScheduleStop` (master-clock based, driven
  by `Tick`) and `Poll()` for the automation/scheduler integration.

## Replay (Instant Replay)

- `CreateReplayBuffer(nodeId, capacityMs)` starts a rolling buffer (10s/30s/
  60s/5min/custom) — `ReplayBufferReady` fires when it is full.
- `CreateReplay(nodeId, mode)` captures the buffer (Normal / SlowMotion /
  Fast / Reverse) and **registers the replay as a virtual video source in the
  production graph** (`ReplayCreated`) — the replay can then be routed into a
  scene and program like any other source, with independent audio.

## Capture

- `ICaptureSource` registry (camera, capture card, screen, NDI, network,
  audio interface, system audio, virtual device). `RegisterCaptureSource`
  adds providers without engine changes.
- Hot-plug: `ConnectCaptureDevice` creates a matching Source node in the
  production graph and publishes `CaptureDeviceConnected`;
  `DisconnectCaptureDevice` removes it and publishes `CaptureDeviceDisconnected`
  — devices never require an application restart.

## Storage & Resources

- `StorageInfo(path)` uses filesystem capacity + profile bitrate to estimate
  hours remaining and warns before starting (e.g. 420 GB free, 12 GB/hour →
  35 hours).
- **Automatic storage protection** (configurable thresholds): >30% normal,
  <20% warning, <10% stop Optional recordings, <5% protect production only —
  via `SetStoragePolicy(level, action)`.
- **Resource priority**: `Critical (Program) / High (Streaming) / Medium (Master) / Low (ISO) / Optional (Preview)`. Under encoder/disk pressure the
  engine sheds Optional first and protects Critical — a secondary recording can
  never destroy the live production.
- **Health monitoring**: encoder load, dropped frames, disk health, audio
  health, bitrate; `UpdateEncoderLoad`/`UpdateDroppedFrames` drive
  `EncoderOverload`/`DroppedFramesDetected` events.
- **Security**: no credentials are stored by the engine; network/cloud targets
  are plain references resolved through the secure configuration mechanism.

## EventBus

Publishes `recording.*`: `RecordingStarted`, `RecordingStopped`,
`RecordingPaused`, `RecordingResumed`, `RecordingFailed`, `RecordingRecovered`,
`SegmentCreated`, `DiskSpaceWarning`, `EncoderOverload`, `DroppedFramesDetected`,
`ReplayBufferReady`, `ReplayCreated`, `CaptureDeviceConnected`,
`CaptureDeviceDisconnected`.

## Automation & .vgr

- Recording is controllable through the Phase 14 FlowEngine: consumers register
  `start_recording` / `stop_recording` / `add_marker` actions against the
  RecordingEngine API (shown in the CLI). Recording never touches encoders
  directly from the UI.
- Recording profiles are configuration documents (`.vgr`-ready); media files
  remain external by default.

## Files

- `modules/include/modules/recording/RecordingTypes.hpp`
- `modules/include/modules/recording/RecordingPlugin.hpp` (interfaces)
- `modules/include/modules/recording/RecordingEngine.hpp` (+ `.cpp`)
- Tests: `tests/unit/tests_recording.cpp` — `TestRecordingEngine` (suite
  `recording`).
- Boot: Kernel step 28 (`RecordingEngine`, after ProductionEngine 27).
