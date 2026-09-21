# System Architecture

> **Status:** Approved — the reference architecture for Believers Presentation Software.
> Derived from the v1.0 Core Specification (`docs/specs/`) and the layered architecture
> requirements captured in the project brief. This document is the *first* one to read
> when asking "what is this engine, and where does X live?"

## 1. Design principles

The engine combines several proven architectural patterns, kept disciplined so that
presentation features never leak into the core:

| Pattern | Where it applies |
|---|---|
| **Layered architecture** | Frontends → Communication → Core → Managers → Feature Modules → Platform (see §2) |
| **Event-driven architecture** | Modules communicate through the EventBus (05), never by calling each other |
| **Service-oriented architecture** | Infrastructure is exposed through `IService` instances registered in the ServiceManager (04) |
| **Plugin architecture** | Features can be added/removed without changing the core (09) |
| **Entity-Component thinking** | Only where appropriate: presentations, layouts, rendering objects — not the whole engine |
| **Resource-oriented design** | A central ResourceManager (10) makes decisions about CPU, memory, GPU, caching, background work |
| **Platform Abstraction Layer (PAL)** | Windows / Linux / macOS specifics live behind interfaces (`platform/`), never in core code |

### The core purity rule

> **The core never knows about Bible, Songs, AI, or Presentations.**
> The PresentationEngine core provides *services that allow modules to do things*; it
> never implements those things itself. `modules/presentation` is a feature module, not
> part of the core. Anything presentation-shaped must live in a module or plugin.

## 2. Layer diagram

```
─────────────────────────────────────────────
Frontends
─────────────────────────────────────────────
WinUI3 │ Qt │ GTK │ CLI │ Web │ Remote App
                    │
                    ▼
─────────────────────────────────────────────
Communication Layer
─────────────────────────────────────────────
IPC │ Named Pipes │ WebSocket │ TCP │ gRPC (Future)
                    │
                    ▼
─────────────────────────────────────────────
Presentation Engine Core
─────────────────────────────────────────────
Kernel │ Services │ Events │ Lifecycle │ Scheduling │ Resources
                    │
                    ▼
─────────────────────────────────────────────
Engine Managers
─────────────────────────────────────────────
Logger │ Configuration │ Memory │ Plugin │ Module │
Display │ Renderer │ Assets │ Drivers │ Database
                    │
                    ▼
─────────────────────────────────────────────
Feature Modules
─────────────────────────────────────────────
Presentation │ Media │ Bible │ Songs │ Templates │ AI │
Cloud │ Streaming │ OBS │ NDI │ MIDI
                    │
                    ▼
─────────────────────────────────────────────
Platform Layer
─────────────────────────────────────────────
Windows │ Linux │ macOS
```

**Reading the layers:** frontends talk to the core *only* through the communication
layer. The core and managers expose services; feature modules are consumers of those
services. The platform layer is the only place OS-specific code lives.

## 3. Layer responsibilities

### 3.1 Frontends
Presentation surfaces: WinUI3 (primary desktop), Qt/GTK (portability), CLI (this repo's
v1 demo, `apps/cli/`), Web, and the Remote App (companion controller). Frontends never
contain engine logic — they render state and forward commands.

### 3.2 Communication layer
IPC transports that decouple frontends/remote controllers from the engine process:
named pipes, WebSocket, TCP, and (future) gRPC. v1 ships the **TCP + JSON-line**
reference transport (`core/include/core/ipc/IpcServer.hpp`, `IpcClient.hpp`): one JSON document per
line, handlers registered by method, `{"id","method","params"} → {"id","result"|"error"}`.
WebSocket and named pipes are future backends behind the same handler API; gRPC remains
future. The CLI demo still links directly against the core.

### 3.3 Presentation Engine Core
The six core services (`docs/specs/01–07`, `10–12`):
- **Kernel (01)** — boot/shutdown orchestration, global state, ordering, clock, health.
- **Services (04)** — DI container.
- **Events (05)** — decoupled pub/sub backbone.
- **Lifecycle (12)** — the one state machine every subsystem obeys.
- **Scheduling (06, 07)** — ThreadPool + TaskScheduler, the engine's time and concurrency.
- **Resources (10)** — machine telemetry + performance modes.

### 3.4 Engine Managers
The operational subsystems. **Logger, Configuration, Memory, Plugin, Module are
implemented in v1** (`core/logging`, `core/config`, `core/memory`, `core/plugins`,
`core/modules`). **Display, Renderer, Assets, Drivers, Database are implemented too**
(`core/display`, `core/rendering`, `core/assets`, `core/drivers`, `core/database`) behind
the interfaces (`interfaces/include/interfaces/IDisplay.hpp`, `IRenderer.hpp`, `IDriver.hpp`), registered as
services and booted by the Kernel (boot step 12). Feature modules register their own
backends into these registries — the core owns the registries, not the hardware.

### 3.5 Feature Modules
Everything end-user-visible: Presentation, Media, Bible, Songs, Templates, AI, Cloud,
Streaming, OBS, NDI, MIDI. v1 ships one reference module, `modules/presentation/`,
demonstrating how a module registers, starts, subscribes to events, and stops — without
the core knowing it exists.

### 3.6 Platform layer (PAL)
Phase 2 of the architecture brief is implemented (`docs/architecture/PAL.md`): a full
Platform Abstraction Layer owns **all** OS-specific code. `platform/include/platform/IPlatform.hpp`
is the facade — the only platform-dependent symbol the core links  (`platform::CreatePlatform()`) — aggregating **18 subsystem interfaces**: `IFilesystem`,
`IPaths`, `ITimer`, `IThreading`, `IProcess`, `ILibrary`, `IMonitor`, `IAudio`,
`INetwork`, `IPower`, `IClipboard`, `IEnvironment`, `ILocale`, `ISocket`, `IInput`,
`IDialogs`, `INotifications` (plus `IPlatform` itself). Headers live under `include/` trees
never beside their `.cpp` (DoD §folder): `core/include/core/`,
`modules/include/modules/`, `interfaces/include/interfaces/`,
`platform/include/platform/`; all includes are root-relative
(`core/...`, `modules/...`, `interfaces/...`, `platform/...`).

- **Backends**: OS-agnostic implementations live in `platform/common/` (`FilesystemImpl`
  on `std::filesystem`, `TimerImpl` on `std::chrono`, `PosixSocket`); the Linux backend
  (`platform/linux/`) reads `/proc`, `/sys`, `statvfs`, XDG dirs, ALSA PCM streams,
  `getifaddrs`, `/proc/bus/input/devices`, clipboard tools and desktop tools
  (zenity/notify-send). The **Windows backend (`platform/windows/`) is complete** for all
  18 subsystems (Win32, Winsock, Common Item Dialog, …); macOS is interface stubs.
- **Access**: the Kernel installs the backend at boot
  (`platform::PlatformAccessor::Install(CreatePlatform())`); managers route OS access
  through `PlatformAccessor::Get()` — PluginManager (IFilesystem + ILibrary),
  ModuleManager (IFilesystem), DatabaseManager (IFilesystem), ThreadPool (IThreading)
  and the IPC server/client (`ISocket`) no longer touch OS APIs directly.
- **Consumers**: ResourceManager consumes `IPlatform::Sample()` for machine telemetry
  (10 §3); the Kernel's platform watcher drains `PollChanges()` into the Event Bus as
  `platform.*` events (monitor/power/battery/resolution/audio-hotplug…).
- **Isolation**: the core never branches on OS macros (grep-verified — zero OS-gated
  `#if` outside the PAL); compile-time identity comes from `platform/include/platform/OsTag.hpp`
  (`kCompileOs`/`kCompileArch`/`kCompileBits`). The TCP IPC transport runs on the PAL
  `ISocket` interface — no socket code remains in the core.
- **Architecture coverage**: backends are pointer-width/endianness agnostic — Windows
  x86 (32-bit) / x64 / ARM64, Linux x86_64 / aarch64 / 32-bit ARM (armv7), macOS
  arm64 / x86_64. `Arch()` reports canonical names (`x86_64` · `x86` · `arm64` · `arm`
  · `riscv64`) equal to `kCompileArch` on every target (except Linux 32-bit builds on
  64-bit kernels, where uname reports the kernel arch); Win32 size hazards are guarded;
  cross-compiling is a standard CMake toolchain-file operation.

## 4. The engine-wide contract

Every manager — core or planned — implements the same interface (see
`docs/specs/00-cross-system-rules.md` §10 Engine-wide manager contract):

- **Lifecycle:** `Initialize() → Start() → Stop() → Shutdown()`, plus `Reload()` and
  `Reset()`. No exceptions across this boundary; failures are `Result<T>`.
- **Reporting:** every manager reports *Name, Version, Status, Uptime, CPU usage, Memory
  usage, Thread count, Dependencies, Last error, Health state* via
  `ServiceName() / ServiceVersion() / ServiceUptime() / MetricsSnapshot() / GetHealth()`
  on `IService` (`interfaces/include/interfaces/IService.hpp`).
- **Integration:** managers use the shared Logger (02), ConfigurationManager (03),
  EventBus (05), and LifecycleManager (12) — none implements its own version of these.

## 5. v1 implementation footprint (what exists today)

| Layer | Present in v1 | Location |
|---|---|---|
| Frontends | CLI (demo) | `apps/cli/main.cpp` |
| Communication layer | TCP + JSON-line IPC (reference transport) | `core/include/core/ipc/IpcServer.hpp`, `IpcClient.hpp` |
| Engine core | Kernel, Services, Events, Lifecycle, Scheduling (ThreadPool+TaskScheduler), Resources | `core/` |
| Managers | Logger, Configuration, Memory, Plugin, Module | `core/` |
| Managers (v1) | Display, Renderer, Assets, Drivers, Database | `core/display`, `core/rendering`, `core/assets`, `core/drivers`, `core/database` |
| Feature modules | Presentation (reference module), Songs, Media | `modules/presentation/`, `modules/songs/`, `modules/media/` |
| Content & Assets (Phase 3) | CAMS: ContentManager facade, AssetDatabase, AssetRegistry, AssetCache, AssetLoader, ImportManager (IImporter), ExportManager (IExporter), AssetIndexer, AssetWatcher, AssetValidator, AssetSerializer, AssetCompressor, ThumbnailManager, VFS (Disk/Memory/Zip) | `modules/content/`, `modules/include/modules/content/` |
| Notifications (Phase 4) | NotificationService facade (Factory → Rules → Queue → Manager → Providers), NotificationManager, NotificationQueue, NotificationScheduler, NotificationHistory, NotificationRules, NotificationFactory, Console/Center/StatusBar providers | `modules/notification/`, `modules/include/modules/notification/` |
| Project & Data (Phase 4) | DataManager facade + ProjectManager, ProjectRegistry, WorkspaceManager, DocumentManager (IDocumentHandler), UndoRedoManager, HistoryManager, SessionManager, SnapshotManager, RecoveryManager, BackupManager, PackageManager, DependencyManager, ReferenceManager, RecentManager, FavoritesManager, TemplateManager, ProfileManager | `modules/project/`, `modules/include/modules/project/` |
| Adaptive Runtime (Phase 5) | AdaptiveRuntime facade (heartbeat: Collect → Analyze → Optimize) + HardwareProfiler, CapabilityDetector, PerformanceProfiler, FeatureManager (ModuleContract), UserModeManager, ResourceBudgetManager, RuntimeOptimizer (the question-set brain), Power/Thermal/MemoryPressure/GPU managers, UsageLearningEngine, SmartCacheManager, StartupOptimizer, RecommendationEngine | `modules/adaptive/`, `modules/include/modules/adaptive/` |
| Rendering Engine (Phase 6) | RenderEngine facade (scenes, pipeline, outputs, diagnostics) + IGraphicsBackend (Null + Software backends), Scene/SceneNode/Layer/Camera, RenderObject (Text/Image/Video/Shape/Background/Gradient/Overlay/Countdown/Clock), TextEngine (FontManager + TextLayout), Animator + TransitionEngine, EffectStack, RenderPipeline + RenderGraph, GPUResourceManager, RenderCache, ResourceUploader, FrameGraph, RenderOutputs (Audience/Stage/Preview/Thumbnail/Stream/Screenshot) | `modules/rendering/`, `modules/include/modules/rendering/` |
| Platform | Linux telemetry via PAL | `platform/linux/LinuxPlatform.cpp` |
| PAL | `IPlatform` + `CreatePlatform()` (Win backends complete, mac stubs) | `platform/include/platform/IPlatform.hpp`, `platform/Platform.cpp` |

### 3.7 Content & Asset Management System (CAMS) — Phase 3

The engine's content backbone (docs/specs/13, docs/architecture/CAMS.md). After this
phase **no module accesses files directly** — everything goes through the
`bps::content::ContentManager` facade:

- **Facade**: `ContentManager` (IService) — find/load/save/import/export/delete/move/
  rename/duplicate/search. Boots after Database + PAL in the Kernel (boot step 13).
- **Library**: `AssetDatabase` (metadata-only store + type/tag/category/extension
  indexes), `AssetRegistry` (UUID → runtime asset, aliases, duplicate detection).
- **Runtime**: `AssetCache` (LRU, pin/evict/shrink), `AssetLoader` (sync + async via
  ThreadPool, progress + cancellation), `ThumbnailManager` (placeholder SVG + PNG/JPEG
  dimension extraction).
- **Pipeline**: `ImportManager` (IImporter registry — Open/Closed: new formats are
  registered, never coded into CAMS), `ExportManager` (IExporter registry),
  `AssetValidator`, `AssetSerializer` (JSON, schema version + migration),
  `AssetCompressor` (stored/RLE).
- **Discovery**: `AssetWatcher` (TaskScheduler heartbeat poll → `content.asset_*`
  events + DB sync), `AssetIndexer` (inverted index: exact/partial/fuzzy + filters).
- **Transport**: Virtual File System — `DiskVfs` (PAL IFilesystem), `MemoryVfs`,
  `ZipVfs`/`ZipWriter` (stored-entry ZIP read/write; deflate/zstd future).

Integrations: Logger (`CAMS` tag), ConfigurationManager (`cams.cache.bytes`,
`cams.roots`, `cams.watch.enabled`), EventBus (publishes `content.*`; consumes
shutdown / hot-reload / `ResourcePressureHigh` → cache shrink), DatabaseManager
(`content` collection persists metadata), ResourceManager (memory pressure eviction).

Conformance of every *expectation* against the implementation is tracked in
[`Conformance.md`](./Conformance.md). Detailed behavior contracts live in
[`docs/specs/`](../specs/README.md).

### 3.8 Notification Service + Project & Data System — Phase 4

Two foundational user-facing subsystems land in Phase 4 (docs/specs/14, 15;
docs/architecture/Notifications.md, ProjectSystem.md). Both follow the same
**Manager → Registry → Interface → Implementations → EventBus** pattern used by CAMS,
and both are UI-agnostic — WinUI/Qt/Web frontends consume them without backend changes.

**Notification Service** (`bps::notification`, Kernel boot step 14):

- **Pipeline**: `EventBus → NotificationFactory → NotificationRules → NotificationQueue
  → NotificationManager → INotificationProvider`. The factory translates engine events
  into `NotificationSeed`s (modules never build UI strings — `TemplateFormatter` owns
  text and enables localization).
- **Policy engine**: `INotificationRule` + `DefaultNotificationRules` (ProjectSaved →
  toast 5 s, suppressed while presenting; resource pressure → persistent; kernel panic
  → critical, always shown). Rules are registered, never hardcoded.
- **Queue**: priority buckets + rate limiting + dedup/grouping (`groupKey` merges
  repeats: “200 assets imported”). **Scheduler**: delayed delivery; `SetPresenting`
  suppresses non-critical notifications during a live presentation.
- **Providers**: `INotificationProvider` (channels + capabilities). Built-ins:
  Console, Center, StatusBar. Discord/Slack/Email/OBS/Webhook = register a provider,
  zero engine changes. The Notification Service only *manages* providers — it never
  creates a toast. Provider failures are isolated per provider.
- **History**: `NotificationHistory` + `INotificationStorage` (searchable, exportable).
  Consumes content/resource/platform/project events; publishes none.

**Project & Data System** (`bps::project`, Kernel boot step 15):

- **Facade**: `DataManager` (IService) exposes the whole subsystem: projects,
  workspace, documents, undo/redo, history, sessions, snapshots, recovery, backups,
  packages, dependencies, references, recents, favorites, templates, profiles.
- **Projects**: `ProjectManager` + `ProjectRegistry` — create/open/save/save-as/close/
  rename/duplicate/delete/archive; `.bpsproj` files through the PAL; metadata in the
  DatabaseManager (`projects` collection) so open projects restore after restart.
- **Documents**: `DocumentManager` + `IDocumentHandler` registry (presentation/song/
  theme handlers plug in); open/save/close/lock/read-only/dirty lifecycle.
- **Editing**: `UndoRedoManager` (ICommand + transaction groups, bounded stacks),
  `HistoryManager` (edit/save/checkpoint records, persisted).
- **Safety**: `SessionManager` (clean-shutdown flag), `RecoveryManager` (detects crashed
  sessions → `project.recovery_available`), `SnapshotManager` (manual/auto + restore),
  `BackupManager` (full/incremental, scheduled, retention, restore).
- **Portability**: `PackageManager` (`.bpspkg` ZIP of project + CAMS assets + manifest
  via ZipWriter; import rehydrates assets through CAMS), `DependencyManager` (edges +
  missing detection), `ReferenceManager` (unused/broken/duplicate/shared).
- **UX support**: `WorkspaceManager` (serializable layout/selection state),
  `RecentManager`, `FavoritesManager`, `TemplateManager`, `ProfileManager` (apply →
  ConfigurationManager keys).

Integrations: both subsystems use only the Core (Logger, Config, EventBus,
TaskScheduler, DatabaseManager) and CAMS — zero OS calls (all I/O through the PAL).
Project events (`project.saved/created/closed`, `project.recovery_available`,
`project.package_exported`, `project.backup_completed`, `project.snapshot_created`,
`project.undo_performed`) flow to the Notification Service, which decides what the
user sees. Kernel shutdown tears them down in reverse boot order (Data → Notifications
→ … → CAMS → Database).

### 3.9 Adaptive Runtime System (ARS) — Phase 5

The Adaptive Runtime (docs/specs/16; docs/architecture/AdaptiveRuntime.md) makes the
engine **self-optimizing rather than user-optimized**: it continuously asks “what is the
best experience I can deliver on this PC?” and answers from hardware + live metrics +
user preference — never from device names. It boots **last** (Kernel step 16, after
Projects) and shuts down **first**, so its heartbeat never runs during teardown.

- **Hardware & capabilities** — `HardwareProfiler` reads the PAL (`Info`, `Snapshot`,
  `Monitor`, `Power`, `Environment`); `CapabilityDetector::Supports(capability)` answers
  capability questions (hardware decode/encode, DirectX12, Vulkan, HDR, raytracing,
  compute, CUDA/OpenCL, SSE4/AVX2/AVX512/NEON). Decisions never check model names.
- **The question-set brain** — `RuntimeOptimizer` owns every `Get*` question
  (threads, texture/cache/render budgets, batch size, hardware decoder, GPU-per-task,
  AI tier, search tier, streaming quality, thumbnail %, background work). Subsystems
  ask the runtime; nothing outside `modules/adaptive/` hardcodes those numbers.
- **Budgets & contracts** — `ResourceBudgetManager` splits RAM into per-subsystem
  budgets (renderer/cache/search/video/AI/thumbnails) with pressure discounts;
  `FeatureManager` + `ModuleContract` (min/recommended/max RAM, GPU policy, threads,
  startup cost, lazy/suspension support) auto-enable/disable/suspend features.
- **Quality & user control** — 7 quality profiles (Ultra…Minimal/Custom), 9 user modes,
  3 config layers (Automatic/Assisted/Expert) and 5 assisted preferences; Automatic
  derives the profile from RAM/GPU, assisted modes bias it, expert overrides it.
- **Reflexes** — `PowerManager` (battery), `ThermalManager` (≥ 85 °C), `MemoryPressureManager`
  and `GPURuntime` throttle background work while the live presentation stays untouched
  (`BackgroundWorkAllowed()`).
- **Learning** — `UsageLearningEngine` (kind+id, per-weekday, persisted via the
  Database) → `SmartCacheManager` preload hints → `StartupOptimizer` module set;
  `RecommendationEngine` suggests (never forces) profile changes.
- **Heartbeat** — every 3 s on the Core TaskScheduler: Collect (PAL snapshot +
  profiler) → Analyze (battery/thermal/memory/presentation) → Optimize
  (`ApplyOptimization`: ThreadPool resize, CAMS cache capacity, ResourceManager mode).
- **Events** — consumes `engine.resource.pressure_changed`, `platform.power_changed`,
  `platform.battery_low`, `platform.monitor_*`, `platform.sleep/wake`,
  `engine.config.hot_reload`; publishes `adaptive.quality_changed`,
  `adaptive.module_suspended/resumed`, `adaptive.memory_budget_changed`,
  `adaptive.optimization_applied`, `adaptive.feature_enabled/disabled`.

### 3.10 Rendering Engine — Phase 6

The Rendering Engine (docs/specs/17; docs/architecture/RenderingEngine.md) converts
engine objects into pixels and knows nothing about WinUI, Qt, GTK, or any frontend —
and nothing about presentations, Bibles or songs. It only knows **Scene → Layers →
Objects → Pixels**. It boots **last** (Kernel step 17, after Adaptive) and shuts down
**first**, so no frame loop ever runs during teardown.

- **Backends** — `IGraphicsBackend` is the *only* graphics seam in the engine:
  `NullGraphicsBackend` (headless accounting) and `SoftwareGraphicsBackend` (a
  complete CPU rasterizer: clear, fill rect, image/glyph blits, shapes, and
  per-region pixel effects) ship today; DirectX12 / Vulkan / Metal drop in behind
  the same interface. Frames are exchanged as backend-agnostic RGBA8 buffers.
- **Scene system** — `Scene` → `Layer` → `SceneNode` → `Component`. Layers are
  independent and disable-able (Background/Video/Image/Text/Overlay/Debug/Custom);
  nodes carry local/world transforms (position + rotation + scale compose with
  parent); a camera provides viewport, zoom and title-safe areas.
- **Render objects** — generic renderables: Text, Image, Video, Shape,
  Background, Gradient, Overlay, Countdown, Clock, Custom. Nothing is hardcoded
  for "Song" or "Bible".
- **Text engine** — Unicode-aware `TextLayout` (alignment, wrapping, letter/line
  spacing, RTL, vertical alignment), `TextStyle` decorations (stroke, shadow,
  glow, gradient fill), and `FontManager` with a built-in 5×7 bitmap font that
  renders on any host with zero external assets (glyph atlas + fallback +
  discovery; TrueType/OpenType plug in behind the same interface).
- **Animation + transitions** — `Animator` (timeline keyframes over position /
  scale / rotation / opacity / color / mask / custom with 10 easing functions)
  and `TransitionEngine` (fade / slide / push / zoom / reveal / wipe / crossfade,
  direction + easing). Both are deterministic and unit-tested.
- **Effects** — `EffectStack` with blur, glow, shadow, opacity, crop, mask,
  brightness, contrast, saturation — applied as pixel operations by the backend.
- **Pipeline + render graph** — `RenderPipeline` owns the fixed pass order
  (Background → Video → Image → Text → Overlay → Effects → Debug); nothing
  bypasses it. `RenderGraph` is the extensible layer: custom passes register as
  dependency nodes and run in topological order without modifying the renderer.
- **GPU resources** — `GPUResourceManager` (texture/shader registry, dedup by
  name, ref-counted release, idle eviction, budgets from the Adaptive Runtime),
  `RenderCache` (glyph atlases + text layouts), `ResourceUploader` (async decode
  on the core ThreadPool, drained per frame) and `FrameGraph` (per-frame usage /
  idle tracking).
- **Render outputs** — `Audience / Stage / Preview / Thumbnail / Stream /
  Screenshot`. One scene renders once; `OutputManager` distributes the frame to
  every enabled output at its own target size. Screenshot saves PPM via the PAL.
  This is the seam the Display Engine (Phase 7) uses to route to monitors / NDI /
  OBS without duplicating rendering logic.
- **Events** — publishes `render.frame_rendered`, `render.texture_loaded`,
  `render.shader_compiled`, `render.gpu_out_of_memory`, `render.frame_dropped`,
  `render.error`; consumes `content.asset_loaded/deleted`, `display.changed`,
  `engine.config.hot_reload`, `engine.resource.pressure_changed`.
- **Diagnostics** — `RenderStats`: FPS, frame time, GPU memory, draw calls,
  texture/shader counts, frame drops, queue size.

### 3.11 Display & Output Engine — Phase 7

The Display Engine (docs/specs/18; docs/architecture/DisplayEngine.md) thinks in
**outputs**, not monitors. A monitor is just one provider. It boots after the
Rendering Engine (Kernel step 18) and consumes one rendered frame, distributing it
to every output — adding a monitor never duplicates rendering.

- **Provider architecture** — `IDisplayProvider` (Initialize / Enumerate / Probe /
  Capabilities / ShowFrame / Shutdown) with three providers today: `Null`
  (headless, never fails), `Virtual` (three software displays — audience 1080p /
  stage 720p / preview 540p — with simulated hot-plug via
  `AddVirtualDevice/RemoveVirtualDevice`), and `Linux` (real monitors through the
  PAL `IMonitor` + `PlatformAccessor`). Windows / OBS / NDI / Web / LED-wall
  providers drop in behind the same interface — no engine changes.
- **Outputs** — `OutputKind` (Audience / Stage / Preview / Recording / Stream /
  Screenshot / Custom). Each output binds a display device, a layout region and a
  `ScalingMode` (Native / Fit / Fill / Stretch / Letterbox / Crop / PixelPerfect),
  a resolution and a refresh rate; outputs start/stop and are re-routed on
  hot-plug without restart. **The display-test feature** (`DisplayTest`) renders
  test patterns (color bars, gradient, checkerboard, solid colors, frame-code)
  so an operator can verify a screen is connected, sized, and receiving frames
  before going live.
- **Router** — `OutputRouter` maps one source frame onto every output using the
  output's scaling + region (letterbox/crop math is pixel-exact and
  unit-tested).
- **Profiles** — `DisplayProfiles` save/restore named output assignments
  („Sunday Morning“ = audience+stage+preview; „Youth Service“ = +recording).
  JSON-serialized; a disconnected display is remembered and automatically
  restored on reconnect.
- **Recovery** — a lost display never stops the presentation: the output is
  marked lost, the assignment is kept, and the next probe reconnects it
  automatically (`DisplayRestored`).
- **Events** — publishes `display.device_connected/disconnected`,
  `display.ready/failed/restored`, `display.profile_applied`,
  `output.started/stopped/lost`; consumes `display.changed` and
  `engine.resource.pressure_changed` (drops to a minimal frame under pressure).

### 3.12 Presentation Engine — Phase 8

The Presentation Engine (docs/specs/19; docs/architecture/PresentationEngine.md)
is the **conductor**, not the artist: it orchestrates live presentations and
never renders pixels or routes outputs. It boots after the Display Engine (Kernel
step 19).

- **Model** — `Presentation` → `PresentationSlide` (stable UUID ids, sections,
  tags, notes, metadata) → content blocks. `PresentationCompiler` validates +
  compiles raw shows into `CompiledPresentation` (resolved references, prepared
  scenes, ready transitions, no raw project access at runtime).
- **State machine** — `PresentationStateMachine` with
  Created → Loaded → Validated → Compiled → Prepared → Ready → Live ↔ Paused →
  Stopped → Finished (+ Recovering/Closed); every transition is validated and
  invalid ones fail with a typed error.
- **Runtime + controller** — `PresentationRuntime` tracks current slide, active
  cues/timers/media and playback mode; `PresentationEngine` is the facade —
  Open/Load/Validate/Compile/Prepare/GoLive/Pause/Resume/StopPlayback and
  Next/Previous/JumpTo/BlackScreen/LogoScreen — all routing through the
  controller.
- **Navigation** — `PresentationNavigator`: Next/Previous/First/Last, jump by
  id/tag/section, search-and-jump, plus navigation history with Back().
- **Timeline + cues** — `PresentationTimeline` (sequential / parallel / delayed /
  scheduled / looping entries with deterministic time-based evaluation) and
  `IPresentationCue` with a built-in set (slide, media, audio, timer, countdown,
  black, logo, script). New cue types register without touching the engine.
- **Scene builder** — `SceneBuilder` converts a slide into a `rendering::Scene`
  (text/image/shape objects) through the public RenderEngine API — the
  Presentation Engine never touches GPU objects.
- **Validator** — `PresentationValidator` reports missing media/fonts, broken
  references, duplicate ids and unsupported content as warnings, never crashes.
- **Session + recovery** — `PresentationSession` snapshots the runtime state
  (presentation id, slide id, playback mode, position, outputs) to JSON;
  `RecoverSession` restores it after a crash (`PresentationRecovered`).
- **Events** — publishes `presentation.opened/closed/compiled/validated/started/
  paused/resumed/stopped/completed/recovered`, `presentation.slide_changed`,
  `presentation.transition_started/completed`, `presentation.cue_triggered`;
  consumes `display.ready`, `media.ready`.

### 3.13 Search & Indexing Engine (SIE) — Phase 9

The Search Engine (docs/specs/20) is the engine-wide knowledge layer: it does not
know what a Bible, song, image or presentation is — it only knows **documents,
metadata and indexes**. It boots after the Presentation Engine (Kernel step 20).

- **Document model** — `SearchDocument`: id, type, title, content, author,
  language, tags, metadata map, modified-time, rank boost, related ids. Content-
  aware: `IIndexAdapter` per type extracts real content (song lyrics, slide
  text, PDF text) so search finds *what is inside* a file, not just its name.
- **Index** — `IndexStorage` (inverted postings with term frequencies,
  prefix-completion terms, per-document metadata, snapshot/restore, corruption-
  tolerant validation). Full + incremental indexing; re-indexing a document
  replaces it without duplicates.
- **Query** — `SearchEngine::Search` parses raw text into structured terms,
  ranks with a replaceable `IRankingStrategy` (exact > prefix > partial >
  metadata + recency decay + rank boost), applies `SearchFilter` (type, tag,
  language, author, date range), returns snippets, and LRU-caches per
  **query+filter signature** so filtered searches never reuse unfiltered
  results.
- **Suggestions, sessions, history** — prefix suggestions over history +
  index terms; restorable `SessionState`; recent-query history; index
  export/import (JSON) and `ValidateIndex` health checks.
- **Events** — publishes `search.index_updated/index_rebuilt/started/completed/
  suggestions_updated`; consumes `content.asset_loaded/deleted` (assets are
  indexed automatically) and `engine.config.hot_reload`.

### 3.14 Media Engine — Phase 10

The Media Engine (docs/specs/21) is the single owner of every media asset:
images, videos, audio, animated images, SVG — it knows nothing about songs,
slides or themes. It boots after the Search Engine (Kernel step 21).

- **Model** — `MediaAsset` with rich, extensible metadata (type, format,
  resolution, duration, codec, bitrate, frame rate, channels, orientation,
  color space, timestamps, tags, GPS); `MediaKind` covers Image / Video / Audio /
  AnimatedImage / Svg / Font / Background / Custom.
- **Pipeline** — import → validate → extract metadata → generate thumbnail →
  cache → ready; `MediaPipelineReport` exposes validation/processing results.
- **Thumbnails** — `GenerateThumbnail` produces deterministic
  `ThumbnailInfo` (target size + aspect) and publishes
  `media.thumbnail_generated`; generation is cheap and non-blocking.
- **Playback** — `PlaybackSession` tracks state (Idle → Preparing → Playing →
  Paused → Stopped), position, speed, looping and seek; software decode with
  hardware decode delegated to the Adaptive Runtime
  (`ShouldUseHardwareDecoder`).
- **Caching + recovery** — per-kind caches (`CacheSizes`), a `CacheBudgetBytes`
  cap, automatic eviction, and `ResetCaches`; decode failures publish
  `media.decode_failed`, retry/fallback and never crash the engine.
- **Events** — publishes `media.imported/ready/removed/thumbnail_generated/
  playback_started/playback_stopped/decode_failed`.

### 3.15 Native .vgr format

The .vgr format (docs/specs/22) is the platform's native document format — one
extension, many internal document types (Show / Presentation / Template / Theme /
Workspace / Playlist / Project Package / Asset Collection / Config Profile). It
boots before the Scene Engine (Kernel step 22) and is the source of truth that
importers convert *into* and exporters convert *from*.

- **Header** — magic `BPSVGR01`, format version, engine version, document type,
  UUID, created/modified timestamps, author, compression hint, integrity hash,
  dependency manifest, external asset references, search metadata, custom
  metadata.
- **File I/O** — `VgrFile` (modules/vgr) is the one place .vgr files are read
  and written: reads validate every CRC; writes go to a `.tmp` beside the target,
  are read back and verified, then swapped in with the old file parked as `.bak`
  (restored on failure) — one complete file on disk at every instant. Shows
  (`PresentationDocument`) and templates (`TemplateFile`) both use it.
- **Layout** — [magic][format version][header JSON length][header JSON][section
  count][sections…][whole-file CRC32]. Every section is (name, kind, length,
  crc32, payload); `document` (JSON body) + named sections + embedded assets
  are supported.
- **Integrity** — per-section CRC32 + whole-file CRC32; `Read` rejects bad
  magic, unsupported versions and truncated sections; version-forward
  migration is detected by the header.
- **Compression** — `stored` and `rle` (asset payloads) today; deflate/zstd
  slot in behind the same header field.

### 3.16 Scene Composition Engine (SCE) — Phase 11

The Scene Composition Engine (docs/specs/23) is the definitive layer above the
Presentation Engine: **everything on screen is a Scene**, and scenes are composed
from Regions → Layers → Widgets rather than hardcoded „songs“ or „slides“. It
boots last (Kernel step 23).

- **Scene tree** — `SceneComposition` holds named `SceneRegion`s; each region
  holds ordered `SceneLayer`s; each layer holds a widget. Outputs choose a
  **layout** (which regions appear where), so Audience / Stage / Stream render
  the same scene differently without duplicating presentations.
- **Widgets** — `IWidget` (id, type, name, region, layer, enabled, opacity,
  visible-on rule) with built-ins: Text, Image, Video, Clock, Countdown, Logo,
  QR, Ticker, LowerThird, Scoreboard, Weather, Chat. Everything derives from
  `IWidget` — no special-cased content systems.
- **Templates** — `SceneTemplate` (id, name, widget list) instantiates a scene
  blueprint; templates are stored as .vgr documents, so they are searchable,
  versioned and shareable like any other document.
- **Themes** — `SceneTheme` (id, name, colors, fonts, spacing, animation
  durations) applied to a scene; content never owns styling — changing one
  theme restyles every scene that uses it.
- **Rule engine** — `SceneRule` (IF condition THEN action: show/hide widget,
  set layout, apply theme, set opacity) is evaluated per output; rules make
  „IF ContentType == Song AND Output == Audience THEN SongLayout“ possible
  without hardcoding.
- **Events** — publishes `scene.composed/layout_applied/rule_applied/
  theme_applied`.

## 6. Lifecycle of this document

- This file replaces the original empty placeholder. It is the architectural home for
  layer boundaries, the core purity rule, and the engine-wide contract.
- When a new layer or manager is added, update §2/§3/§5 here and the corresponding spec
  in `docs/specs/` — the two must never disagree.
