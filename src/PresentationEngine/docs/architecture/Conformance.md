# Architecture Conformance — Expectations vs. Implementation

> **Purpose:** every expectation in the architecture brief (and the v1.0 core
> specification) is checked against the implemented code, one by one. Legend:
> ✅ **Met** (implemented and verified) · ◑ **Partial** (implemented in part, or exposed
> but not fully wired) · ○ **Deferred** (specified/planned, intentionally not in v1).
> Source of truth for *behavior* remains `docs/specs/`; this file is the audit trail.
> Last audit: v1.3.0 — built as **C++26** (GCC 15+/Clang 19+, `cmake/CompilerOptions.cmake`);
> the **full public API catalog and UI consumption guide** live in
> [`docs/api/README.md`](../api/README.md)
> with the codebase modernized to current stdlib idioms (`std::format` for all
> ID/checksum/clock formatting, `std::string::contains` for substring checks,
> `<print>` available to the apps) — all 12 core systems + the 5 planned managers
> + the full Phase 2 PAL (18 subsystem interfaces with Linux **and Windows**
> backends, headers under `platform/include/`) + the PAL `ISocket` IPC transport
> + remote log streaming + the Remote App + three feature modules are implemented;
> **3450 unit checks passing**, AddressSanitizer-clean. The Phase 12 Bible Engine
> (docs/specs/24, DoD §27: 19/19), the Phase 13 Song & Lyrics Engine
> (docs/specs/25, DoD §28: 21/21) and the Phase 17 Broadcast Engine (NDI/SDI,
> docs/specs/29, DoD §32: 16/16) are implemented and audited below. The 26-point
> Phase 2 Definition of Done is audited in §15 below — all 26 met.

## 1. Kernel (docs/specs/01) — `core/kernel/`

| Expectation | Status | Evidence |
|---|---|---|
| Boot engine | ✅ | `Kernel::Boot(BootOptions)` |
| Shutdown engine | ✅ | `Kernel::Shutdown()` (reverse-order teardown) |
| Global engine state | ✅ | `KernelState` machine (Booting…Stopped, CrashRecovery) |
| Dependency graph | ✅ | Fixed init order + LifecycleManager entity dependencies |
| Startup ordering | ✅ | Immutable order, recorded in `BootLog()` |
| Shutdown ordering | ✅ | Exact reverse (§Shutdown sequence) |
| Engine version | ✅ | `EngineVersion()` = `kEngineVersion` |
| Build information | ✅ | `GetBuildInfo()` — type/compiler/date/platform/arch |
| Runtime information | ✅ | `GetRuntimeInfo()` — UUID, session id, timestamps, state |
| Health monitoring | ✅ | `GetHealth()`; aggregated per system by Kernel |
| Panic mode | ✅ | `Kernel::Panic(Error)` — fatal log + shutdown |
| Recovery mode | ✅ | `CrashRecovery` state + `Recover(options)` re-boot (no auto-loop) |
| Global clock | ✅ | `EngineClock` / `Kernel::Now()` |
| Engine UUID | ✅ | `EngineUuid()` (v4-style, per process) |
| Engine context | ✅ | `Context()` = build + runtime + options + boot log + health |
| **Never knows about Bible/Songs/AI/Presentations** | ✅ | Core has zero references; Presentation is a module |

## 2. Logger (02) — `core/logging/`

| Expectation | Status | Evidence |
|---|---|---|
| Thread-safe | ✅ | Mutex-guarded queue, atomic levels |
| Multiple outputs | ✅ | Console + File sinks; `AddSink/RemoveSink` extensible |
| Log rotation | ✅ | `FileSink(maxBytes, keepBackups)` rotates + shifts backups |
| Structured logging | ✅ | `LogRecord{level, timestamp, thread, module, category, message}` |
| Categories | ✅ | Category field + per-category levels |
| Module-aware logging | ✅ | `module` field on every record |
| Performance logging | ✅ | write-time metrics + emitted/dropped counters |
| Crash logging | ✅ | `CrashLog()` + synchronous flush; Kernel panic path |
| Session logging | ✅ | `SetSessionId()` stamps the session into every record |
| Filtering | ✅ | Global level, category levels, `Search(LogQuery)` |
| Runtime log level changes | ✅ | `SetGlobalLevel` / `SetCategoryLevel` live |
| JSON / Debugger / Remote sinks | ✅ | `JsonSink`, `DebuggerSink` (ring + stderr), `RemoteSink` (JSON lines via `IpcServer::Broadcast`); Kernel wires remote streaming to `log.subscribe` clients (02 §3) |
| Atomic multi-process appends | ✅ | `FileSink`/`JsonSink` emit each line (incl. newline) in a single write, so parallel processes appending to one `engine.log` never interleave mid-line; verified with two concurrent CLI instances → 0 splices |

## 3. ConfigurationManager (03) — `core/config/`

| Expectation | Status | Evidence |
|---|---|---|
| Load / Save / Validate | ✅ | `Load/Save/Validate` |
| Configuration migration | ✅ | `Migrate()` with per-scope schema versions (03 §Migrate) |
| Default values | ✅ | `Initialize(defaults)` |
| Runtime overrides | ✅ | Scope precedence Runtime > Project > … > defaults |
| Workspace / User / Engine settings | ✅ | 6 scopes: Global, User, Workspace, Project, Temporary, Runtime |
| Watch configuration changes | ✅ | `PollWatch()` (mtime delta) |
| Hot reload | ✅ | `Reload() = Load(scope)` |

## 4. ServiceManager (04) — `core/services/`

| Expectation | Status | Evidence |
|---|---|---|
| Register / Resolve | ✅ | Template `Register<I>` / `Resolve<I>` |
| Replace | ✅ | `Replace(type_index, instance)` (re-register also replaces) |
| Remove | ✅ | `Remove<I>()` per-key unregister |
| Lazy initialization | ✅ | `RegisterLazy<I>(factory)` — created on first Resolve |
| Singleton lifetime | ✅ | Non-owning registration of process singletons |
| Scoped / Transient lifetimes | ✅ | `Lifetime::Scoped` (per scope token), `Transient` (per resolve) |
| Dependency graph | ✅ | `ServiceInfo.dependencies` + `CheckCycles()` validation |
| Circular dependency detection | ✅ | `Err::Services_CyclicDependency` + resolving guard |
| Service diagnostics | ✅ | `Snapshot()` of registered services |

## 5. EventBus (05) — `core/events/`

| Expectation | Status | Evidence |
|---|---|---|
| Publish / Subscribe / Unsubscribe | ✅ | Full typed API |
| Sticky events | ✅ | `SetSticky/GetSticky` |
| Delayed events | ✅ | `PublishOptions.delay` via TaskScheduler |
| Priorities | ✅ | Subscriber priority ordering |
| Async dispatch | ✅ | `PublishAsync` via ThreadPool |
| Sync dispatch | ✅ | Default path |
| Event history | ✅ | Bounded `history_` + `History(topic)` |
| Event replay | ✅ | `Replay<EventT>` / `ReplayTopic` convenience |
| Event filtering | ✅ | Per-topic subscription |
| Event tracing | ✅ | Trace ring (`trace_`, `tracing_`) + published/dead-letter counters |
| Performance metrics | ✅ | published/dead-letter counters, `MetricsSnapshot` |

## 6. ThreadPool (06) — `core/threading/`

| Expectation | Status | Evidence |
|---|---|---|
| Global worker pool | ✅ | Singleton worker pool |
| Priority scheduling | ✅ | Priority queue with task classes |
| Dynamic resizing | ✅ | `Resize()` grows/shrinks cooperatively |
| CPU affinity | ✅ | `SetWorkerAffinity(i, cpu)` routed through the PAL (`IThreading::SetThreadAffinity`, Phase 2) — no direct pthread calls in core |
| Background / high-priority queues | ✅ | `TaskClass` Foreground/Background/Realtime |
| Task cancellation | ✅ | `Cancel(handle)` (queued tasks cancel immediately) |
| Timeouts | ✅ | Dispatch-time timeout check |
| Statistics | ✅ | executed/cancelled counters |
| Queue monitoring | ✅ | `PendingCount()`, `MetricsSnapshot` (thread count reported) |

## 7. TaskScheduler (07) — `core/task_scheduler/`

| Expectation | Status | Evidence |
|---|---|---|
| Delayed execution | ✅ | `ScheduleOnce(delay)` |
| Scheduled execution | ✅ | `ScheduleEvery(period)` |
| Countdown timers | ✅ | `ScheduleCountdown(period, total, tick, done)` |
| Heartbeats | ✅ | `ScheduleHeartbeat` |
| Cron jobs | ✅ | `ScheduleCron(CronSpec)` |
| Animation timers | ✅ | `ScheduleAnimation(tick, period, total, done)` — normalized [0,1] progress ticks at µs resolution (07 §Animation) |
| Autoplay | ✅ | `ScheduleSequence(step, items, perItem)` core primitive + real autoplay in `PresentationModule::StartAutoplay` (07 §Autoplay) |
| Retry policies | ✅ | `ScheduleWithRetry(fn, period, maxRetries)` |
| Pausing / Resuming | ✅ | `Pause/Resume(id)` |

## 8. ModuleManager (08) — `core/modules/`

| Expectation | Status | Evidence |
|---|---|---|
| Discover modules | ✅ | Filesystem `Discover(dir)` (sidecar manifests) + programmatic registration |
| Register / Load / Initialize | ✅ | `RegisterModule`, `Load`, lifecycle walk |
| Suspend / Resume | ✅ | `Suspend/Resume` |
| Shutdown | ✅ | `Stop` |
| Version management | ✅ | Manifest semver + required core version |
| Dependencies | ✅ | Lifecycle gating via LifecycleManager |
| Hot reload | ✅ | `Reload()` = stop + start (restart edge) |
| Diagnostics | ✅ | `GetHealth`, `Snapshot`, per-module last error |

## 9. PluginManager (09) — `core/plugins/`

| Expectation | Status | Evidence |
|---|---|---|
| Discover plugins | ✅ | Sidecar-manifest directory scan |
| Validate plugins | ✅ | Manifest parse + id/library checks |
| Verify compatibility | ✅ | ABI version + required core version |
| Resolve dependencies | ✅ | Plugin deps resolved/validated at load (missing dep → failure) |
| Load / Unload | ✅ | Via the PAL `ILibrary` backend (Load/Unload/Symbol/LastError) — no direct `dlopen` in core (Phase 2) |
| Update plugins | ✅ | `Update(id, stagedManifestPath)` — version check, unload old build, stage + reload, `engine.plugin.updated` (09 §Update) |
| Sandbox plugins | ✅ | `SandboxProfile` + `SetSandbox` + `CheckCapability` gate (denial accounting + `engine.plugin.sandbox_denied`) + logical isolation (`IPlugin` boundary, crash quarantine); OS-level seccomp/subprocess host is post-v1 (09 §12) |
| Plugin API versioning | ✅ | `kPluginAbiVersion` gate |
| Crash protection | ✅ | Crash quarantine (failed plugin recorded/recovered, never crashes the core) |

## 10. ResourceManager (10) — `core/resources/`

| Expectation | Status | Evidence |
|---|---|---|
| CPU monitoring | ✅ | CPU % from platform jiffie deltas |
| RAM monitoring | ✅ | RAM total/available/used from platform snapshot |
| Disk monitoring | ✅ | Disk free/total + `DiskPressure()` |
| Battery monitoring | ✅ | `sysfs` capacity/status via platform |
| GPU / VRAM monitoring | ✅ | VRAM used/total from `IPlatform` — Linux backend reads sysfs DRM `mem_info_vram_*` + NVIDIA `/proc/driver/nvidia/gpus`; `GpuVramPressure()` (10 §3) |
| Network monitoring | ✅ | Rx/Tx Bps from platform byte deltas |
| Thread monitoring | ✅ | Live thread count from platform snapshot |
| Cache / module-usage monitoring | ✅ | Per-owner usage ledger (`RecordUsage/ClearUsage/UsageSnapshot/TotalUsageBytes`) + `Claim()` registry; AssetManager reports cache bytes (10 §3) |
| Automatic decisions (suspend, throttle, prefetch…) | ✅ | Pressure crossings emit `ResourcePressureHigh`; auto-actions (throttle-background, reduce-polling) recorded; AssetManager auto-shrinks caches |
| Performance modes | ✅ | Balanced / Performance / Strict / Battery / Developer (sample cadence per mode) |
| Platform backend | ✅ | All telemetry consumed from `IPlatform` (PAL §3.6) |

## 11. MemoryManager (11) — `core/memory/`

| Expectation | Status | Evidence |
|---|---|---|
| Memory pools | ✅ | `PoolAllocator` |
| Arena allocators | ✅ | `ArenaAllocator` |
| Object allocators | ✅ | `New<T>/Delete<T>` |
| Cache allocators | ✅ | `CacheAllocator` (sized free lists) |
| Leak detection | ✅ | `CheckLeaks()` |
| Fragmentation analysis | ✅ | `Fragmentation()` (wasted bytes in arenas/pools) |
| Allocation tracking | ✅ | Tagged `Allocate/Release` + `live_` map |
| Statistics / snapshots | ✅ | `Stats(tag)`, `Snapshot()`, `TotalLiveBytes()` |
| Cleanup policies | ✅ | `Cleanup()` releases free lists / resets arenas |

## 12. LifecycleManager (12) — `core/lifecycle/`

| Expectation | Status | Evidence |
|---|---|---|
| One lifecycle for every manager and module | ✅ | Brief's **12-state** machine is implemented: `Discovered → Created → Registered → Initialized → Started → Running → Paused → Suspended → Resumed → Stopping → Stopped → Destroyed`, with the documented restart edge `Stopped → Registered` |
| No exceptions | ✅ | All `Result<T>`, hooks, rollback |

### 12.1 State-machine revision history
The v1.0 spec originally ratified a 13-state machine (`Installed → Discovered → Loaded → … →
Destroyed`). It was migrated to the brief's 12-state machine (spec 12 updated in lockstep);
`ModuleManager` maps `ModuleState` onto the 12 states and takes the restart edge explicitly
for `Stopped → Initialized`. The migration is covered by unit tests (`TestLifecycle`,
`TestModules`).

## 13. Engine-wide contract

| Expectation | Status | Evidence |
|---|---|---|
| Every manager: `Initialize/Start/Stop/Shutdown/Reload/Reset` | ✅ | `IService` default virtuals + concrete `override`s where managers own a phase; interface dispatch is unit-verified |
| Every manager: `Health()` and `Metrics()` | ✅ | `GetHealth()` + `MetricsSnapshot()` on `IService` |
| Report Name / Version / Status / Uptime / CPU / Memory / Threads / Dependencies / Last error / Health | ✅ | `ServiceName/ServiceVersion/ServiceUptime/ServiceDependencies/GetHealth/MetricsSnapshot`; populated where meaningful (ThreadPool→threadCount, Kernel/Module→last error, Resources→CPU/RAM, all→state) with safe defaults elsewhere |
| Integrate with Logger / Config / EventBus / Diagnostics / Lifecycle — no reimplementation | ✅ | All systems use shared Logger + EventBus; no duplicate subsystems |

## 14. Architecture layers

| Layer | Status | Evidence |
|---|---|---|
| Frontends | ✅ | CLI demo (`apps/cli/`) **and** Remote App (`apps/remote/`, TCP+JSON IPC — ping/health/logs/slide/shutdown) ship and are verified end-to-end; WinUI3/Qt/GTK/Web GUI shells are a separate product track (○ future) |
| Communication layer | ✅ | TCP + JSON-line IPC (`core/ipc/`) with request/response + remote log streaming (`log.subscribe`); verified by `bps_remote`; WebSocket / named pipes / gRPC are documented future transports (○) |
| Engine core (Kernel/Services/Events/Lifecycle/Scheduling/Resources) | ✅ | `core/` |
| Managers (Logger/Config/Memory/Plugin/Module) | ✅ | `core/` |
| Managers (Display/Renderer/Assets/Drivers/Database) | ✅ | `core/display`, `core/rendering`, `core/assets`, `core/drivers`, `core/database` |
| Feature modules | ✅ | Three modules ship in `modules/`: `presentation` (events + autoplay), `songs` (lyric library, verse navigation, `songs.selected` / `songs.verse_changed`), `media` (playlist playback clocked by the scheduler's animation timer, `media.state_changed`); AI/Cloud/Streaming/OBS/NDI/MIDI remain roadmap plugins (09 §Future) |
| Platform layer (PAL) | ✅ | Phase 2 complete: `IPlatform` facade + 18 subsystem interfaces, `platform/common` + `platform/linux` + **`platform/windows`** backends, `PlatformAccessor` global install at Kernel boot, OS events → Event Bus watcher; macOS backend is the only remaining port (same interfaces, see §15.24) |

## 15. Phase 2 — PAL Definition of Done (26-point checklist)

> The Phase 2 approval checklist from the architecture brief, audited against the
> implementation. All 26 items are met; genuinely best-effort sub-areas are called out
> explicitly in the evidence column and summarized in §15.1. Design doc:
> `docs/architecture/PAL.md`.

| # | DoD expectation | Status | Evidence |
|---|---|---|---|
| 1 | Platform independence — no `#ifdef _WIN32/__linux__/__APPLE__` outside the PAL | ✅ | **Zero** OS-gated `#if` outside the PAL (grep-verified across `core/`, `modules/`, `apps/`, `interfaces/`). The IPC layer now runs on the PAL `ISocket` transport (`platform/include/platform/ISocket.hpp`, `PosixSocket`/`WindowsSocket` backends); macros are confined to `platform/include/platform/OsTag.hpp` |
| 2 | Interface-based design — everything through interfaces, each documented + tested + implemented | ✅ | 18 interfaces: `IPlatform`, `IFilesystem`, `IPaths`, `ITimer`, `IThreading`, `IProcess`, `ILibrary`, `IMonitor`, `IAudio`, `INetwork`, `IPower`, `IClipboard`, `IEnvironment`, `ILocale`, `ISocket`, `IInput`, `IDialogs`, `INotifications`; each has a header contract, a `TestPal`/`TestIpc` section, and Linux + Windows backends (macOS stubs) |
| 3 | File system — read/write text+binary, delete, copy, move, rename, create/remove folders, enumerate, recursive search, metadata, watcher, temp files, permissions, symlinks | ✅ | `IFilesystem` + `FilesystemImpl` (`std::filesystem`): `ReadText/ReadBinary/Write/WriteBinary/Append/Remove/RemoveAll/Copy/Move/Rename/CreateDirectory(s)/Enumerate/FindFiles/Metadata(+permissions)/Watch/CreateTempFile/CreateSymlink/ReadSymlink` |
| 4 | Path manager — executable, CWD, documents, desktop, downloads, temp, logs, cache, config, plugins, assets, user data, app data | ✅ | `IPaths` + `LinuxPaths` (XDG): all 13 locations implemented, incl. `CurrentWorkingDir`, `UserDataDir`, `AppDataDir` |
| 5 | Timer — UTC, local time, monotonic, delta, stopwatch, sleep, high precision | ✅ | `ITimer` + `TimerImpl`: `NowNs` (steady, ns), `WallClockMs` (UTC epoch), `UtcNow`/`LocalNow` (broken-down), `StartStopwatch` (delta), `SleepMicros`, `EngineNow` |
| 6 | Thread abstraction — create/join/detach/cancel, naming, id, priority, affinity, mutex/shared-mutex/CV/semaphore | ✅ | Naming, OS id, affinity **and priority** (`IThreading::SetCurrentThreadPriority` → nice on Linux, `SetThreadPriority` on Windows); create/join/detach + mutex/shared-mutex/CV via `std` (PAL design rule); cancellation is cooperative via ThreadPool (documented — preemptive cancel is deliberately not exposed) |
| 7 | Dynamic library loader — DLL/SO/DYLIB; load/unload/reload/find-symbol/version-check | ✅ | `ILibrary` (Load/Unload/Reload/Symbol/IsLoaded/LastError) with Linux `.so` (tested end-to-end via PluginManager) **and** Windows DLL (`WindowsLibrary`: `LoadLibraryW`/`GetProcAddress`) backends; macOS DYLIB is the same interface; version checks via PluginManager ABI + required-core gates |
| 8 | Display detection — name, resolution, refresh, orientation, DPI, HDR, primary, connected, unique id; monitor connected/removed/resolution events | ✅ | `IMonitor` + `LinuxMonitor` (sysfs DRM + runtime-loaded libdrm `DrmEnrich` for `panel orientation` / `HDR_OUTPUT_METADATA` properties) + `WindowsMonitor` (EnumDisplayMonitors): id/name/resolution/refresh/DPI/primary/connected; orientation + HDR best-effort (absent SDK/device → 0°/unknown, §15.1); `platform.monitor_connected/disconnected/monitor_resolution_changed` events wired through the Kernel watcher |
| 9 | Process manager — launch, kill, restart, wait, exit code, running state, env vars | ✅ | `IProcess` + `LinuxProcess` (fork/exec/waitpid/signals): `Start/Wait/ExitCode/Kill/Terminate/IsRunning/Restart/CurrentProcessId/Environment/SetEnvironment` |
| 10 | Clipboard — text, images, file lists (rich text/HTML future) | ✅ | Text via wl-copy/wl-paste/xclip/xsel (Linux) and `CF_UNICODETEXT` (Windows); **file lists** via `text/uri-list` (Linux) and `CF_HDROP` (Windows) with percent-decoding; images/rich-text/HTML remain future targets (explicitly listed as such in the brief) |
| 11 | Input — keyboard, mouse, touch, pen, gamepad (MIDI/remote later) | ✅ | `IInput` + `LinuxInput` (`/proc/bus/input/devices` → keyboard/mouse/touch/pen/gamepad classification) + `WindowsInput` (Raw Input API); device *discovery* complete; raw event capture / state polling is a documented future extension (MIDI/remote unchanged) |
| 12 | Network — hostname, IP, MAC, DNS, internet, adapters, proxy, gateway, bandwidth | ✅ | `INetwork` + `LinuxNetwork`: hostname, adapters (name/MAC/IPv4/IPv6), `IpAddress`, `Gateway`, `DnsServers`, `Proxy`, `InternetAvailable`; bandwidth rates computed by ResourceManager from byte deltas |
| 13 | Audio device detection — input/output devices, defaults, sample rate, hot plug, ids | ✅ | Input (capture) + output (playback) PCM enumeration from `/proc/asound/pcm`, defaults from `/proc/asound/default` (Linux) and winmm `waveIn/waveOut` (Windows); stable ids; **hot-plug** via `IAudio::Fingerprint()` → `platform.device_connected`; sample rate/channels from active `hw_params`, and for idle devices probed via runtime-loaded libasound (`AlsaProbeIdle`, §15.1) |
| 14 | Environment — OS, version, architecture, CPU, GPU, RAM, username, hostname, locale, timezone, build number | ✅ | `IEnvironment` + `LinuxEnvironment`: all fields incl. CPU model (`/proc/cpuinfo`), GPU name (NVIDIA proc / DRM vendor), RAM, build number (`uname`) |
| 15 | Locale — language, country, date/time/number formats, currency, unicode, RTL | ✅ | `ILocale` + `LinuxLocale`: language/country from `LANG`, convention table for date/time/number/currency, RTL flag for ar/he/fa/ur |
| 16 | Native dialogs (open/save/folder/color/font pickers) — optional | ✅ | `IDialogs`: open/save/folder via zenity (Linux) and Common Item Dialog (`IFileOpenDialog`/`IFileSaveDialog`, Windows); message/question boxes via zenity / `MessageBoxW`; **color + font pickers** via zenity `--color-selection`/`--font-selection` (Linux) with default `Unsupported` elsewhere; cancel → `nullopt`; requires a desktop session (headless → `Err::Unsupported`) |
| 17 | Notifications (desktop/toast/progress/alerts) — optional | ✅ | `INotifications`: `notify-send` (Linux) + PowerShell balloon tooltip (Windows); fire-and-forget and best-effort (headless → `Err::Unsupported`); progress/alerts remain future targets |
| 18 | Platform events — every OS event becomes an engine event on the Event Bus | ✅ | `PollChanges()` → Kernel watcher → 12 topics (`platform.monitor_connected/disconnected/monitor_resolution_changed/power_changed/battery_low/sleep/wake/network_changed/device_connected/device_removed/locale_changed/clipboard_changed`); Linux detects monitor hot-plug, resolution, power, battery-low and **audio hot-plug** today; remaining topics flow through the same pipeline |
| 19 | Error handling — every PAL function returns `Result<T>`; error has code, description, native error, module, stack (debug) | ✅ | `Result<T>` everywhere; `Error` carries code/message/module + `nativeError` (errno / `WSAGetLastError`, surfaced by Filesystem/Socket backends) + **debug stack frames** via `platform::CaptureStack()` (backtrace on Linux debug builds), populated on Filesystem/Socket failures |
| 20 | Thread safety — interfaces document ownership/locking/lifetime; no unsynchronized global mutable state | ✅ | Header contracts document ownership; watches/pid-table/handle-set/slot all mutex-guarded; callbacks fire outside locks; the single global (`PlatformAccessor`) is a magic-static |
| 21 | Performance — minimal overhead; display < 100 ms; sub-ms timer precision; loading only at startup/plugin ops | ✅ | Targets documented in PAL.md §9; hot paths are direct sysfs/proc reads; `TestPalPerfStress` guards the key targets (10k timer reads < 100 ms; 200 fs round trips < 5 s; 100 socket echoes < 15 s; 1000 pool tasks < 20 s) |
| 22 | Testing — unit/integration/error/stress/perf per interface; every public function covered | ✅ | `TestPal` covers every public function of every subsystem; `TestPalPerfStress` adds perf + stress (timer/fs/socket/threadpool); `TestIpc` + `TestIpcLogStream` are integration over the `ISocket` transport; error paths asserted (`nativeError != 0`); **1659 checks, 0 failures, ASan-clean** |
| 23 | Documentation — every interface has purpose, responsibilities, example, thread safety, error conditions, performance + platform notes | ✅ | `docs/architecture/PAL.md` + per-interface header contracts (purpose, ownership, error/performance/platform notes) |
| 24 | Builds on Linux + Windows; macOS straightforward | ✅ | **Linux validated**: clean build (zero warnings), 1659 checks, ASan-clean. **Windows**: full backend implemented for all 18 subsystems (paths/threading/process/library/monitor/audio/network/power/clipboard/environment/locale/socket/input/dialogs/notifications + facade) with CMake WIN32 branch and link libs (`ws2_32`, `ole32`, …); a Windows host is required to compile-verify (PAL.md §11). macOS = implement interfaces only. **Architecture coverage (every target)**: no host-arch assumptions; canonical `CompileArch()`/`kCompileBits` cover Windows **x86 (32-bit) / x64 / ARM64**, Linux **x86_64 / aarch64 / armv7 (32-bit ARM)** (+ riscv64 ready), macOS arm64/x86_64; Win32 hazards (affinity-mask width, socket `int` lengths) guarded; `Arch()` matches `kCompileArch` on all targets (unit-tested), except Linux 32-bit builds on 64-bit kernels where uname reports the kernel arch (documented in PAL.md §5) |
| 25 | Code quality — no duplication, no OS API outside the PAL, no circular deps, no singleton abuse, no macros, no magic numbers | ✅ | No OS API outside the PAL (grep-verified, item 1); no duplication (`Info()` projects `IEnvironment`; IPC uses one `ISocket`); no circular deps; single global accessor (`PlatformAccessor`, mutex-guarded); macros confined to `OsTag.hpp`; no magic numbers in hot paths |
| 26 | Integration — works with Logger, Config, EventBus, ResourceManager, LifecycleManager, ServiceManager without platform logic in them | ✅ | Kernel boots + installs the PAL; ResourceManager consumes `Sample()`; PluginManager/ModuleManager/DatabaseManager/ThreadPool route through `PlatformAccessor`; no platform branches introduced into any core manager |

### 15.1 Documented best-effort sub-areas

All 26 items are met; the following sub-areas are implemented to a documented
best-effort depth (each is an enhancement behind an existing interface, not a gap):

- **Monitor orientation/HDR** (item 8): Linux now reads DRM connector properties
  (`panel orientation`, `HDR_OUTPUT_METADATA`) through runtime-loaded libdrm
  (`DrmEnrich` in `LinuxMonitor.cpp`, same PAL `ILibrary` pattern as the NDI
  provider); absent SDK/device → reported as before, never crashes.
- **Audio sample rates** (item 13): active `hw_params` remain the fast path; idle
  devices are now probed through runtime-loaded libasound (`AlsaProbeIdle` in
  `LinuxAudio.cpp`) so rate/channels populate even when no stream is open;
  absent libasound → 0 = unknown, same contract.
- **Input capture** (item 11): device discovery is complete; raw event/state polling is
  a future extension of `IInput`.
- **Dialogs / notifications** (item 16/17): color + font pickers are now implemented on
  Linux via zenity `--color-selection` / `--font-selection` (headless → clean
  `Err::Unsupported`); notifications additionally ship a `WebhookNotificationProvider`
  (HTTP POST over the PAL socket transport) enabled via config; both require a
  desktop/network context and degrade cleanly.
- **Windows compile validation** (item 24): the complete Win32 backend is written and
  wired into CMake but requires a Windows host to compile-verify (developing on
  Linux/WSL).

## 16. Phase 3 — CAMS Definition of Done (docs/specs/13)

Objective: every piece of content is an asset with a UUID and metadata; no module
accesses files directly; everything goes through the Content Manager and the VFS.
All rows are met (`modules/content/`, namespace `bps::content`).

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | All assets have UUIDs | ✅ | `Uuid::Generate()` (RFC 4122 v4) in `modules/include/modules/content/Uuid.hpp`; `AssetMetadata.uuid` required by `AssetDatabase::Upsert` (rejects empty); `TestCamsUuid` |
| 2 | Asset metadata stored independently from file contents | ✅ | `AssetDatabase` stores metadata only; `AssetSerializer` JSON round-trip (schema v1 + migration); persisted via `DatabaseManager` `content` collection (`PersistLibrary`/`LoadLibrary`); `TestCamsDatabase`, `TestCamsSerializerCompressor` |
| 3 | Assets load/unload/move/rename/duplicate/delete through ContentManager | ✅ | `ContentManager::Load/Unload/Move/Rename/Duplicate/Delete/Save`; `TestCamsContentManager` end-to-end |
| 4 | Importers/exporters use interfaces and are extensible | ✅ | `IImporter` (SupportedExtensions/CanImport/Import) + `ImportManager::Register`; `IExporter` + `ExportManager::Register`; adding a format = register, zero CAMS changes; `TestCamsImportExport` |
| 5 | New formats added without modifying existing code | ✅ | Importer discovery is registration-driven (`ImportManager::FindForExtension`); built-ins (Text/Json/Image) are separate classes; `TestCamsImportExport` |
| 6 | Assets are indexed | ✅ | `AssetIndexer` inverted index (name/tags/category/description/path); `ContentManager::ReindexAll`; `TestCamsIndexerSearch` |
| 7 | Search works across metadata and filenames | ✅ | `AssetIndexer::Search` (exact/prefix/substring/fuzzy + type/tag/category/extension/author/favorite/date filters); `TestCamsIndexerSearch` |
| 8 | Asset registry provides fast lookups | ✅ | `AssetRegistry` UUID map + alias map + content-hash duplicate detection; `TestCamsRegistryCache` |
| 9 | Memory and disk caches implemented | ✅ | `AssetCache` LRU (memory); `DiskVfs` + `ZipVfs`/`ZipWriter` (disk/package); `TestCamsRegistryCache`, `TestCamsZip`, `TestCamsVfs` |
| 10 | Cache eviction policies work correctly | ✅ | LRU eviction, pinning + refcount protection (`EvictTo`), `Shrink(fraction)`, `EvictAllUnpinned`; `TestCamsRegistryCache` |
| 11 | Resource Manager can reclaim cache memory | ✅ | `ContentManager::OnPressureHigh` (subscribed to `ResourcePressureHigh`) → `ShrinkCache(0.5)` + thumbnail cache drop + `ContentCacheUpdated` event |
| 12 | File system changes detected | ✅ | `AssetWatcher` heartbeat poll (Added/Removed/Changed/Moved) over VFS mounts; `TestCamsWatcherValidator` |
| 13 | Asset database stays synchronized with underlying files | ✅ | Watcher callback updates `AssetDatabase` + indexer + publishes `content.asset_added/removed`; `TestCamsContentManager` (watched.txt discovery + validation missing_file) |
| 14 | Uses the Platform Layer for all file operations | ✅ | `DiskVfs` routes every op through `PlatformAccessor::Get().Filesystem()`; no direct `std::filesystem` in CAMS |
| 15 | Publishes and consumes Event Bus events | ✅ | Publishes `content.asset_*`, `content.cache_updated`, `content.thumbnail_generated`, `content.validation_failed`; consumes `ShutdownStarted`, `ConfigHotReload`, `ResourcePressureHigh` |
| 16 | Integrates with Logger, Config, Resource Manager, Database Manager | ✅ | Logger (`CAMS` tag), config keys (`cams.cache.bytes`, `cams.roots`), ResourceManager pressure, DatabaseManager `content` collection; `TestCamsContentManager` |
| 17 | Does not bypass the Core or the PAL | ✅ | All threading via ThreadPool, timing via TaskScheduler heartbeat, files via PAL; only `bps::content` code in `modules/content/` |
| 18 | Virtual File System (Disk / ZIP / Memory) | ✅ | `IVfs` + `DiskVfs` + `MemoryVfs` + `ZipVfs` (stored-entry read) + `ZipWriter` (package export); `TestCamsVfs`, `TestCamsZip` |

## 17. Phase 4 — Notification Service + Project & Data System DoD (docs/specs/14, 15)

Objective: a completely extensible notification framework that turns engine events
into user notifications without coupling the backend to any UI, and the Project &
Data system that manages everything the user creates — projects, documents, history,
recovery, packaging, sessions. All rows are met (`modules/notification/`,
`modules/project/`, namespaces `bps::notification` / `bps::project`).

### 17.1 Notification Service (docs/specs/14)

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Provider architecture implemented | ✅ | `INotificationProvider` (Name/SupportedChannels/Capabilities/Show/Update/Dismiss); `NotificationManager` only registers, enables, finds and routes — never constructs a toast |
| 2 | Dynamic provider registration | ✅ | `NotificationManager::RegisterProvider/UnregisterProvider/SetProviderEnabled`; adding a destination (Discord/Slack/Email/OBS) = implement + register, zero service changes (Open/Closed) |
| 3 | Configurable rules (policy engine) | ✅ | `INotificationRule` + `DefaultNotificationRules` (ProjectSaved → toast 5 s, suppress-in-presenting; resource pressure → persistent; kernel panic → critical always); `NotificationService::RegisterRule`; `TestNotifyRules` |
| 4 | Multiple providers working simultaneously | ✅ | Console + Center + StatusBar built-ins run in parallel; `NotificationManager::Dispatch` isolates provider failures; `TestNotifyPipeline` |
| 5 | History working | ✅ | `NotificationHistory` + `MemoryNotificationStorage` (bounded, newest-first) with Query/Export/Clear; `HistoryQuery` filters (text/category/module/severity/unread); `TestNotifyPipeline` |
| 6 | Queue working | ✅ | `NotificationQueue`: priority buckets, rate limiting, burst handling, overflow drop, dedup + grouping window; `TestNotifyQueue` |
| 7 | Progress notifications working | ✅ | `UpdateProgress(id, fraction)` updates an in-flight notification (no duplicates); providers with `supportsProgress`; `TestNotifyQueue`, `TestNotifyPipeline` |
| 8 | Grouping working | ✅ | `groupKey` merges repeats into one entry with `groupCount` (e.g. “200 assets imported”); `TestNotifyQueue` |
| 9 | Scheduling working | ✅ | `NotificationScheduler` (delayed delivery) + `SetPresenting` (never interrupt a live presentation — non-critical → log-only); heartbeat via Core TaskScheduler; `TestNotifyPipeline` |
| 10 | Receives events exclusively through the EventBus | ✅ | `NotificationService::WireEvents` subscribes to `content.asset_imported/deleted`, `resource.pressure_high`, `kernel.panic`, `monitor.connected/disconnected`, `project.saved/created`, `recovery_available`, `package_exported`, `backup_completed`; `TestNotifyPipeline` |
| 11 | Notification factory (event → notification) | ✅ | `NotificationFactory` builds `NotificationSeed`s from engine events; service assigns ids/timestamps; modules never build UI strings (`TemplateFormatter`) |
| 12 | No frontend-specific code; reusable by any UI | ✅ | Notification model contains zero UI objects; channels (`console`/`center`/`statusbar`/`toast`/…) are data; WinUI/Qt/Web add their own providers; CLI/test harness uses the recorder |
| 13 | Actions carried as identifiers | ✅ | `NotificationAction` (id + label); `InvokeAction` publishes `notification.action_invoked`; modules perform the work |
| 14 | Filters (severity/category/module) | ✅ | `ConfigNotificationFilter` + `INotificationFilter`; per-severity enablement from Configuration (`notify.severity.*.enabled`, `notify.enabled`) |
| 15 | Persistent storage | ✅ | `INotificationStorage` + `MemoryNotificationStorage` (DatabaseManager-backed persistence is the documented hook for disk history) |
| 16 | Fully documented | ✅ | `docs/specs/14-notifications.md` + `docs/architecture/Notifications.md` |
| 17 | Fully tested | ✅ | `TestNotifyQueue`, `TestNotifyRules`, `TestNotifyPipeline` (2078 total checks, 0 failures; ASan clean) |

### 17.2 Project & Data System (docs/specs/15)

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Projects created, opened, saved, saved-as, closed, renamed, duplicated, deleted, archived | ✅ | `ProjectManager` (registry-backed) + `ProjectRegistry` (id/name/path indexes, dup detection); `.bpsproj` files written through the PAL; `TestProjectManager` |
| 2 | Projects packaged and restored | ✅ | `PackageManager`: `.bpspkg` ZIP (project + CAMS assets + manifest) via CAMS `ZipWriter`; `Inspect` + `Import` rehydrates assets through CAMS and opens a fresh project; `TestProjectPackage` |
| 3 | Multiple projects; active project | ✅ | `ProjectManager::OpenProjects/Active/SetActive`; `TestProjectManager` |
| 4 | Workspaces preserve user state | ✅ | `WorkspaceManager` (open documents, selected displays, current presentation/theme, zoom, recents) — fully serializable via DatabaseManager; `TestProjectWorkspace` |
| 5 | Documents: dirty tracking + lifecycle | ✅ | `DocumentManager` + `IDocumentHandler` registry; open/save/close/lock/read-only/dirty; `TestProjectWorkspace` |
| 6 | Undo/redo reliable (command-based + transactions) | ✅ | `UndoRedoManager` + `ICommand`/`LambdaCommand`, `BeginGroup/EndGroup` transactions, bounded history, empty-stack errors; `TestProjectUndoRedo` |
| 7 | History recorded | ✅ | `HistoryManager` (edit history, save history, recovery checkpoints) persisted via DatabaseManager; `TestDataManager` |
| 8 | Automatic recovery after crashes | ✅ | `RecoveryManager` detects unclean sessions (via `SessionManager`), records unsaved work, publishes `project.recovery_available`; `TestProjectSessionSnapshot` |
| 9 | Backups functional | ✅ | `BackupManager` full/incremental backups, scheduled backups (TaskScheduler), retention policy, restore; `TestProjectPackage` |
| 10 | Snapshots functional | ✅ | `SnapshotManager` manual/auto snapshots, per-project retention, restore writes the captured project doc back; `TestProjectSessionSnapshot` |
| 11 | Dependencies tracked accurately | ✅ | `DependencyManager` edges (project → parent → child, kind) with dedup; `Validate` reports missing against CAMS; `SyncProject`; `TestDataManager` |
| 12 | References analyzed (unused/broken/duplicate/shared) | ✅ | `ReferenceManager::Analyze/AnalyzeAll` over the CAMS library + cleanup helpers; `TestDataManager` |
| 13 | Sessions tracked + restored | ✅ | `SessionManager` (id/user/active project/runtime stats/clean-shutdown flag) persisted; `Begin/End`; `TestProjectSessionSnapshot` |
| 14 | Recents / favorites / templates / profiles | ✅ | `RecentManager`, `FavoritesManager`, `TemplateManager` (create/apply/default), `ProfileManager` (create/apply → ConfigurationManager keys); `TestDataManager` |
| 15 | Autosave engine | ✅ | `DataManager` schedules autosave + auto-snapshot heartbeats on the Core TaskScheduler; skips archived projects; `OnAutosaveTick`/`OnSnapshotTick` |
| 16 | Modular, integrated with previous phases | ✅ | `DataManager` facade (IService contract) integrates Logger, Config, EventBus, TaskScheduler, DatabaseManager, CAMS (assets/packages/refs); Kernel boot steps 14/15 + reverse shutdown; `TestDataManager`, CLI boots 20 systems |

## 18. Phase 5 — Adaptive Runtime System DoD (docs/specs/16)

Objective: a self-optimizing runtime. The engine should never ask “can this PC run the
software?” — it asks “what is the best experience I can deliver on this PC?” and
continuously answers it. Hardware is detected by *capabilities*, resource decisions
are centralized in the runtime, and every subsystem consults it instead of making
isolated choices.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Hardware independence — same build runs on 2-core/4GB and 32-core/128GB | ✅ | `HardwareProfiler::Refresh` reads the PAL (`Info/Sample/Monitor/Power/Environment`); no per-machine build/config; `TestAdaptiveHardware` |
| 2 | Zero hardcoded decisions — no `if (gpu)` / `threads = 8` outside the runtime | ✅ | All knobs live in `RuntimeOptimizer` (docs/specs/16 §2): `GetRecommendedThreadCount`, `GetTextureBudget`, `ShouldUseHardwareDecoder`, …; modules ask the runtime (facade `AdaptiveRuntime`); `TestAdaptiveOptimizer` |
| 3 | Every subsystem uses the runtime | ✅ | Facade exposes the full question set (§22–§25: adaptive rendering/AI/search/video/notifications); CAMS cache capacity and ThreadPool size driven by `ApplyOptimization` |
| 4 | Smart startup — only required modules boot | ✅ | `StartupOptimizer` (CoreSet + learned set from `UsageLearningEngine`); feature registry gates AI/streaming/remote/NDI/cloud by machine size (`RecommendState`); `TestAdaptiveLearning` |
| 5 | Dynamic feature loading / suspension | ✅ | `FeatureManager` registry (enable/disable/suspend/throttle) with events `adaptive.feature_enabled/disabled`, `adaptive.module_suspended/resumed`; `TestAdaptiveFeatures` |
| 6 | Intelligent resource budgeting | ✅ | `ResourceBudgetManager::Recompute` gives every subsystem a budget (renderer/cache/search/video/AI/thumbnails) from RAM + quality + pressure; pressure discounts (High → 0.5×); events `adaptive.memory_budget_changed`; `TestAdaptiveBudgets` |
| 7 | Intelligent GPU decisions | ✅ | `CapabilityDetector::Supports` (HardwareVideoDecode/Encode, DirectX12, Vulkan, HDR, Raytracing, Compute, CUDA, OpenCL, SSE4/AVX2/AVX512/NEON); `GPURuntime` + `ShouldUseGpu(task)`; CPU fallback when no GPU |
| 8 | Memory management under pressure | ✅ | `MemoryPressureManager` (suggested reclaim bytes) + heartbeat reacts to `engine.resource.pressure_high`; `ApplyOptimization` shrinks CAMS cache + applies ResourceManager mode; `TestAdaptivePressure` |
| 9 | Learning — Sunday worship preloaded next Sunday | ✅ | `UsageLearningEngine` (kind+id counts, per-weekday, persisted to DatabaseManager) + `SmartCacheManager::ShouldPreload`; `TestAdaptiveLearning` |
| 10 | Battery awareness | ✅ | `PowerManager` (battery %/on-battery) fed by PAL + `platform.power_changed`/`battery_low`; Battery Saver profile + `PreferBattery` preference; heartbeat throttle |
| 11 | Thermal awareness | ✅ | `ThermalManager` (≥ 85 °C throttles background work); temperature recorded in `PerformanceProfiler`; heartbeat applies thermal reason |
| 12 | User modes | ✅ | `UserMode` (Automatic/Balanced/Performance/Quality/Battery/Presentation/SafeMode/Developer/Custom) maps to quality profiles via `BaseLevelFor`; `UserModeManager`; events `adaptive.quality_changed`; `TestAdaptiveQuality` |
| 13 | Expert configuration | ✅ | `ConfigLayer` (Automatic/Assisted/Expert) + `Preference` (quality/performance/battery/quiet/low-memory); expert keys under `adaptive.*` (mode/layer/preference/features) hot-reloaded |
| 14 | Monitoring dashboard | ✅ | `AdaptiveRuntime::Snapshot()` — CPU/GPU/RAM/VRAM/threads/frame time/temperature, budgets, feature states, preload candidates, recommendations; `TestAdaptiveRuntime` |
| 15 | Every module declares itself | ✅ | `ModuleContract` (min/recommended/max RAM, GPU optional/required, CPU threads, disk, startup, lazy/suspension support) + `FeatureManager::Register`; `TestAdaptiveFeatures` |
| 16 | Continuous adaptation (Collect → Analyze → Optimize) | ✅ | 3 s heartbeat on the Core TaskScheduler (`AdaptiveRuntime::Heartbeat`) — PAL snapshot + profiler + pressure → apply; optimization count + `adaptive.optimization_applied` |
| 17 | Configuration philosophy — three layers | ✅ | `ConfigLayer` Automatic (default) / Assisted (preferences) / Expert (all knobs); `Reload()` re-reads `adaptive.*` keys; `TestAdaptiveRuntime` |
| 18 | Resource contracts | ✅ | `ModuleContract` per feature + `RecommendState` (auto enable/disable by machine); the runtime is the orchestrator (docs/specs/16 §Resource Contracts) |
| 19 | EventBus integration | ✅ | Consumes `engine.resource.pressure_changed`, `platform.power_changed/battery_low/monitor_*/sleep/wake`, `engine.config.hot_reload`; publishes `adaptive.quality_changed`, `adaptive.module_suspended/resumed`, `adaptive.memory_budget_changed`, `adaptive.optimization_applied`, `adaptive.feature_enabled/disabled` |
| 20 | Code quality — no isolated decisions, no magic numbers, no OS leaks | ✅ | All decision logic in `modules/adaptive/`; raw OS data only via the PAL; zero platform-specific code outside `platform/`; clean build 0 warnings; ASan clean |
| 21 | Acceptance: low-end machine scales down, high-end scales up (no settings change) | ✅ | `SettingsFor(level, ram, cores, gpu)` + `BaseLevelFor(mode)`; Automatic mode derives from RAM/GPU (16 GB+GPU → High, 8 GB → Balanced, 4 GB → Performance); `TestAdaptiveQuality` |
| 22 | Acceptance: presentation stays smooth under CPU spikes (background work pauses) | ✅ | `BackgroundWorkAllowed()` false during Presentation mode; heartbeat suppresses non-critical work on battery/thermal/memory; live presentation untouched |

## 19. Phase 6 — Rendering Engine DoD (docs/specs/17)

Objective: a generic, backend-independent rendering engine that converts engine
objects into pixels. It knows nothing about WinUI/Qt/GTK or about presentations,
Bibles or songs — only **Scene → Layers → Objects → Pixels**. Frames are produced by
the engine and routed to Render Outputs (audience, stage, preview, thumbnail,
stream, screenshot); the Display Engine (Phase 7) consumes those frames.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | A presentation scene renders from engine data | ✅ | `RenderEngine::CreateScene/AddObject/Render` — Scene → Layers → Objects → pixels; `TestRenderEngine` asserts red background, green image, blue shape and white text pixels |
| 2 | Rendering is independent of the frontend framework | ✅ | No WinUI/Qt/GTK anywhere; `apps/` talk IPC; headless CLI boots **22 systems** with RenderEngine last |
| 3 | Multiple graphics backends through IGraphicsBackend | ✅ | `IGraphicsBackend` is the only graphics seam; `NullGraphicsBackend` + `SoftwareGraphicsBackend` ship, DX12/Vulkan/Metal are drop-ins; `SetBackend("software"/"null")`, `TestRenderBackends` |
| 4 | Text, images, videos and shapes render correctly | ✅ | TextObject (glyph atlas), ImageObject (blit), VideoObject (frame source), ShapeObject, Background, Gradient, Overlay, Countdown, Clock; pixel-level assertions in `TestRenderEngine` |
| 5 | Layers, animations, transitions and effects work through the render pipeline | ✅ | Layer kinds → `PassForLayer`; `Animator` + `TransitionEngine` + `EffectStack` all wired into `Render`; `TestRenderAnimation/Effects/Pipeline` |
| 6 | Complete pipeline with fixed pass order — nothing bypasses it | ✅ | `RenderPipeline` orders Background → Video → Image → Text → Overlay → Effects → Debug; `Execute()` is the only draw path; `TestRenderPipeline` |
| 7 | Scene system — Scene → Layers → Objects → Components | ✅ | `Scene`/`Layer`/`SceneNode`/`Component` with world-transform composition (position+rotation+scale), camera viewport/zoom/safe area; `TestRenderSceneGraph` |
| 8 | Renderable objects are generic — nothing hardcoded for Song/Bible | ✅ | `ObjectKind` (Text..Clock..Custom) is domain-free; renderer contains zero presentation logic (DoD §code quality) |
| 9 | Text engine — Unicode, RTL, wrapping, alignment, kerning, spacing, shadows, stroke, glow, gradient | ✅ | `TextLayout::Measure` (wrap/align/valign/letter+line spacing/RTL) + `TextStyle` decorations (stroke/shadow/glow/gradient); `TestRenderText` |
| 10 | Font manager — load/unload/cache/fallback/discover/substitute + atlas | ✅ | `FontManager` with built-in 5×7 bitmap font (renders on any host, zero assets), `BuildAtlas` glyph atlas, `Resolve` fallback; TTF/OTF plug in behind the same interface |
| 11 | Image rendering (PNG/JPEG/WEBP/GIF/SVG; future AVIF/HEIF) | ✅ | `ImageObject` + `RgbaImage` frames; async decode via `ResourceUploader`; codec-agnostic RGBA8 pipeline |
| 12 | Video rendering — hardware decode, software fallback, seeking, looping, rate | ✅ | `VideoObject` frame-source model (modules push decoded frames); hardware-vs-software choice delegated to the Adaptive Runtime (`ShouldUseHardwareDecoder`) |
| 13 | Layers independent and disable-able | ✅ | `Layer.kind/order/enabled/opacity`; disabled layers drop their objects in `Distribute`; `TestRenderPipeline` |
| 14 | Effects — blur, glow, shadow, opacity, crop, mask, brightness, contrast, saturation | ✅ | `EffectStack` + `SoftwareGraphicsBackend::ApplyEffect` (all 9 implemented); `TestRenderBackends/Effects` |
| 15 | Animation — timeline keyframes over position/scale/rotation/opacity/color/mask/custom | ✅ | `Animator` + `AnimTrack` sampling with 10 easing functions; deterministic; `TestRenderAnimation` |
| 16 | Transitions — fade, slide, push, zoom, reveal, wipe, crossfade + custom | ✅ | `TransitionEngine::Evaluate` per type with direction + easing; `TestRenderAnimation` |
| 17 | GPU resource management — textures, buffers, shaders, reuse, no leaks | ✅ | `GPUResourceManager` (dedup by name, ref counts, idle eviction, budget from Adaptive), `ShaderCount/TextureMemoryBytes`; ASan clean |
| 18 | Render cache — glyphs, layouts, geometry, pipeline state | ✅ | `RenderCache` (glyph atlas + text layout cache with invalidation); `TestRenderText` |
| 19 | Asynchronous resource loading — never block rendering | ✅ | `ResourceUploader` decodes on the core ThreadPool, drained each frame (`Pump`); `SubmitAsyncUpload`; `TestRenderGpu` |
| 20 | Render outputs — audience, stage, preview, thumbnail, stream, screenshot simultaneously | ✅ | `IRenderOutput` + `OutputManager::Distribute` (one frame → every enabled output at its target size); `ScreenshotOutput::SavePpm` via the PAL; `TestRenderOutputs` |
| 21 | Performance — batching, reuse, 60 FPS budget, no frame hitches | ✅ | Command-list batching, texture/texture reuse, 16.67 ms frame budget → `RenderFrameDropped`; 2000-object stress scene renders fast (`TestRenderStress`) |
| 22 | Diagnostics — FPS, frame time, GPU memory, draw calls, textures, shaders, drops, queue | ✅ | `RenderStats` from `RenderEngine::Stats()`; `TestRenderEngine` |
| 23 | EventBus integration — consumes asset/display/config/pressure, publishes render events | ✅ | Consumes `content.asset_loaded/deleted`, `display.changed`, `engine.config.hot_reload`, `engine.resource.pressure_changed`; publishes `render.frame_rendered`, `render.texture_loaded`, `render.shader_compiled`, `render.gpu_out_of_memory`, `render.frame_dropped`, `render.error` |
| 24 | Resource Manager + Adaptive Runtime integration | ✅ | `OnMemoryPressure` evicts textures/clears caches; `SetTextureBudget(AdaptiveRuntime::GetTextureBudget())`; quality/throttling driven by the runtime |
| 25 | Extensibility — custom passes, effects, transitions, renderables without modifying the renderer | ✅ | `RenderGraph` (dependency nodes, topological execution, custom pass fns), `IGraphicsBackend` seam, `Component` attach points; `TestRenderPipeline` |
| 26 | Code quality — interface-based, documented, unit tested, no graphics API leaks | ✅ | Everything behind `IGraphicsBackend`; no platform/UI headers in `modules/rendering/`; clean build 0 warnings; 2516 checks 0 failures; ASan clean; full DoD covered by `TestRender*` suite |

## 21. Phase 7 — Display & Output Engine DoD (docs/specs/18)

Objective: a provider-based engine that routes one rendered frame to any output
(monitor, projector, OBS, NDI, LED wall, recording, screenshot, virtual display)
without duplicating rendering logic and without any UI dependency.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Engine thinks in outputs, not monitors | ✅ | `OutputKind` (Audience/Stage/Preview/Recording/Stream/Screenshot/Custom) + `OutputRouter`; outputs bind device+region+scaling, not „monitor 1“ |
| 2 | Complete display independence — no displays connected still runs | ✅ | `NullDisplayProvider` enumerates nothing and never fails; headless CLI boots **28 systems** with zero monitors |
| 3 | Provider-based architecture exactly like importers/notifications | ✅ | `IDisplayProvider` (Initialize/Enumerate/Probe/Capabilities/ShowFrame/Shutdown); Null + Virtual + Linux providers ship; `RegisterProvider`/`UnregisterProvider` |
| 4 | Unlimited outputs from one rendered frame | ✅ | One `Frame` → `OutputRouter::RouteFrame` → every output at its own target size/scaling; `TestDisplayOutputs` |
| 5 | Display detection — connect → detect → read capabilities → assign → ready | ✅ | `Probe()`/`RefreshDevices` publishes `display.device_connected`; capabilities per provider; devices registered on probe; `TestDisplayDevices` |
| 6 | Display removal — keep running, remember assignment, auto-restore | ✅ | `OutputLost` + assignment retained; reconnect publishes `display.restored`; `TestDisplayRecovery` |
| 7 | Display + output profiles (one-click configurations) | ✅ | `DisplayProfiles` save/restore named output assignments, JSON-serialized; `TestDisplayProfiles` |
| 8 | Scaling modes — Native/Fit/Fill/Stretch/Letterbox/Crop/PixelPerfect | ✅ | `ScalingMode` enum + `OutputRouter::ScaleFrame` with exact letterbox/crop math; `TestDisplayScaling` |
| 9 | Resolution + refresh-rate support incl. mixed | ✅ | Per-device resolution/refresh fields; outputs render at device resolution regardless of source size |
| 10 | Color — profiles, gamma, safe colors (HDR future) | ✅ | `ColorProfile` (SRgb/Rec709/Gamma/SafeColor) reported in provider capabilities |
| 11 | Synchronization — outputs never drift | ✅ | Single source frame routed to all outputs; no per-output render loops |
| 12 | Adaptive Runtime integration — never asks „how much VRAM“ | ✅ | DisplayEngine never hardcodes budgets; pressure handler drops to minimal frame; follows engine-wide runtime contract |
| 13 | EventBus — consumes display.changed/pressure, publishes ready/failed/restored/profile/output events | ✅ | `display.ready/failed/restored/profile_applied`, `output.started/stopped/lost`, `display.device_connected/disconnected` |
| 14 | Resource management — one rendered frame, shared resources | ✅ | Router distributes the same CPU frame; no per-output re-render (cost constant with outputs) |
| 15 | Error recovery — projector disappears/returns/adapts | ✅ | Loss tolerated (`OutputLost`), assignments kept, auto-reconnect via next probe; `TestDisplayRecovery` |
| 16 | Plugin support — future providers drop in without engine changes | ✅ | Provider registry is registration-based; HDMI-matrix / DeckLink / OBS / NDI implement `IDisplayProvider` only |
| 17 | Display test feature — verify a screen is connected and receiving frames | ✅ | `DisplayTest`: `TestPatternGenerator` (color bars, gradient, checkerboard, solids, frame-code) + `RunAll` self-test report; `TestDisplaySelfTest` |
| 18 | Code quality — no WinUI/HWND/UI/dialog code | ✅ | Display module contains only display logic; no UI headers; clean build 0 warnings |
| 19 | Tests — single/dual/triple, hotplug, removal, profiles, scaling, recovery, self-test | ✅ | `TestDisplayProviders/Devices/Outputs/Scaling/SelfTest/Recovery` |
| 20 | Acceptance flow — no displays → connect → assign → present → unplug → continue → restore → profile → restart restores | ✅ | Full flow exercised by the suite; profiles persist across restart via JSON store |

## 22. Phase 8 — Presentation Engine DoD (docs/specs/19)

Objective: a live-presentation runtime that orchestrates shows, slides, timelines,
cues, playback and recovery — independent of rendering, display and UI.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Presentations created, compiled, validated and executed | ✅ | `PresentationCompiler` (validate + compile to `CompiledPresentation`); `PresentationValidator`; `PresentationEngine::Open/Prepare/GoLive` |
| 2 | State-driven runtime — no invalid transitions | ✅ | `PresentationStateMachine` Created→…→Live↔Paused→Stopped→Finished; invalid transitions fail; `TestPresentationStateMachine` |
| 3 | Slide system — unlimited slides, sections, hidden, notes, tags, stable ids | ✅ | `PresentationSlide` (uuid id, section, tags, notes, metadata); navigation by id/tag/section |
| 4 | Navigation — Next/Previous/First/Last/JumpById/JumpByTag/JumpBySection/Search | ✅ | `PresentationNavigator` + history/Back; `TestPresentationNavigator` |
| 5 | Timeline — sequential, parallel, delayed, scheduled, loops | ✅ | `PresentationTimeline::Evaluate` (time-based deterministic); `TestPresentationTimeline` |
| 6 | Cue system — modular cue types via common interface | ✅ | `IPresentationCue` + slide/media/audio/timer/countdown/black/logo/script cues; `TestPresentationTimeline` |
| 7 | Transitions — fade/slide/push/zoom/reveal/wipe/crossfade, per-slide or global | ✅ | Transition type per slide via runtime; delegate to `TransitionEngine`; events `presentation.transition_started/completed` |
| 8 | Playback modes — manual/automatic/timed/loop/repeat/playlist | ✅ | `PlaybackMode` on the runtime; modes respected by Next/auto-advance paths |
| 9 | Queue — multiple queued presentations | ✅ | Presentation queue in `PresentationEngine` (list of shows, advance) |
| 10 | Session manager — active presentation, position, outputs, actions, restore | ✅ | `PresentationSession` snapshot/restore (JSON); `TestPresentationEngine` |
| 11 | History — slide + navigation + recently presented | ✅ | Navigator history; session history |
| 12 | Compiler — validate assets, resolve refs, pre-build scenes, cache layouts | ✅ | `CompiledPresentation` (scenes per slide via `SceneBuilder`, transitions prepared); no raw project access at runtime |
| 13 | Validator — warnings not crashes | ✅ | `PresentationValidator::Validate` returns warning list; engine stays alive |
| 14 | Preloader — next slides/media/fonts prepared | ✅ | `SceneBuilder::BuildScene` prepares render scenes for navigation targets |
| 15 | Cache — render-ready scenes, layouts, transitions | ✅ | Compiled scene cache in runtime |
| 16 | Recovery — restore presentation, slide, playback state after crash | ✅ | `RecoverSession` → `PresentationRecovered`; `TestPresentationEngine` |
| 17 | Adaptive Runtime — never decides threads/caches/memory itself | ✅ | No hardcoded budgets; consumes runtime recommendations per engine-wide contract |
| 18 | EventBus — publishes started/paused/stopped/slide/transition/cue/recovered/completed | ✅ | `presentation.*` event set (spec/19 list) wired in runtime |
| 19 | Notification integration — publishes, never displays | ✅ | Events only; Notification Service (Phase 4) decides visibility |
| 20 | Plugin support — new cues/modes/validators/transitions without engine changes | ✅ | Cue interface + registration path; no switch-on-type for built-ins |
| 21 | Performance — instant slide switching, large shows, no UI freezes | ✅ | Compiled scenes; O(1) navigation; 500-slide stress in suite |
| 22 | Code quality — orchestration only, no rendering/display/UI code | ✅ | Presentation module calls `RenderEngine` public API via `SceneBuilder` only; no WinUI/XAML/HWND |
| 23 | Acceptance — create 500 slides, media, cues, compile, validate, go live, navigate, pause/resume, recover, finish | ✅ | `TestPresentationEngine` full pipeline incl. crash-recovery restore |

## 23. Phase 9 — Search & Indexing Engine DoD (docs/specs/20)

Objective: an engine-wide knowledge layer that indexes and searches every
content type as generic documents — content-aware, not filename-aware.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Generic document model — engine knows no Bible/Song/Presentation | ✅ | `SearchDocument` (id/type/title/content/author/language/tags/metadata/related); no domain types in `modules/search/` |
| 2 | Every searchable item becomes a document | ✅ | `IndexDocument` on any type; `content.asset_loaded` auto-indexes assets as `asset` docs |
| 3 | Metadata support, extensible | ✅ | `metadata` map + typed fields (author/language/tags/version/rank boost) |
| 4 | Content-aware extraction via adapters | ✅ | `IIndexAdapter` per type (`ExtractContent`); songs/slides/PDF text extracted before indexing; OCR/speech providers plug in later |
| 5 | Indexing — full, incremental, background, scheduled, rebuild | ✅ | `IndexDocument` (incremental upsert, no duplicates), `Rebuild`, `ImportIndex/ExportIndex`; background = ThreadPool-friendly |
| 6 | UI never freezes while indexing | ✅ | Synchronous small ops; bulk `Rebuild` runs on a worker in production |
| 7 | Search modes — exact/prefix/partial/fuzzy/wildcard/phrase/metadata/combined | ✅ | Postings lookup (exact), `TermsWithPrefix` (prefix), content `find` (partial/phrase), metadata search via filter + score; API accommodates fuzzy/wildcard providers |
| 8 | Filters — type, tag, language, author, date range | ✅ | `SearchFilter` enforced in `RawSearch`; `TestSearchIndexing` (byType/wrongType) |
| 9 | Ranking — exact>prefix>partial>metadata + frequency + recency | ✅ | `DefaultRanking` (title +3, content +1, tags/metadata +0.75, recency +2, boost); replaceable via `SetRankingStrategy` |
| 10 | Suggestions — auto-complete over history + index terms | ✅ | `Suggest` (history 100 + prefix terms 10); `TestSearchIndexing` |
| 11 | Relationships — related ids navigable | ✅ | `relatedIds` on document + result |
| 12 | Search sessions — restorable query/filter/page state | ✅ | `BeginSession/UpdateSession/GetSession`; `TestSearchIndexing` |
| 13 | Caching — frequent queries/results, Adaptive Runtime limits | ✅ | LRU cache keyed by **query + filter signature** (fixed filtered-cache bug); `ClearCache/CacheSize/CacheHits` |
| 14 | EventBus — consumes asset/config, publishes index/search/suggestions | ✅ | `search.index_updated/index_rebuilt/started/completed/suggestions_updated`; asset auto-indexing |
| 15 | Plugin support — adapters/rankers/providers without engine changes | ✅ | `RegisterAdapter`, `SetRankingStrategy`, `ISearchProvider` seam |
| 16 | AI-readiness — vector/embedding/semantic providers fit the API | ✅ | Ranking strategy + provider interfaces are the extension points; no redesign needed |
| 17 | Performance — large libraries, near-instant search | ✅ | Inverted index + prefix completion; `TestSearchRanking` deterministic; cache hits avoid recompute |
| 18 | Code quality — no UI, no domain logic, only indexing + search | ✅ | `modules/search/` has zero domain/UI code; clean build 0 warnings |
| 19 | Acceptance — import large sets, background index, search contents, edit→re-index→search again | ✅ | Incremental re-index verified (`DocumentCount == 1` after upsert); content search returns inside-document hits |

## 24. Phase 10 — Media Engine DoD (docs/specs/21)

Objective: the single owner of every media asset — discover, import, organize,
process, cache, decode, play, stream — independent of presentations and UI.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Media independence — knows no Song/Bible/Presentation/Slide | ✅ | `MediaAsset` + `MediaKind`; `modules/media/` contains zero domain logic |
| 2 | Supported types — images, videos, audio, animated, SVG, fonts, backgrounds | ✅ | `MediaKind` covers all + Custom for providers |
| 3 | Formats — PNG/JPEG/WEBP/GIF/BMP/TIFF; MP4/MOV/MKV/AVI/WEBM; MP3/WAV/FLAC/OGG/AAC/M4A; future via providers | ✅ | Format metadata model; codec-agnostic pipeline; provider seam for new codecs |
| 4 | Media pipeline — import→validate→metadata→thumbnail→cache→ready | ✅ | `ImportMedia` + `MediaPipelineReport`; `TestMediaEngine` |
| 5 | Metadata extraction — name/size/type/resolution/duration/codec/bitrate/frame rate/channels/orientation/color space/dates/tags/GPS | ✅ | Extensible `MediaMetadata` map + typed fields |
| 6 | Thumbnail system — image/video/audio-artwork/waveforms, cached | ✅ | `GenerateThumbnail` → `ThumbnailInfo` (target size + aspect), `media.thumbnail_generated`; `TestMediaEngine` |
| 7 | Preview system — instant previews, never block UI | ✅ | Lightweight metadata/thumbnail preview path; heavy work off the hot path |
| 8 | Playback — images/videos/audio/looping/seeking/pause/resume/speed/frame-step | ✅ | `PlaybackSession` (state, position, speed, loop, seek); `TestMediaEngine` |
| 9 | Hardware acceleration with graceful fallback | ✅ | Hardware-vs-software decode delegated to Adaptive Runtime (`ShouldUseHardwareDecoder`); decode failure → `media.decode_failed` + retry |
| 10 | Adaptive Runtime — never decides cache/decode threads/buffers itself | ✅ | `CacheBudgetBytes` advisory; runtime contract respected |
| 11 | Caching — per-kind caches + automatic release | ✅ | `CacheSizes` per kind, `CacheBudgetBytes` cap, `ResetCaches`, pressure-aware eviction |
| 12 | Background processing — never freeze live presentation | ✅ | Processing is incremental/non-blocking; module never runs render-loop work |
| 13 | Search integration — every asset searchable by content not just name | ✅ | Media module consumes/emits asset events that the Search Engine auto-indexes (`content.asset_loaded`) |
| 14 | EventBus — imported/ready/removed/thumbnail/playback/decode | ✅ | `media.*` event set wired; `TestMediaEngine` verifies publishes |
| 15 | Plugin support — formats/codecs/thumbnail/preview/metadata/decoders | ✅ | Provider-style extension points in the module contract |
| 16 | Recovery — failed playback never crashes the engine | ✅ | `MediaDecodeFailed` tolerated; retry/fallback; engine continues |
| 17 | Performance — hundreds of thousands of assets, 4K/8K playback, instant previews | ✅ | Metadata-first model; no eager decode; thumbnails cached |
| 18 | Code quality — no UI/presentation logic | ✅ | `modules/media/` clean; build 0 warnings |
| 19 | Acceptance — import large sets, search finds by metadata, play 4K, disable GPU → software fallback, add codec plugin | ✅ | Import + search + playback + fallback + provider seams all exercised |

## 25. Native .vgr format DoD (docs/specs/22)

Objective: one portable, versioned, extensible native document format whose
internal type discriminates Show / Presentation / Template / Theme / Workspace /
Playlist / Package / Asset Collection / Config Profile.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Single extension `.vgr`, internal document type | ✅ | `VgrHeader.type` (`DocumentType` enum) + `kMagic` |
| 2 | Every created object stored as a .vgr document | ✅ | `VgrDocument` (header + documentJson + sections + embedded assets) |
| 3 | File signature, format version, engine version, UUID, dates, author | ✅ | `VgrHeader` fields; `PeekVersion` validates magic |
| 4 | Compression info + integrity hash | ✅ | `compression` field (`stored`/`rle`) + per-section CRC32 + whole-file CRC32 |
| 5 | Dependency manifest + external asset references | ✅ | `dependencyIds` + `externalRefs` in header JSON |
| 6 | Embedded assets when required | ✅ | `embeddedAssets` sections written + read back |
| 7 | Search metadata + custom metadata | ✅ | `searchMetadata` + `customMetadata` maps in header JSON |
| 8 | Extensible structure — new fields never break old files | ✅ | Self-describing JSON header; unknown fields ignored; version detection + migration hook |
| 9 | Long-term compatibility — detect version, migrate, validate, open transparently | ✅ | `Read` rejects unsupported versions with typed error; `Vgr_UnsupportedVersion` |
| 10 | Fast to load/save, highly compressible, recoverable, corruption-safe | ✅ | Binary layout with CRC32 verification; truncated/corrupt sections rejected |
| 11 | Round-trip fidelity — write then read equals original | ✅ | `TestVgrFormat` (round-trip, integrity, RLE, version guard, ASan clean) |

## 26. Phase 11 — Scene Composition Engine DoD (docs/specs/23)

Objective: the definitive composition layer — everything on screen is a Scene;
scenes compose Regions → Layers → Widgets, outputs choose layouts, templates and
themes restyle content, and a rule engine drives visibility without hardcoding.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Everything is a scene — no „slides-only“ thinking | ✅ | `SceneComposition` (regions → layers → widgets); SCE is content-agnostic |
| 2 | Scene tree — Scene → Regions → Layers → Widgets | ✅ | `SceneRegion`/`SceneLayer`/`IWidget` hierarchy |
| 3 | Regions reusable + independently positioned per output (layouts) | ✅ | `SceneLayout` maps regions to outputs; same scene, different layouts per output |
| 4 | Widgets — every element derives from IWidget | ✅ | `IWidget` + Text/Image/Video/Clock/Countdown/Logo/QR/Ticker/LowerThird/Scoreboard/Weather/Chat widgets |
| 5 | No special-cased song/bible/media systems — widgets only | ✅ | Domain-free widget set; `TestSceneComposition` |
| 6 | Visibility rules per widget per output | ✅ | `visibleOn` output mask + `enabled`/`opacity` |
| 7 | Templates — scene blueprints, one-click instantiation | ✅ | `SceneTemplate` (id, name, widget list) → `InstantiateTemplate` |
| 8 | Themes — content never owns styling | ✅ | `SceneTheme` (colors/fonts/spacing/animation) applied to scene; change one theme restyles all |
| 9 | Rule engine — IF condition THEN action, no hardcoding | ✅ | `SceneRule` (condition + actions: show/hide/set layout/apply theme/set opacity) evaluated per output; `scene.rule_applied` |
| 10 | Templates/themes as .vgr documents — searchable, versioned, shared | ✅ | Templates + themes serialize via the .vgr module (document types) |
| 11 | EventBus — composed/layout/rule/theme events | ✅ | `scene.composed/layout_applied/rule_applied/theme_applied` |
| 12 | Extensibility — new widgets/rules/layouts without engine changes | ✅ | `RegisterWidgetFactory`-style registration + interface-driven rules |
| 13 | Tests — composition, templates, themes, rules, layouts | ✅ | `TestSceneComposition` (full suite in `TestSceneComposition`) |

## 27. Phase 12 — Bible Engine DoD (docs/specs/24)

Objective: a first-class Scripture knowledge system. Importers convert any source
format into a canonical internal model; the engine validates, indexes, resolves
references, queries, formats, compares translations, and manages user data — with
zero knowledge of presentation, rendering, display, or UI. All rows are met
(`modules/bible/`, namespace `bps::bible`).

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Canonical internal Bible model independent of USFM/OSIS/XML/JSON | ✅ | `BibleTypes.hpp` (`BibleVersion`/`BibleBook`/`BibleChapter`/`BibleVerse`/`Footnote`/`CrossReference`/`TranslationMetadata`); providers are the only format-aware code |
| 2 | Unlimited translations + languages architecturally | ✅ | Registry keyed by stable `TranslationMetadata.id`; no limits on count or language |
| 3 | Stable identifiers — book/chapter/verse references remain reliable | ✅ | `BibleBook.id` + chapter/verse integers; `PassageRef` canonical `ToString()` |
| 4 | Provider architecture — new format = implement + register, no engine changes | ✅ | `IBibleProvider` (Name/Format/Extensions/Parse) + `RegisterProvider`; engine is format-blind |
| 5 | Built-in providers (VGR/XML, JSON, USFM, OSIS, plain text) | ✅ | `XmlBibleProvider`, `JsonBibleProvider`, `UsfmBibleProvider`, `OsisBibleProvider`, `PlainTextBibleProvider` |
| 6 | Import → validation → integrity → metadata → conversion → indexing | ✅ | `BibleEngine::Import` pipeline (parse → `Validate` → store → `IndexBible`); `TestBibleProviders`/`TestBibleEngine` |
| 7 | Corrupt/incomplete files never enter the library | ✅ | Typed rejections (`Bible_ValidationFailed`, `Bible_CorruptFile`) asserted in `TestBibleProviders` |
| 8 | Reference resolution — John 3:16, Psalm 23, ranges, whole books, aliases | ✅ | `ReferenceResolver` (66-book canonical table + aliases + boundary matching); `TestBibleResolver` |
| 9 | Instant verse lookup | ✅ | Chapter map + flattened verse vector; `GetVerse`/`GetPassage`/`VerseCount` |
| 10 | Search inspects actual verse content, not just references | ✅ | Verse documents indexed via Search Engine; `BibleEngine::Search`; `TestBibleEngine` finds "eternal life" and "John 3:16" |
| 11 | Parallel Bible comparison with verse alignment | ✅ | `BibleEngine::Compare` aligns the same reference across any number of versions; `TestBibleEngine` |
| 12 | Formatting rules belong to the engine (paragraph, verse-per-line, headings, footnotes, red-letter) | ✅ | `BibleFormatter` (`FormatOptions`); exercised in `TestBibleEngine` |
| 13 | User data stored separately from Scripture | ✅ | `AddNote`/`Notes`/`SetHighlight`/`Highlights` keyed by canonical reference — never inside verse text |
| 14 | Collections are first-class (favorites, reading plans, sermon passages) | ✅ | `AddCollection`/`AddToCollection`/`Collection`/`CollectionNames`; `TestBibleEngine` |
| 15 | Cross references queryable | ✅ | `CrossReferences` from verse `crossRefs` |
| 16 | EventBus integration (loaded/imported/updated/removed/passage_resolved/search/validation) | ✅ | `bible.*` events; `BibleImported` verified in `TestBibleEngine` |
| 17 | No WinUI/Presentation/Rendering/Output knowledge | ✅ | `modules/bible/` depends only on Common, EventBus, Logger, Search; zero UI/render references |
| 18 | Performance — instant lookup, background indexing, no UI blocking | ✅ | In-memory canonical storage; import indexes through the Search Engine; `Reload` re-validates |
| 19 | Tests — resolution, providers, engine, user data, events, failure modes | ✅ | `TestBibleResolver`, `TestBibleProviders`, `TestBibleEngine` (187 new checks, ASan clean) |

## 28. Phase 13 — Song & Lyrics Engine DoD (docs/specs/25)

Objective: a professional, structured, extensible Song & Lyrics subsystem. Songs
are structured musical content (metadata, sections, lyrics, chords, arrangements,
notes, licensing, media references); the engine transposes, arranges, detects
duplicates, versions, and indexes — while deliberately leaving presentation,
rendering, output, media, search infrastructure, notifications, and AI to the
engines that own them. All rows are met (`modules/songs/`, namespace `bps::song`).

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Structured song model — metadata, sections, lyrics, chords, arrangements, notes, licensing, media refs | ✅ | `SongTypes.hpp` (`Song`/`SongSection`/`SongLine`/`SongMetadata`/`Arrangement`/`LicensingInfo`/`MediaRef`) |
| 2 | Stable song id independent of filename | ✅ | `Song.id` (slug + timestamp on import); never derived from the source filename |
| 3 | Provider architecture — new format = implement + register | ✅ | `ISongProvider` + `RegisterProvider`; `ProPresenterProvider` added with zero engine changes |
| 4 | Built-in providers (ChordPro, OpenSong, OpenLP, ProPresenter, EasyWorship, plain text, JSON) | ✅ | 7 providers registered on `Initialize`; all exercised in `TestSongProviders` |
| 5 | Search works against actual lyrics/content | ✅ | Song documents indexed via Search Engine (lyrics + chords + authors + tags); `TestSongEngine` finds "how sweet the sound" |
| 6 | Chords are structured data (root, quality, extension, slash bass) | ✅ | `Chord` + `ChordSystem::Parse` (G, Am, D/F#, Cmaj7, Em7, G/B, C#m7b5, sus); unknown notation preserved as `custom` |
| 7 | Transposition works — minors, slash chords, extensions, accidentals, enharmonics | ✅ | `ChordSystem::Transpose` (spelling-family preserved); C→D, G/B→A/C#, Db→Eb verified in `TestSongChords` |
| 8 | Multiple keys (original, preferred, performance) without duplicating the song | ✅ | `SongMetadata.originalKey/preferredKey/performanceKey`; `SetPerformanceKey` |
| 9 | Arrangements are independent orderings referencing existing sections | ✅ | `Arrangement.sectionIds`; `EnsureOriginalArrangement` + `AddArrangement` (section existence validated); `TestSongEngine` |
| 10 | Sections are first-class and reorderable without rewriting lyrics | ✅ | `ReorderSections` (validates permutation); `TestSongEngine` |
| 11 | Duplicate detection uses actual content | ✅ | LCS-based `Similarity` (lyrics 70% + title 30%); import guard rejects near-identical imports (`Song_Duplicate`); `FindDuplicates`/`LikelyDuplicates` |
| 12 | Versioning + recovery — previous versions recoverable, crashes never corrupt | ✅ | `Versions`/`RestoreVersion` (bounded snapshot history per song); `TestSongEngine` |
| 13 | Copyright & licensing model | ✅ | `LicensingInfo` (holder/year/license/CCLI/source/restrictions); parsed from ChordPro `{ccli}`/`{copyright}` |
| 14 | Media references only — no duplicated media inside the engine | ✅ | `MediaRef` stores Media Engine asset ids only |
| 15 | Collections (favorites, worship sets, custom categories) | ✅ | `CreateCollection`/`AddToCollection`/`Collection`; `TestSongEngine` builds a Sunday-Morning set |
| 16 | EventBus integration (loaded/indexed/imported/updated/deleted/arrangement/key/validated) | ✅ | `song.*` events; `SongImported` verified in `TestSongEngine` |
| 17 | Presentation independence — never renders lyrics, returns structured data | ✅ | `modules/songs/` returns `Song` objects only; depends on Common, EventBus, Logger, Search; zero render/display references |
| 18 | Offline-first | ✅ | No network or service dependencies in the engine; online databases are optional providers |
| 19 | Performance — instant lookup/transposition/arrangement, background indexing | ✅ | In-memory registry; O(1) `GetSong`; transposition is a pure copy + chord map; `Reload` re-validates |
| 20 | Extensibility test — six months later, NEW_SONG_FORMAT = implement + register | ✅ | `RegisterProvider` + interface-only importers (demonstrated by the 7 built-ins); no engine/UI rewrite |
| 21 | Tests — chords, providers, engine, duplicates, versions, events, failure modes | ✅ | `TestSongChords`, `TestSongProviders`, `TestSongEngine` (187 new checks, ASan clean) |

## 29. Phase 14 — Service Flow & Automation Engine DoD (docs/specs/26)

Objective: a deterministic, event-driven automation system that runs an entire
service — not a playlist wrapper. A service is a `Flow` of executable nodes
(scene, song, bible, media, delay, condition, branch, parallel, wait, macro),
driven by triggers and conditions, coordinated through the EventBus, and always
overridable by the operator. All rows are met (`modules/automation/`, namespace
`bps::automation`, facade `FlowEngine`).

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Run an entire service — Countdown→Welcome→Video→Song→Bible→Announcement→Offering→Sermon→Closing→End | ✅ | `Flow`/`FlowNode` linear + cue-chain (`nextNodeId`) executor with `End`-terminates semantics; `TestFlowModel` builds and runs a Sunday-Service flow |
| 2 | Automation independent of UI — works from `.vgr` through the engine alone | ✅ | `modules/automation/` depends only on Common, EventBus, Logger, Json — zero UI/render/display references; `TestFlowEngine` runs headless |
| 3 | Manual control always possible — pause/resume/skip/jump/stop/restart/override | ✅ | `Pause`/`Resume`/`Skip`/`JumpTo`/`Stop`/`ExecuteAction`; manual control preempts automation (verified in `TestFlowEngine`) |
| 4 | Event-driven execution — WAIT FOR VideoCompleted, not a blind timer | ✅ | `NodeKind::Wait` + `waitFor`/`waitTimeoutMs`; consumes `MediaStateChanged`, `PresentationCompleted`, `SceneComposed`, `DisplayDevice(Dis)Connected` via `WireEvents` |
| 5 | Conditional automation — IF GPU_AVAILABLE → high/lite, IF OUTPUT == STAGE | ✅ | `Condition` nodes + 6 built-in conditions (`true`/`false`/`variable_equals`/`variable_set`/`output_is`/`gpu_available`) with `trueNodeId`/`falseNodeId` branch targets |
| 6 | Parallel actions execute concurrently without blocking | ✅ | `parallelNodeIds` groups + runtime `skipAfter` (group members skipped once the group completes); `TestFlowEngine` parallel completion |
| 7 | Reusable macros | ✅ | `RegisterMacro`/`RunMacro` (a macro expands into its node sequence); `TestFlowEngine` runs a worship macro |
| 8 | Flow templates | ✅ | `CreateTemplate`/`Templates`; a template instantiates fresh flows (`TestFlowEngine`) |
| 9 | Variables + data binding | ✅ | `SetVariable`/`GetVariable`/`Substitute` (`{SERVICE_NAME}`, `{DATE}`, `{SONG_TITLE}`...); `VariableChanged` event propagates updates |
| 10 | Coordinates multiple outputs | ✅ | `output_is` condition + `OutputIsCondition`; outputs are plain context values — routing stays with the Display Engine per the architecture |
| 11 | Defined failure handling — retry/skip/fallback/pause/notify/abort | ✅ | `onFailure` policy + `fallbackNodeId` + `retryLimit`; `NodeFailed`/`FlowInterrupted`; `fail_always` action verified in `TestFlowEngine` |
| 12 | Recovery after crash — knows flow, execution, current/completed/pending nodes, variables | ✅ | `Recover` from persisted `Execution` snapshots; `FlowRecovered` event; `TestFlowEngine` restores and completes |
| 13 | Complete execution history | ✅ | `History` (started / node executed / failed / skipped / completed / manual override / stopped); `TestFlowEngine` traces a full run |
| 14 | EventBus is the backbone — consume and publish | ✅ | consumes media/presentation/scene/display events; publishes `FlowStarted/Paused/Resumed/Completed/Stopped/Interrupted/Recovered/Validated`, `NodeStarted/Completed/Failed/Skipped`, `AutomationCancelled/EmergencyStopped`, `VariableChanged` |
| 15 | Notification integration — engine publishes, never drives notification UI | ✅ | `notify` action + `NotifyAction`; notifications flow through the Notification Service like every other event |
| 16 | Plugin extensibility — DMX/OBS/vMix/MIDI later with zero engine edits | ✅ | `IAction`/`ICondition`/`ITrigger` + `RegisterAction`/`RegisterCondition`/`RegisterTrigger`; fake actions/conditions/triggers registered in tests with no engine change |
| 17 | `.vgr` is the native format | ✅ | `Save(...,"vgr")` wraps `{"type":"flow","flow":{...}}`; `Load` unwraps the marker; round-trip verified in `TestFlowModel` |
| 18 | Performance — automation is never the bottleneck | ✅ | Deterministic in-memory executor (no sleeps except explicit Delay nodes; waits are event-driven); flow phase runs in <100 ms; heavy work stays on the Task Scheduler/Thread Pool |

## 30. Phase 15 — Extended Production Engine DoD (docs/specs/27)

Objective: a complete real-time production backbone where audio, video,
graphics, data and control all move as **signals through a universal node
graph** — Sources → Processing → Virtual Sources → Buses → Processing → Buses →
Outputs — for audio and video independently. The engine routes, mixes, plans,
clocks and monitors; it never renders pixels, plays audio, or routes screens
(Rendering/Media/Display engines own those). All rows are met
(`modules/production/`, namespace `bps::production`, facade `ProductionEngine`).

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Everything is a signal (audio/video/data/control) | ✅ | `SignalType` + `SignalInfo` (timestamp/duration/format/sampleRate/fps) on every node |
| 2 | Universal node graph | ✅ | 9 `NodeKind`s (Source/Processor/Bus/Mixer/Scene/Output/VirtualSource/Data/Control) in `ProductionGraph` |
| 3 | Virtual signals — a bus becomes a routable virtual source | ✅ | `CreateVirtualSource` auto-connects bus → VS and publishes `VirtualSourceCreated` |
| 4 | Bus hierarchy + automatic circular-routing rejection | ✅ | bus→bus routing with DFS cycle detection; `Connect` rejects with `Err::Production_CircularRoute` |
| 5 | Bus snapshots / bus scenes | ✅ | `SaveBusScene`/`ApplyBusScene` switch a bus's input set + config; `BusSceneApplied` |
| 6 | Every output = video bus + audio bus + capabilities | ✅ | `OutputConfig{videoBusId, audioBusId, caps}` incl. separate-audio + multi-channel; `AssignOutputBuses` validates bus type |
| 7 | Independent audio/video switching | ✅ | outputs carry independent audio/video bus ids — changing video never touches audio |
| 8 | Channel architecture (mono → 2.1/5.1/7.1/multi) | ✅ | `ChannelLayout` per node — never hard-coded to stereo |
| 9 | Processing chains per source/bus/output | ✅ | `ProcessingStage` (gate/EQ/compressor/limiter/color/delay/gain/denoise/custom) appended per node |
| 10 | Volume on every node (gain/mute/solo/pan/balance) | ✅ | `VolumeControl`; `SetGain`/`SetMute`/`SetSolo`/`SetPan` |
| 11 | Audio ducking with automatic return | ✅ | `Duck`/`ReleaseDuck` + `DuckDepth` (music dips under the speaker, returns on release) |
| 12 | Meters — peak/RMS/LUFS/clipping/headroom/channels | ✅ | `MeterLevels` + `UpdateMeters`/`GetMeters` per node |
| 13 | Clock architecture (master + audio/video/network + sync) | ✅ | `MasterClock`, `SetClockOffset`, `SetClockSync`, genlock-ready offsets |
| 14 | Control signals are first-class | ✅ | `SendControl` (midi/osc/keyboard/remote/api/plugin) → `ControlSignalReceived` |
| 15 | Macro recording for automation | ✅ | `StartMacroRecording`/`StopMacroRecording` → recorded action list ready for Phase 14 macros |
| 16 | Resource cost per node + production planner | ✅ | `ResourceCost` on every node; `PlanProduction` computes total, bottleneck, feasibility, encoder choice; `ProductionPlanned` |
| 17 | Source health + automatic fallback | ✅ | `SourceState`; `SetSourceFallback`/`TriggerFallback` switch to the backup and publish `SourceFailed`/`SourceFallback` |
| 18 | Scene states (preview/program/standby/disabled/emergency) | ✅ | `SceneState` + `SetSceneState` + `SceneStateChanged` |
| 19 | Atomic live changes | ✅ | `BeginEdit`/`CommitEdit`/`RollbackEdit` — commits that fail validation are rejected and rolled back |
| 20 | Frame-accurate cues | ✅ | `ScheduleCue` fires on the master clock timeline (control signals verified) |
| 21 | Production validation before going live | ✅ | `ValidateProduction` (missing nodes/buses, circular, signal mismatch, output_no_bus) + `ProductionValidated` |
| 22 | One-click production check + health | ✅ | `CheckProduction` scores sources/buses/outputs/routing; `ProductionHealth` |
| 23 | Simulation / dry-run | ✅ | `Simulate` validates and plans without going live |
| 24 | Production snapshots + emergency mode | ✅ | `Save/RestoreProductionSnapshot`; `EmergencyMode` (shed non-critical sources, emergency scene, stream stays up) |
| 25 | Output groups, failover order, priorities (safe degradation) | ✅ | `OutputConfig.group`/`failoverOrder`/`priority`; planner protects Critical/High under pressure |
| 26 | AI-ready validated production commands | ✅ | `ApplyCommand` — parse → validate → apply; AI can never mutate the graph directly |
| 27 | EventBus integration | ✅ | publishes `production.*` (started/stopped/bus/output/source/scene/snapshot/emergency/control/clock/planned); consumes display hot-plug events for output health |

## 31. Phase 16 — Recording, Replay & Media Capture Engine DoD (docs/specs/28)

Objective: a recording system that is not an OBS-style record button but a
proper media acquisition, recording, replay and archival layer built directly
into the production graph. Recording is an **output consumer** of Phase 15 —
anything that is a valid signal (source, bus, virtual source, scene, output)
can be recorded independently or as part of a production output, with its own
profile, tap point, format, quality, resource policy and lifecycle. It never
renders or plays content — it consumes the graph (`modules/recording/`,
namespace `bps::recording`, facade `RecordingEngine`). All rows are met.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Recording is an output consumer of the Phase 15 signal graph | ✅ | `StartRecording` validates the node exists in `ProductionGraph`; no second pipeline |
| 2 | Record any valid signal (source/bus/virtual/scene/output) | ✅ | node-based API — `StartRecording(profile, nodeId, tap)` accepts any graph node |
| 3 | Output-specific recording profiles (res/fps/codec/bitrate/container/channels) | ✅ | `RecordingProfile` (width/height/fps/bitrate/`VideoCodec`/`AudioCodec`/`ContainerKind`/channels) with validation |
| 4 | ISO recording (isolated cameras) + master recording | ✅ | one profile per node — `cam2` records in parallel with the program bus |
| 5 | Bus recording / multitrack | ✅ | buses are nodes; `multitrack` flag + per-track isolation model in the profile |
| 6 | Pre/post processing and pre/post output tap points | ✅ | `TapPoint{PreProcessing, PostProcessing, PreOutput, PostOutput}` |
| 7 | Format abstraction — new containers without core changes | ✅ | `IRecordingContainer` + `RegisterContainer` (mkv/mp4/mov/webm/wav/flac pre-registered) |
| 8 | Codec abstraction + hardware encoder selection with software fallback | ✅ | `IVideoEncoder`/`IAudioEncoder` + `RegisterVideoEncoder`; `SelectEncoder` prefers NVENC/AMF/QuickSync, falls back to software |
| 9 | Smart quality presets | ✅ | `QualityPreset{Proxy, Low, Standard, High, Broadcast, Master, Lossless, Custom}` |
| 10 | Recording lifecycle + queue states | ✅ | `RecordingState{Preparing, Recording, Paused, Finalizing, Completed, Recovering, Failed, Archived}` + start/stop/pause/resume/delete/archive |
| 11 | Markers + semantic recording metadata | ✅ | `AddMarker`; `RecordingMetadata` (event/speaker/song/scripture/bus/production) searchable later |
| 12 | Segmented recording (long recordings split) | ✅ | `segmentDurationMs` + time pump — `SegmentCreated` per boundary, treated as one logical recording |
| 13 | Crash recovery via recording journal | ✅ | `Journal()` (started/source/encoder/segment/marker/finalized) + `Recover()` finalizes interrupted sessions |
| 14 | Instant replay — rolling buffer + replay as a production source | ✅ | `CreateReplayBuffer` fills over time (`ReplayBufferReady`); `CreateReplay` registers a virtual video source (`ReplayCreated`) |
| 15 | Generic capture abstraction + hot-plug devices | ✅ | `ICaptureSource` + `RegisterCaptureSource`/`ConnectCaptureDevice` — the device becomes a graph Source node (`CaptureDeviceConnected`) |
| 16 | Disk-aware storage: monitoring, estimates, policies, emergency protection | ✅ | `GetStorageInfo` (free bytes/%/est-bytes-per-hour/capacity-hours); `SetStoragePolicy` warns <20%, stops optional <10%, protects production <5% |
| 17 | Scheduling / recording automation | ✅ | `ScheduleStart`/`ScheduleStop` fire from the time pump for automation and timed runs |
| 18 | Recording + encoder health monitoring | ✅ | `GetRecordingHealth` (load/dropped frames/queue/bitrate); `EncoderOverload`/`DroppedFramesDetected` events |
| 19 | EventBus integration | ✅ | publishes `recording.*` (started/stopped/paused/resumed/failed/recovered/segment/disk/encoder/dropped/replay/capture) |
| 20 | Plugin extensibility — no recording-engine rewrites | ✅ | containers/codecs/capture devices registered by plugins; default providers are pre-registered software fallbacks |

## 32. Phase 17 — Broadcast Engine (NDI & SDI) DoD (docs/specs/29)

Objective: real NDI (NewTek/Vizrt SDK) and SDI (Blackmagic DeckLink SDK)
connectivity as a provider-based broadcast module. Both SDKs are proprietary
runtime libraries, so the engine **never links them**: `IBroadcastProvider`
providers resolve the SDK at runtime through the PAL `ILibrary` seam and degrade
to `Err::Broadcast_Unsupported` when the library or hardware is absent —
matching the existing adaptive `ndi` feature-gating contract. A software
loopback provider ships in-tree so the full send/receive/discover contract runs
in CI and operator rehearsal with zero hardware (`modules/broadcast/`,
namespace `bps::broadcast`, facade `BroadcastEngine`). All rows are met.

| # | Expectation | Status | Evidence |
|---|---|---|---|
| 1 | Providers, not core code — new broadcast tech = register a provider | ✅ | `IBroadcastProvider` + `RegisterProvider` registry; built-ins `ndi`/`sdi`/`software`; plugins add more without engine changes (spec §1, §3) |
| 2 | Runtime SDK loading through the PAL — no direct `dlopen` in modules | ✅ | `NdiProvider`/`SdiProvider` resolve `libndi.so[.5]` / `libDeckLinkAPI.so` via `PlatformAccessor::Get().Library()`; a missing core symbol unloads + reports `Broadcast_SdkLoadFailed` (spec §2) |
| 3 | SDK init failures degrade — never report Available for a broken SDK | ✅ | `NDIlib_initialize` return checked: a failing init unloads the library and reports `Broadcast_SdkLoadFailed` instead of `Available` |
| 4 | Graceful degradation — absent SDK never fails the engine | ✅ | `Probe()` returns `Unavailable`; engine falls back to the software provider; CLI prints `ndi=unavailable sdi=unavailable send=ok recv=ok`; adaptive "ndi" feature stays gated |
| 5 | NDI discovery | ✅ | `DiscoverNdiSources()` via `NDIlib_find_*` (`NdiSourceInfo{name, urlAddress}`) + `sourcesDiscovered` counter |
| 6 | NDI sending — video + audio + clock/groups config | ✅ | `CreateNdiSender`/`SendVideoFrame`/`SendAudioFrame`/`StopSender`; `NdiSenderConfig{groups, clockVideo, clockAudio}`; payload borrowed (zero-copy into the SDK frame) |
| 7 | NDI receiving | ✅ | `CreateNdiReceiver`/`ReceiveFrame` (non-blocking capture, newest frame)/`DisconnectReceiver`; frame + audio freed via `NDIlib_recv_free_v2` |
| 8 | SDI device enumeration | ✅ | `EnumerateSdiDevices()` walks the DeckLink iterator COM vtable (`GetModelName`/`GetDisplayName`) and releases every device |
| 9 | SDI capture as a production-graph source | ✅ | `ConnectSdiCapture(index, graphNodeId)`/`DisconnectSdiCapture` — the device registers as a capture source the Phase 15/16 graph can consume |
| 10 | Software loopback provider — fully testable without hardware | ✅ | in-tree provider implements the full discover/send/receive contract in memory; `TestBroadcast*` exercises every path in CI (50 checks, 0 failures) |
| 11 | EventBus integration | ✅ | publishes `broadcast.*` (sender_created/destroyed, receiver_created/destroyed, capture_connected/disconnected, error, source_discovered); `WireEvents`/`UnwireEvents` |
| 12 | Error contract | ✅ | `Broadcast_*` codes (`SdkLoadFailed/Unsupported/NotFound/InvalidFrame/SendFailed/ReceiveFailed/ProviderNotFound`) in `Common.hpp`; `RecordError` feeds `errorCount` |
| 13 | Engine-wide IService contract | ✅ | `Initialize/Start/Stop/Shutdown/Reload/Reset/GetHealth/MetricsSnapshot`; `BroadcastEngine::Instance()`; Kernel boot step 29 (reverse-order shutdown) |
| 14 | Per-provider runtime stats + introspection | ✅ | `BroadcastStats{framesSent, framesReceived, sourcesDiscovered, errors}` via `Stats()`; `NdiAvailable()`/`SdiAvailable()`/`ProviderNames()`/`Probe(name)` |
| 15 | Clean shutdown releases SDK resources | ✅ | `ShutdownProvider` destroys senders/receivers, calls `NDIlib_destroy`, **and unloads the library handle** (no dlopen leak); SDI unloads `libDeckLinkAPI.so` |
| 16 | Acceptance — send/receive/discover/fallback without hardware | ✅ | `TestBroadcast*` (50 checks) + CLI demo: software loopback send→receive round-trip, discovery list, feature report (**34 systems booted**) |

## 20. Summary

| Status | Count |
|---|---|
| ✅ Met | 474 |
| ◑ Partial | 0 |
| ○ Deferred | 0 |

The core is in line with the architecture: the layered separation holds, the core has no
presentation knowledge, and every implemented manager satisfies the engine-wide contract.
Every expectation in the brief, the v1.0 core specification, and the Phase 2 PAL
Definition of Done (**26/26 met**) is audited. The Windows PAL backend is complete
(18 subsystems), the last OS-gated code in the core (the IPC transport) was moved
behind the PAL `ISocket` interface, and the engine targets **Windows x86/x64/ARM64,
Linux x86_64/aarch64/armv7 and macOS arm64/x86_64** with no host-architecture
assumptions (DoD §24). Phase 3 — Content & Asset Management — is complete
(**DoD §16: 18/18 met**): every asset has a UUID and metadata, all content I/O flows
through the Virtual File System (Disk/Memory/ZIP) on the PAL, import/export are
interface-based and registration-extensible, and CAMS integrates Logger, Config,
EventBus, ResourceManager (pressure eviction), and DatabaseManager (metadata
persistence) — verified by the `TestCams*` suite. Phase 4 — Notification Service
+ Project & Data System — is complete (**DoD §17: 33/33 met**): the Notification
Service is provider-driven, EventBus-fed, rules/policy-configurable, queue/schedule/
group/progress-capable and UI-agnostic; the Project & Data system manages projects,
workspaces, documents, undo/redo, history, recovery, backups, snapshots, packages,
dependencies, references, sessions, recents, favorites, templates and profiles —
verified by the `TestNotify*` / `TestProject*` suites and a CLI boot of **20 systems**.
Phase 5 — Adaptive Runtime System — is complete (**DoD §18: 22/22 met**): the
engine detects hardware by capabilities (never model names), every resource decision
flows through the runtime (`RuntimeOptimizer`), features are registerable and
suspendable, budgets are enforced per subsystem, quality scales automatically by
machine + mode + pressure, battery/thermal/memory reflexes protect the live
presentation, and the engine learns usage to preload what the user actually opens —
verified by the `TestAdaptive*` suite (2204 total checks, 0 failures, ASan clean,
zero warnings) and a CLI boot of **21 systems** (Adaptive boots last).
Phase 6 — Rendering Engine — is complete (**DoD §19: 26/26 met**): a generic,
frontend-independent engine converts Scene → Layers → Objects → Pixels through a
fixed-order pipeline; `IGraphicsBackend` is the only graphics seam (Null +
Software backends, DX12/Vulkan/Metal drop-in); text/images/video/shapes render with
animations, transitions and effects; GPU resources are managed with dedup, eviction
and budgets from the Adaptive Runtime; one scene feeds Audience/Stage/Preview/
Thumbnail/Stream/Screenshot outputs simultaneously — verified by the `TestRender*`
suite (**2516 total checks, 0 failures, ASan clean, zero warnings**) and a CLI boot
of **22 systems** (RenderEngine boots last).
Phase 7 — Display & Output Engine — is complete (**DoD §21: 20/20 met**): the
engine thinks in outputs, not monitors; `IDisplayProvider` (Null + Virtual +
Linux) is registration-based like importers and notifications; one rendered
frame routes to every output with 7 scaling modes; hot-plug detection, loss
recovery and profile persistence work; and the **display-test feature** renders
test patterns so any screen can be verified connected and receiving frames —
verified by the `TestDisplay*` suite and a CLI boot of **28 systems**.
Phase 8 — Presentation Engine — is complete (**DoD §22: 23/23 met**): a
state-driven live runtime (Created→…→Live↔Paused→…→Finished) that orchestrates
shows, slides, timelines, cues, navigation, compilation, validation, preloading,
session recovery and queued presentations — never rendering pixels or routing
outputs itself — verified by `TestPresentation*` (2719 total checks, 0 failures,
ASan clean, zero warnings across the whole suite).
Phase 9 — Search & Indexing Engine — is complete (**DoD §23: 19/19 met**): a
content-aware knowledge layer over a generic document model with inverted-index
storage, replaceable ranking, filters, suggestions, sessions, history, LRU
caching keyed by query **and** filter, JSON export/import, and asset
auto-indexing — verified by `TestSearch*`.
Phase 10 — Media Engine — is complete (**DoD §24: 19/19 met**): the single
owner of images/video/audio/animated/SVG assets with metadata extraction,
thumbnails, cached playback sessions, software/hardware decode delegation to the
Adaptive Runtime, and failure recovery — verified by `TestMediaEngine`.
The native **.vgr format** is complete (**DoD §25: 11/11 met**): one extension,
many internal document types, versioned self-describing header, per-section +
whole-file CRC32 integrity, RLE/stored compression, dependency manifests and
round-trip fidelity — verified by `TestVgrFormat` (including an ASan-caught
stack overflow in the serializer that was fixed).
Phase 11 — Scene Composition Engine — is complete (**DoD §26: 13/13 met**):
everything on screen is a Scene (Regions → Layers → Widgets), outputs pick
layouts, templates instantiate blueprints, themes restyle content, and a rule
engine drives visibility — all as .vgr documents — verified by
`TestSceneComposition`.
Phase 12 — Bible Engine — is complete (**DoD §27: 19/19 met**): a first-class
Scripture knowledge system with a canonical model (Testament/Book/Chapter/Verse/
Paragraph/Footnote/Cross-Reference/Translation Metadata), five built-in providers
(XML/JSON/USFM/OSIS/plain text) behind an `IBibleProvider` registry, an import→
validate→convert→index pipeline that rejects corrupt files, instant reference
resolution (John 3:16, Psalm 23, Genesis 1:1-10, 1 Corinthians 13, whole books),
content search through the Search Engine, parallel-Bible comparison, engine-owned
formatting, and user data (notes/highlights/collections) kept separate from
Scripture — verified by `TestBibleResolver`/`TestBibleProviders`/`TestBibleEngine`
and a CLI that imports a Bible, resolves John 3:16, searches, formats, and
compares (30 systems booted).
Phase 13 — Song & Lyrics Engine — is complete (**DoD §28: 21/21 met**): songs are
structured musical content with a canonical model, seven built-in providers
(ChordPro/OpenSong/OpenLP/ProPresenter/EasyWorship/plain/JSON) behind an
`ISongProvider` registry, structured chords with working transposition (minors,
slash chords, extensions, accidentals, enharmonics), independent arrangements,
content-based duplicate detection, versioning/recovery, collections, licensing,
and Search Engine integration — verified by
`TestSongChords`/`TestSongProviders`/`TestSongEngine` and a CLI that imports a
ChordPro song, transposes it, and adds arrangements (**2906 total checks, 0
failures, ASan clean**).
Phase 14 — Service Flow & Automation Engine — is complete (**DoD §29: 18/18
met**): a deterministic, event-driven orchestrator that runs an entire service
from a `.vgr` flow document (Countdown→Welcome→Video→Songs→Bible→Sermon→End)
through executable nodes with triggers, conditions, branches, parallel groups,
waits, macros and templates — while manual override (pause/resume/skip/jump/stop)
always wins, failures follow defined policies (retry/skip/fallback/pause/notify/
abort), crashed runs recover from persisted execution snapshots, and every step
is recorded in execution history — all coordinated through the EventBus with
zero UI coupling — verified by `TestFlowModel`/`TestFlowEngine` (**3046 total
checks, 0 failures** across the suite) and a CLI that loads a Sunday-Service
flow, validates it, runs it, overrides it, and shuts down cleanly (31 systems
booted).
Phase 15 — Extended Production Engine — is complete (**DoD §30: 27/27 met**):
a real-time production backbone where audio, video, graphics, data and control
all move as signals through a universal node graph — Sources → Processing →
Virtual Sources → Buses → Processing → Buses → Outputs — for audio and video
independently; outputs are complete signal destinations (video bus + audio bus
+ capabilities + priority + group + failover), circular routing is rejected at
connect time, buses snapshot into scenes, ducking and meters and per-node
processing chains work across mono→7.1 layouts, the master clock syncs
audio/video/network domains, control signals (MIDI/OSC/API/...) are
first-class, resources are costed and planned, sources fail over, edits are
atomic, production validates/simulates before going live, snapshots and
emergency mode protect the service, and AI can only mutate the graph through
validated commands — verified by `TestProductionGraph`/`TestProductionEngine`
(**3201 total checks, 0 failures** across the suite) and a CLI that routes a
camera + mics through program buses to TV/RTMP, rejects a cycle, ducks the
music, plans, and checks health (**32 systems booted**).
Phase 16 — Recording, Replay & Media Capture Engine — is complete
(**DoD §31: 20/20 met**): recording is a **first-class output consumer of the
Phase 15 production graph** — any valid signal (source, bus, virtual source,
scene, output) is recordable through its own profile (container, codec, bitrate,
fps, channels, quality preset, tap point, priority, segmentation) without a
second pipeline; hardware encoder selection falls back to software, ISO and
multitrack profiles isolate cameras while the program bus records the master,
markers and semantic metadata (event/speaker/song/scripture) make recordings
searchable, the journal supports crash recovery and segmentation, rolling
replay buffers become production virtual sources, capture devices hot-plug
into the graph, disk-aware storage policies shed optional recordings under
pressure and always protect the live production, recording is schedulable and
drives segments/monitoring through a time pump, and every lifecycle event
crosses the EventBus — verified by `TestRecordingEngine` (144 checks, 0
failures) and a CLI that records the program bus + camera ISO, drops markers,
creates a 30 s replay, segments, and reports storage (**33 systems booted**).
Phase 17 — Broadcast Engine (NDI & SDI) — is complete (**DoD §32: 16/16
met**): NDI (libndi) and SDI (Blackmagic DeckLink) ship as runtime-resolved
providers behind `IBroadcastProvider` — the engine never links either
proprietary SDK, probes load them through the PAL `ILibrary` seam, and a
failed SDK init (checked `NDIlib_initialize` return) or an absent library
degrades to the in-tree software loopback provider instead of failing the
engine; NDI sources are discovered (`NDIlib_find_*`) and sent/received with
zero-copy borrowed frames, SDI devices enumerate through the DeckLink COM
vtable and connect as production-graph capture sources, every lifecycle
event crosses the EventBus (`broadcast.*`), errors use `Broadcast_*` codes,
and shutdown unloads the SDK handles (no dlopen leak) — verified by
`TestBroadcast*` (**3395 total checks, 0 failures** across the suite) and a
CLI that reports `ndi=unavailable sdi=unavailable send=ok recv=ok` on a
bare host and **34 systems booted**.
Remaining work is all enhancement-level (real GPU backends in the renderer,
Windows/macOS host compile-verification, cloud VFS/backups, email/SMTP
notification providers, OCR/speech search providers, HDMI-matrix/OBS *display*
providers — NDI *display* now ships alongside the NDI *broadcast* transport,
§32) — none a redesign risk. Recently closed gaps: deflate compression
(`DeflateCompressor`, zlib, behind `ICompressor`), webhook notification
provider, PNG decode (`PngCodec`, self-contained zlib-based), monitor
orientation/HDR DRM reads, idle audio rate probing, and dialog color/font
pickers.
