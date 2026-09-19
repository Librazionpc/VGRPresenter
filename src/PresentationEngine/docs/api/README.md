# Engine API — Consumption Guide & Catalog

> **Purpose:** this directory is the contract between every frontend (WinUI, Linux UI,
> web, CLI, remote app) and the C++ engine. It answers two questions:
>
> 1. **What APIs exist?** — the full catalog of public entry points, one section per system.
> 2. **How should a UI consume them?** — the patterns to follow so the UI stays a _client_
>    of the engine and never a duplicate of it.
>
> Behavioral details live in `docs/specs/` (one file per system) and the headers
> under `core/include/`, `modules/include/`, and `interfaces/include/`. This file is the
> index + the UI-facing map.

---

## 1. The golden rules for any UI

These rules come from `docs/architecture/SystemArchitecture.md` and the phase briefs.
If a UI breaks one of these, it is consuming the engine wrong.

1. **The UI never touches internals.** No direct access to globals, singletons other
   than the public `Xxx::Instance()` entry points, no bypassing the Kernel.
2. **The UI resolves systems through the Kernel / ServiceManager**, never by
   re-instantiating them. Everything the UI needs is already booted and registered.
3. **The UI reads state through APIs, not by peeking at storage.** Bible text, song
   content, scenes, recordings, search results — all come back as structured data
   objects (`Result<T>`). Never parse engine files yourself.
4. **The UI writes state through commands.** Engines are the only writers. The UI
   submits intents (e.g. `GoLive()`, `Import()`, `ApplyProfile()`), engines validate
   and execute.
5. **The UI listens through the EventBus.** Every state change the UI cares about is
   published as a typed event. Subscribe once at startup; never poll UI state.
6. **Errors are `Result<T>`.** Every API returns `Result<T>` — check `.ok()`, surface
   `.error().message` in the UI, never throw across the boundary.
7. **Lifecycle stays with the Kernel.** A UI calls `Kernel::Instance().Boot()` /
   `Shutdown()` and never calls `Initialize()/Start()` on individual systems itself.
8. **Everything is UI-independent.** If your feature would require touching engine
   code to add a new format, device, provider, action, or condition — you are
   extending the wrong layer. Register a provider/plugin instead.

---

## 2. The UI bootstrap sequence

Every UI (WinUI, Linux UI, web, CLI) starts the same way:

```text
1. Kernel::Instance().Boot(BootOptions{...})      // boots 29+ systems in order
2. Read EngineContext / GetRuntimeInfo()           // what is running, health
3. Subscribe to EventBus events the UI cares about  // typed, one per concern
4. Resolve engines via ServiceManager (or singletons) and render initial state
5. On exit: Kernel::Instance().Shutdown()
```

`BootOptions` (see `core/include/core/kernel/Kernel.hpp`) controls the boot:
`configPath`, `logLevel`, `threadCount`, `resourceMode`, `profileDir`, `dataDir`,
`pluginDirs`, `ipcPort` (>0 starts the TCP+JSON IPC server for remote apps).

The Kernel boot order (from `core/kernel/Kernel.cpp`, sections 1–29):

| #   | System                                                           | Spec | Role                            |
| --- | ---------------------------------------------------------------- | ---- | ------------------------------- |
| 1   | Logger                                                           | 02   | all logging                     |
| 2   | ConfigurationManager                                             | 03   | config + profiles + migration   |
| 3   | ServiceManager                                                   | 04   | DI registration / resolution    |
| 4   | EventBus                                                         | 05   | typed pub/sub backbone          |
| 5   | ThreadPool                                                       | 06   | async execution                 |
| 6   | TaskScheduler                                                    | 07   | timers / cron / retries         |
| 7   | ResourceManager                                                  | 10   | CPU/RAM/disk/GPU pressure       |
| 8   | MemoryManager                                                    | 11   | bounded allocation tracking     |
| 9   | PluginManager                                                    | 09   | plugin discovery + isolation    |
| 10  | ModuleManager                                                    | 08   | feature modules + sidecars      |
| 11  | LifecycleManager                                                 | 12   | entity state tracking           |
| 12  | Platform PAL + Assets/Drivers/Database/Display/Renderer managers | 2/13 | OS abstraction + core managers  |
| 13  | ContentManager                                                   | 13   | VFS, import/export, asset cache |
| 14  | NotificationService                                              | 14   | user notifications              |
| 15  | DataManager / ProjectManager                                     | 15   | .vgr projects + documents       |
| 16  | AdaptiveRuntime                                                  | 16   | hardware-aware quality          |
| 17  | RenderEngine                                                     | 17   | scenes, text, backends          |
| 18  | DisplayEngine                                                    | 18   | outputs, profiles, routing      |
| 19  | PresentationEngine                                               | 19   | presentations + cues + runtime  |
| 20  | SearchEngine                                                     | 20   | full-text index + sessions      |
| 21  | MediaEngine                                                      | 21   | media metadata + playback       |
| 22  | VgrFormat                                                        | 22   | native .vgr read/write/migrate  |
| 23  | SceneCompositionEngine                                           | 23   | templates, layouts, rules       |
| 24  | BibleEngine                                                      | 24   | Bibles, references, notes       |
| 25  | SongEngine                                                       | 25   | songs, chords, transposition    |
| 26  | FlowEngine                                                       | 26   | service automation              |
| 27  | ProductionEngine                                                 | 27   | buses, routing, cues            |
| 28  | RecordingEngine                                                  | 28   | recording, replay, capture      |
| 29  | BroadcastEngine                                                  | 29   | NDI + SDI                       |

---

## 3. Catalog — Core infrastructure APIs

All core systems are singletons (`Xxx::Instance()`) registered in the ServiceManager.

### 3.1 Kernel — `core/include/core/kernel/Kernel.hpp`

Boot / control / inspect the whole engine.

```cpp
Boot(BootOptions) · Shutdown() · Pause() · Resume() · Panic(Error) · Recover(BootOptions)
State() · EngineVersion() · Uptime() · Now() · Options() · BootLog()
EngineUuid() · GetBuildInfo() · GetRuntimeInfo() · Context() · GetHealth()
```

### 3.2 EventBus — `core/include/core/events/EventBus.hpp`

The single communication backbone (spec 05). Typed, topic-addressed, sticky, replayable.

```cpp
Publish<T>(event, {async, delay}) · PublishAsync<T>(event)
Subscribe<T>(cb, priority) → Subscription · Unsubscribe(token)
SubscriberCount(topic) · SetSticky(topic, event) · GetSticky(topic)
SetHistoryLimit(n) · History(topicFilter) · Replay<T>(cb)
```

**UI pattern:** subscribe once per screen/widget at startup; render on event; keep the
subscription token and unsubscribe on teardown. See §6 for the full event catalog.

### 3.3 Logger — `core/include/core/logging/Logger.hpp`

Structured logging (spec 02). The UI reads `Search(LogQuery)` and can add sinks.

```cpp
Info/Warning/Error/Fatal(module, message) · Debugger/Json/File/Console sinks
Search(LogQuery) · SetGlobalLevel/SetCategoryLevel · SetSessionId · CrashLog
AddSink/RemoveSink · Flush · GetHealth
```

### 3.4 ThreadPool — `core/include/core/threading/ThreadPool.hpp`

Async task execution (spec 06).

```cpp
Initialize(n) · Shutdown · Submit(fn, opts) → TaskHandle
SubmitBackground / SubmitRealtime · SubmitFuture(fn, ...) → std::future
Cancel(handle) · Resize(n) · SetWorkerAffinity · CurrentTaskCancelled()
```

### 3.5 TaskScheduler — `core/include/core/task_scheduler/TaskScheduler.hpp`

Timers, cron, countdowns, animations (spec 07).

```cpp
ScheduleOnce / ScheduleEvery / ScheduleCron / ScheduleHeartbeat / ScheduleCountdown
ScheduleAnimation / ScheduleSequence / ScheduleWithRetry
Cancel(id) · Pause(id) · Resume(id) · Reschedule(id, period)
```

### 3.6 ResourceManager — `core/include/core/resources/ResourceManager.hpp`

Machine telemetry + resource pressure (spec 10).

```cpp
Telemetry() · Sample() · MemoryPressure()/CpuPressure()/DiskPressure()/GpuVramPressure()
RecordUsage(owner, bytes) · ClearUsage(owner) · UsageSnapshot() · TotalUsageBytes()
SetMode(mode) · Mode() · Claim(owner, bytes) · ReleaseClaim(owner)
```

### 3.7 MemoryManager — `core/include/core/memory/MemoryManager.hpp`

Allocation accounting + budgets (spec 11).

```cpp
Allocate(tag, size) · Free(ptr) · LiveBytes(tag) · TotalLiveBytes()
SetTagLimit(tag, bytes) · SetGlobalLimit(bytes) · Snapshot() · GetHealth
```

### 3.8 ConfigurationManager — `core/include/core/config/ConfigurationManager.hpp`

Config files, profiles, hot reload (spec 03).

```cpp
Load(scope) · Save(scope) · Get<T>(key) · Set<T>(key, value)
Profile/SetProfile · Migrate · Reload · GetHealth
```

### 3.9 ServiceManager — `core/include/core/services/ServiceManager.hpp`

DI container for all systems (spec 04).

```cpp
Register<T>(service) · Resolve<T>() · Unregister<T>() · Services() · GetHealth
```

### 3.10 DatabaseManager — `core/include/core/database/DatabaseManager.hpp`

Simple JSON document store (spec 14 §15).

```cpp
Open(filePath) · Put(collection, key, json::Value) · Get(collection, key)
Remove · Keys(collection) · Collections() · Flush · DocumentCount
```

### 3.11 PluginManager — `core/include/core/plugins/PluginManager.hpp`

Plugin discovery, isolation, lifecycle (spec 09).

```cpp
Discover(searchPath) · Load(id) · Unload(id) · Enable/Disable · Plugins()
RegisterManifest · GetHealth
```

### 3.12 ModuleManager — `core/include/core/modules/ModuleManager.hpp`

Feature-module registry (spec 08). Every feature engine registers here.

```cpp
RegisterModule(IModule) · UnregisterModule(id) · RegisterFactory(id, factory)
Discover(searchPath) · Load(id) · Start(id) · Stop(id) · Modules() · GetHealth
```

### 3.13 LifecycleManager — `core/include/core/lifecycle/LifecycleManager.hpp`

Entity state tracking + state machine (spec 12).

```cpp
RegisterEntity(id, version) · SetState(id, state) · State(id) · Entities()
Transition(id, from, to) · GetHealth
```

### 3.14 AssetManager — `core/include/core/assets/AssetManager.hpp`

Core asset registry + cache accounting.

```cpp
Register(AssetInfo) · Unregister(id) · Get(id) · Snapshot() · CachedBytes()
SetCacheCapacity · GetHealth
```

### 3.15 DriverManager / DisplayManager / RendererManager (core)

Hardware-driver registry, OS-display abstraction, renderer-backend registry.

```cpp
DriverManager:  RegisterDriver · Drivers() · GetHealth
DisplayManager: RegisterBackend · Displays() · Outputs() · GetHealth
RendererManager: RegisterRenderer · SetActive · Renderers() · GetHealth
```

### 3.16 IPC — `core/include/core/ipc/IpcServer.hpp` + `IpcClient.hpp`

TCP + JSON-line transport for remote apps (remote control, log streaming).

```cpp
Server: Start(port) · Stop() · RegisterHandler(method, fn) · Broadcast(jsonLine)
Client: Connect(host, port) · Call(method, params) → json::Value · ReadNextLine()
```

---

## 4. Catalog — Feature engine APIs (Phases 3–17)

Every feature engine follows the same shape: `Xxx::Instance()`, the full
`IService` lifecycle, `GetHealth()`, `MetricsSnapshot()`, provider registration, and
domain operations. Headers under `modules/include/modules/`.

### 4.1 ContentManager — content (spec 13)

VFS mounts, import/export, asset cache.

```cpp
MountDisk(name, hostDir) · MountMemory(name) · MountZip(name, archive) · Unmount(name)
ImportFile(hostPath, opts) → Uuid · ExportAsset(uuid, exporter, dest)
Search(SearchQuery) · ReindexAll() · RegisterImporter/RegisterExporter
SupportedImportExtensions() · SupportedExportFormats() · GetCacheStats() · Evict()
ShrinkCache(fraction) · SetCacheCapacity(bytes) · WatchRoot / PollWatch / ValidateAll
```

### 4.2 NotificationService — notifications (spec 14)

```cpp
Publish(NotificationSeed) → id · NotifyNow(n) · NotifyAfter(n, delayMs)
UpdateProgress(id, fraction) · Dismiss(id) · MarkRead(id)
History(query) · ClearHistory() · ExportHistory(path) · SetPresenting(bool)
InvokeAction(id, actionId) · Poll() · Queue()/HistoryStore()/Manager()/Scheduler()
```

### 4.3 AdaptiveRuntime — adaptive quality (spec 16)

```cpp
Hardware() · Supports(cap) · Capabilities() · GetRecommendedThreadCount()
GetTextureBudget() · GetCacheBytes() · Optimize() · Profile() · GetHealth
```

### 4.4 RenderEngine — rendering (spec 17)

```cpp
SetBackend(name) · BackendCaps() · CreateScene(id, name, size) → Scene · GetScene(id)
RemoveScene(id) · Render(frame) · SetQuality(q) · FrameStats() · GetHealth
```

The `Scene` API (modules/rendering/Scene.hpp) owns layers, text, textures, effects.

### 4.5 DisplayEngine — display + outputs (spec 18)

```cpp
RegisterProvider/UnregisterProvider · Devices() · GetDevice(id) · RefreshDevices()
AssignOutput(outputId, deviceId) · EnableOutput(id, bool) · SetOutputTransform(id, t)
SaveProfile(profile) · ApplyProfile(name) · GetProfile(name) · ProfileNames()
RouteFrame(frame, sink) · RestoreAssignments() · ConnectedDeviceCount()
RunningOutputCount() · RunSelfTest()
```

### 4.6 PresentationEngine — presentations (spec 19)

```cpp
CreatePresentation(name) → id · Open(id) · Close() · Save() · Duplicate(id) · Delete(id)
Get(id) → Presentation · PresentationIds() · ActivePresentationId()
GoLive() · Pause() · Resume() · StopPlayback() · Next() · Previous()
JumpTo(index) · JumpById(slideId) · Search(query) · AddCue(cue) · ClearTimeline()
Tick(dt) · State() · CurrentIndex() · CurrentSlide() · Runtime() · SaveSession() · Recover()
```

### 4.7 SearchEngine — search (spec 20)

```cpp
RegisterAdapter/UnregisterAdapter · IndexDocument(doc) → id · RemoveDocument(docId)
Rebuild(docs) · Search(query, limit) → vector<SearchResult>
Suggest(prefix, limit) · SetRankingStrategy · RawSearch(query)
BeginSession/UpdateSession/GetSession · RecentQueries · ExportIndex/ImportIndex · ValidateIndex
```

### 4.8 MediaEngine — media (spec 21)

```cpp
RegisterProvider · Import(path, opts) → MediaId · Get(id) · Metadata(id)
Play(id) · Pause(id) · Stop(id) · Seek(id, seconds) · SetPlaybackRate(id, rate)
SetLoop(id, bool) · GetPlaybackState(id) · GetPosition(id) · StepFrame(id)
CacheEntries() · TrimCache(n) · ProcessQueue(batch)
```

### 4.9 SceneCompositionEngine — scene composition (spec 23)

```cpp
RegisterTemplate(tpl) / RemoveTemplate / GetTemplate / TemplateIds
RegisterTheme(theme) / RemoveTheme · RegisterLayout(layout) / GetLayout / LayoutIds
AddRule(rule) / RemoveRule / Rules / ClearRules
Compose(contentId, contentType) → ComposedScene
UpdateWidgetVisibility(widgetId, visible) · UpdateLayout(layoutId, layout) · RuleCount
```

### 4.10 BibleEngine — Bible (spec 24)

```cpp
RegisterProvider/UnregisterProvider · ProviderNames()
Import(source, format, opts) → bibleId · RemoveBible(id) · BibleIds() · BibleCount()
GetBible(id) → BibleVersion · GetBook(id, bookId) → BibleBook
GetPassage(id, book, ch, v1, v2) → verses · GetVerse(...) · VerseCount(id)
ResolveReference(text) → PassageRef · ResolveReferences(text)
Search(query) → hits · Compare(refText, bibleIds) → parallel verses
Format(bibleId, ref, opts) → text
AddNote(id, ref, text) · Notes(id, ref) · SetHighlight(id, ref, on) · Highlights(id)
AddCollection(name) · AddToCollection(coll, ref) · Collection(coll) · CollectionNames()
CrossReferences(id, ref) → vector<CrossReference>
```

### 4.11 SongEngine — songs (spec 25)

```cpp
RegisterProvider/UnregisterProvider · Import(source, format, opts) → songId
CreateSong(Song) → songId · GetSong(id) · UpdateSong(id, song) · RemoveSong(id)
Search(query) → songs · Transposed(id, semitones) → Song · SetPerformanceKey(id, key)
AddArrangement(id, arr) · RemoveArrangement(id, arrId) · Arrangements(id)
ReorderSections(id, orderedIds) · CreateCollection(name) · AddToCollection(coll, songId)
Collection(coll) · CollectionNames() · FindDuplicates() → groups · LikelyDuplicates(id)
Versions(id) · RestoreVersion(id, versionId)
```

### 4.12 FlowEngine — service automation (spec 26)

```cpp
RegisterAction(type, factory)/UnregisterAction · ActionNames()
RegisterCondition(type, factory) · ConditionNames()
RegisterTrigger(type, factory) · TriggerNames()
Load(source, format, flowId) → flowId · Save(flowId) · Delete(flowId) · FlowIds()
Start(flowId, vars) → executionId · Pause(executionId) · Resume(executionId)
Stop(executionId) · JumpTo(executionId, nodeId) · EmergencyStop()
ExecutionState(executionId) → view · History(executionId) → node records
Snapshot(executionId) → FlowSnapshot · Recover(snapshot) → executionId
SetVariable/GetVariable/Variables(executionId) · Notify(topic, executionId)
RequestDispatch(executionId, topic) · Poll(executionId) · ActiveExecutions()
Substitute(text, vars)
```

### 4.13 ProductionEngine — production graph (spec 27)

```cpp
Graph() → ProductionGraph
CreateBus(id, name, role, type) → busId · RemoveBus(busId) · Buses()
AddSource(id, name, type, config) → sourceId · Connect(sourceId, busId)
SaveBusScene(busId, name) · ApplyBusScene(busId, name)
AssignOutputBuses(outputId, videoBus, audioBus) · SetOutputPriority(outputId, p)
CreateVirtualSource(busId) → sourceId · SetDuck(busId, trigger, depthDb)
ScheduleCue(atMs, action, payload) · CueCount()
SaveProductionSnapshot(name) · RestoreProductionSnapshot(name)
EmergencyMode(reason) · BeginEdit()/CommitEdit()/RollbackEdit() · ApplyCommand(cmd)
```

### 4.14 RecordingEngine — recording (spec 28)

```cpp
CreateProfile(profile) → id · GetProfile(id) · ProfileIds() · RemoveProfile(id)
CreateTemplate(name, profileIds) · TemplateNames()
DetectEncoders() → vector<EncoderKind> · SelectEncoder(profile) · SetPreferredEncoder(profileId, kind)
RegisterContainer(name, factory) · RegisterVideoEncoder(kind, factory) · RegisterAudioEncoder(codec, factory)
StartRecording(profileId, nodeId, tapPoint) → recordingId · StopRecording(id, reason)
PauseRecording(id) · ResumeRecording(id) · AddMarker(recordingId, label)
GetStatus(id) → RecordingStateView · RecordingIds() · Recordings(state)
DeleteRecording(id) · ArchiveRecording(id) · SetMetadata(id, key, value) · GetMetadata(id)
Journal() → entries · Recover() → finalized count
CreateReplay(recordingId, durationMs) → replayId
SetDefaultDirectory(dir) · SetStoragePolicy(freePercentBelow, action)
ScheduleStart(profileId, nodeId, atMs) · ScheduleStop(recordingId, atMs)
GetRecordingHealth(id) → EncoderHealth · UpdateEncoderLoad(id, pct)
UpdateDroppedFrames(id, n) · Tick(ms) · ClockMs()
```

### 4.15 BroadcastEngine — NDI + SDI (spec 29)

```cpp
RegisterProvider/UnregisterProvider · ProviderNames()
Probe(name) → ProviderState · DiscoverNdiSources() → sources
CreateNdiSender(name, cfg) → senderId · SendVideoFrame(sender, info, data, bytes)
SendAudioFrame(sender, info, data, bytes) · StopSender(id)
CreateNdiReceiver(sourceName) → receiverId · ReceiveFrame(id, info, out) → bool
DisconnectReceiver(id) · ReceiverIds() · EnumerateSdiDevices()
ConnectSdiCapture(deviceIndex, graphNodeId) → captureId · DisconnectSdiCapture(id)
SdiCaptureIds() · Stats() · NdiAvailable() · SdiAvailable()
```

### 4.16 ProjectManager + managers — projects (spec 15)

```cpp
ProjectManager: Create(name, templateId) → Project · Open(hostPath) → Project
  Save(id, autosave) · SaveAs(id, path) → Project · Close(id) · Rename(id, name)
  Duplicate(id) → Project · Delete(id) · Archive(id, archived) · SetAssetReferences(id, uuids)
  OpenProjects() · Active() · SetActive(id) · Get(id) · LoadAll() · AutosaveAll() · SetDataDir(dir)
DocumentManager: RegisterHandler(handler) · UnregisterHandler(type) · Open(type, path) → docId
  Save(docId) · Close(docId) · Lock(docId, locked)
UndoRedoManager: ExecuteCommand(ICommand) · Undo() · Redo() · CanUndo()/CanRedo()
  ICommand{Execute()/Undo()/Redo()/Label()} · LambdaCommand(label, doFn, undoFn)
HistoryManager: Record(projectId, command, detail) → entry · History(projectId)
RecoveryManager: RecordUnsavedWork(projectId, docId, detail) · ClearProject(projectId)
  Resolve(itemId) · DetectCrashedSession() · CurrentSessionId() · Suggestions() → items
TemplateManager: Create(name, sourceProjectId, description) → templateId · List()
  Get(templateId) · Remove(templateId) · SetDefault(templateId) · Default()
BackupManager: Backup(projectId, destDir, full) → backupId · ApplyRetention(projectId, keepFull, keepIncremental)
  EnableScheduled(every, destDir) · DisableScheduled()
SnapshotManager: Create(projectId, label, json, automatic) → snapshotId · List(projectId)
  Get(snapshotId) · Restore(snapshotId) · Remove(snapshotId) · ClearProject(projectId)
FavoritesManager: Add(kind, id, name) · Remove(kind, id) · IsFavorite(kind, id)
  List(kind) · Save() · Load() · Count()
RecentManager: Record(kind, id, name) · List(kind, limit) · Remove(kind, id) · Clear() · Save() · Load()
WorkspaceManager: SetOpenDocuments(docIds) · OpenDocuments() · Add/RemoveOpenDocument(docId)
  SetSelectedDisplays(displayIds) · SelectedDisplays() · SetCurrentPresentation(assetUuid)
ReferenceManager: Analyze(projectId) → report · AnalyzeAll() · RemoveUnused(projectId, uuids)
  ClearDuplicates(projectId)
ProfileManager: Create(kind, name, settings, description) → profileId · List(kind) · Get(profileId)
  Remove(profileId) · UpdateSettings(profileId, settings) · Apply(profileId)
SessionManager: Begin(user) · End() · SetActiveProject(projectId) · ActiveProject()
  SetUser(user) · User() · NotePresentationOpened()
ProjectRegistry: Register(Project) · Update(Project) · Unregister(id) · Find(id)
  FindByName(name) · FindByPath(path) · All()
DependencyManager: Add(projectId, parent, child, kind) · Remove(projectId, parent, child)
  ClearProject(projectId) · Dependencies(projectId) → edges · Validate(projectId) → issues
PackageManager: Export(projectId, hostDestPath) · Import(hostPkgPath, targetMount) → projectId
  Inspect(hostPkgPath) → manifest · PackagesImported()
DataManager: Initialize/Start/Stop/Shutdown/Reload/Reset · GetHealth (project data store)
```

### 4.17 VgrFormat — native format (spec 22)

```cpp
Write(VgrDocument) → bytes · Read(bytes) → VgrDocument
PeekVersion(bytes) · Migrate(doc, fromVersion)
```

### 4.18 Rendering support types

- `RenderObject` — drawable primitives (text, rect, image, gradient).
- `TextEngine` — layout/measure + rasterize glyphs to textures.
- `Animation` — keyframed property animation.
- `Effects` — blur/overlay/tint passes.
- `Pipeline` — frame pipeline stages.

---

## 5. Catalog — Platform Abstraction Layer (PAL)

Every OS capability is behind an interface in `platform/include/platform/` with
Linux and Windows implementations under `platform/linux/` and `platform/windows/`.
A UI never calls the OS directly; it goes through these:

```text
IPlatform (aggregate) · IFilesystem · IEnvironment · ILibrary · ITimer
IClipboard · IThreading · IDialogs · INetwork · IPaths · IAudio · IProcess
ISocket · ILocale · IMonitor · IPower · IInput · INotifications
```

Access via `platform::PlatformAccessor` (see `platform/include/platform/PlatformAccessor.hpp`).
`OsTag` (`OsTag.hpp`) identifies the host OS; `IsX()` helpers switch behavior.

---

## 6. Catalog — EventBus topics (what the UI can subscribe to)

All events are typed structs in `core/include/core/events/Events.hpp`; each declares a
`static constexpr const char* kTopic`. Subscribe with
`EventBus::Instance().Subscribe<events::PresentationStarted>(...)`.

| Domain        | Topics                                                                                                                                                                                                                                                                                                                             |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Kernel        | `engine.kernel.booted` · `state_changed` · `shutdown_started` · `shutdown_complete` · `panic`                                                                                                                                                                                                                                      |
| System        | `engine.system.health_changed` · `config.hot_reload` · `resource.pressure_changed` · `resource.pressure_high` · `resource.mode_changed` · `threadpool.cpu_pressure`                                                                                                                                                                |
| Assets        | `engine.asset.state_changed`                                                                                                                                                                                                                                                                                                       |
| Modules       | `engine.module.state_changed`                                                                                                                                                                                                                                                                                                      |
| Plugins       | `engine.plugin.state_changed` · `updated` · `sandbox_denied`                                                                                                                                                                                                                                                                       |
| Platform      | `monitor_connected` · `monitor_disconnected` · `power_changed` · `battery_low` · `monitor_resolution_changed` · `sleep` · `wake` · `network_changed` · `device_connected` · `device_removed` · `locale_changed` · `clipboard_changed`                                                                                              |
| Content       | `content.asset_added` · `removed` · `changed` · `loaded` · `unloaded` · `imported` · `exported` · `deleted` · `indexed` · `cache_updated` · `thumbnail_generated` · `validation_failed`                                                                                                                                            |
| Notifications | `notification.action_invoked`                                                                                                                                                                                                                                                                                                      |
| Adaptive      | `adaptive.quality_changed` · `module_suspended` · `module_resumed` · `memory_budget_changed` · `optimization_applied` · `feature_enabled` · `feature_disabled` · `recommendation`                                                                                                                                                  |
| Projects      | `project.opened` · `saved` · `closed` · `created` · `deleted` · `document_opened` · `document_saved` · `document_closed` · `document_dirty` · `undo_performed` · `redo_performed` · `snapshot_created` · `backup_completed` · `package_exported` · `recovery_available` · `dependency_issue`                                       |
| Rendering     | `render.frame_rendered` · `texture_loaded` · `shader_compiled` · `gpu_out_of_memory` · `frame_dropped` · `error`                                                                                                                                                                                                                   |
| Display       | `display.device_connected` · `device_disconnected` · `ready` · `failed` · `restored` · `profile_applied`                                                                                                                                                                                                                           |
| Outputs       | `output.started` · `stopped` · `lost`                                                                                                                                                                                                                                                                                              |
| Presentation  | `presentation.slide_changed` · `opened` · `closed` · `compiled` · `validated` · `started` · `paused` · `resumed` · `stopped` · `completed` · `recovered` · `transition_started` · `transition_completed` · `cue_triggered`                                                                                                         |
| Search        | `search.index_updated` · `started` · `completed` · `index_rebuilt` · `suggestions_updated`                                                                                                                                                                                                                                         |
| Media         | `media.imported` · `ready` · `removed` · `thumbnail_generated` · `playback_started` · `playback_stopped` · `decode_failed` + legacy `media.state_changed`                                                                                                                                                                          |
| Scene         | `scene.composed` · `layout_applied` · `rule_applied` · `theme_applied`                                                                                                                                                                                                                                                             |
| Bible         | `bible.imported` · `loaded` · `updated` · `removed` · `indexed` · `passage_resolved` · `search_completed` · `validation_completed` · `validation_failed`                                                                                                                                                                           |
| Songs         | `song.imported` · `loaded` · `indexed` · `updated` · `deleted` · `arrangement_changed` · `key_changed` · `validated` · `validation_failed` + legacy `songs.selected` · `songs.verse_changed`                                                                                                                                       |
| Flow          | `flow.started` · `paused` · `resumed` · `completed` · `stopped` · `interrupted` · `recovered` · `validated` · `node_started` · `node_completed` · `node_failed` · `node_skipped` · `automation_cancelled` · `emergency_stopped` · `variable_changed`                                                                               |
| Production    | `production.started` · `stopped` · `bus_changed` · `bus_scene_applied` · `output_changed` · `output_failed` · `output_failover` · `source_failed` · `source_fallback` · `scene_state_changed` · `validated` · `snapshot_saved` · `restored` · `emergency` · `virtual_source_created` · `control_signal` · `clock_sync` · `planned` |
| Recording     | `recording.started` · `stopped` · `paused` · `resumed` · `failed` · `recovered` · `segment_created` · `disk_space_warning` · `encoder_overload` · `dropped_frames` · `replay_buffer_ready` · `replay_created` · `capture_connected` · `capture_disconnected`                                                                       |
| Broadcast     | `broadcast.provider_registered` · `provider_unavailable` · `ndi_sources_changed` · `sender_started` · `sender_stopped` · `receiver_connected` · `receiver_disconnected` · `frame_sent` · `frame_received` · `sdi_devices_changed` · `error`                                                                                        |

---

## 7. UI consumption recipes (copy these patterns)

### 7.1 Boot the engine and show a status banner

```cpp
auto& kernel = Kernel::Instance();
if (auto r = kernel.Boot({.logLevel = LogLevel::Info, .dataDir = "./data"}); !r.ok())
    ui->ShowFatal(r.error().message);
ui->SetStatus("Engine " + kernel.EngineVersion().ToString() + " · " +
              std::string(ToString(kernel.State())));
```

### 7.2 Show the live slide as the operator advances

```cpp
auto& bus = EventBus::Instance();
subs.push_back(bus.Subscribe<events::PresentationStarted>(
    [&](const auto& e) { ui->EnterLive(e.id); }));
subs.push_back(bus.Subscribe<events::SlideChanged>(
    [&](const auto& e) { ui->ShowSlide(e.index, e.slideId); }));
// operator action:
PresentationEngine::Instance().Next();
```

> The UI never mutates presentation state directly except through `Next/Previous/GoLive/...`.

### 7.3 Search everything (Bible, songs, content) in one dialog

```cpp
auto& engine = SearchEngine::Instance();
if (auto r = engine.Search(ui->QueryText(), 50); r.ok())
    ui->ShowResults(r.value());          // unified result list
// per-domain deep search:
BibleEngine::Instance().Search(query);   // actual verse text
SongEngine::Instance().Search(query);    // actual lyrics
ContentManager::Instance().Search(SearchQuery{.text = query});  // assets
```

### 7.4 Show a Bible passage with a translation picker

```cpp
auto& bible = BibleEngine::Instance();
if (auto ref = bible.ResolveReference(ui->RefText()); ref.ok())
    if (auto verses = bible.GetPassage(bibleId, ref.value()); verses.ok())
        ui->ShowPassage(bible.Format(bibleId, ref.value(), FormatOptions{}));
```

### 7.5 Run a song live, transposed to the band's key

```cpp
auto& songs = SongEngine::Instance();
// +2 semitones = C -> D. Chord::TransposeKey handles the key name;
// SongEngine::Transposed transposes every chord in the song.
auto perf = songs.Transposed(id, /*semitones=*/2).value();
ui->ShowLyrics(perf.sections);   // engine already transposed the chords
```

### 7.6 Start a service flow and mirror its progress

```cpp
auto& flow = FlowEngine::Instance();
auto exec = flow.Start("sunday", {"SPEAKER_NAME", "Bro. John"}).value();
subs.push_back(bus.Subscribe<events::NodeStarted>(
    [&](const auto& e) { ui->HighlightCue(e.executionId, e.nodeId); }));
subs.push_back(bus.Subscribe<events::NodeCompleted>(
    [&](const auto& e) { ui->CheckCue(e.nodeId); }));
// manual override is always allowed:
flow.JumpTo(exec, "sermon");   // operator click
```

### 7.7 Add a recording profile and record the program

```cpp
auto& rec = RecordingEngine::Instance();
auto prof = rec.CreateProfile({
    .id = "sunday", .name = "Sunday master", .container = ContainerKind::MKV,
    .videoCodec = VideoCodec::H264, .audioCodec = AudioCodec::AAC,
    .width = 1920, .height = 1080, .fps = 60.0,
    .encoderPreference = EncoderKind::Auto});   // Auto = best available
// Tap the "pvb" (program video bus) node at the post-processing tap point.
auto rid = rec.StartRecording(prof.value(), "pvb", TapPoint::PostProcessing).value();
// during the service:
rec.AddMarker(rid, "Sermon started");
rec.SetMetadata(rid, "event", "Sunday Service");
// on end:
rec.StopRecording(rid, "service complete");
```

### 7.8 Show the production graph health in an operator dashboard

```cpp
auto& prod = ProductionEngine::Instance();
ui->DrawGraph(prod.Graph().Nodes(), prod.Graph().Edges());
subs.push_back(bus.Subscribe<events::ProductionOutputFailed>(
    [&](const auto& e) { ui->FlagOutput(e.outputId, e.error); }));
subs.push_back(bus.Subscribe<events::RecordingDiskSpaceWarning>(
    [&](const auto& e) { ui->WarnDisk(e.freePercent); }));
```

### 7.9 Remote control from a second device

```cpp
IpcClient client;
if (auto c = client.Connect("127.0.0.1", port); !c.ok()) return;
// The CLI registers one presentation command when booted with --ipc:
//   presentation.slide  {index: <int>}
auto resp = client.Call("presentation.slide",
                        json::Value::Object{{"index", json::Value::Number(3)}});
// bps_remote is the reference client: ping | health | logs | slide | shutdown
```

### 7.10 Extend the engine without touching it

```cpp
// New import format? -> implement IBibleProvider/ISongProvider/IImporter, register.
// New action type?    -> FlowEngine::RegisterAction("myaction", factory).
// New display?        -> DisplayEngine::RegisterProvider(...).
// New NDI/SDI?        -> BroadcastEngine::RegisterProvider(...).
// Everything else     -> implement IModule, ModuleManager::RegisterModule.
```

---

## 8. File map (where each API lives)

| Concern                    | Headers                                                                                                                                                                                                      |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Core lifecycle & DI        | `core/include/core/kernel/Kernel.hpp`, `core/services/ServiceManager.hpp`, `interfaces/include/interfaces/IModule.hpp`, `IService.hpp`                                                                       |
| Events                     | `core/include/core/events/EventBus.hpp`, `Events.hpp`                                                                                                                                                        |
| Logging                    | `core/include/core/logging/Logger.hpp`                                                                                                                                                                       |
| Scheduling                 | `core/include/core/threading/ThreadPool.hpp`, `core/task_scheduler/TaskScheduler.hpp`                                                                                                                        |
| Resources                  | `core/include/core/resources/ResourceManager.hpp`, `core/memory/MemoryManager.hpp`                                                                                                                           |
| Config/DB/Plugins          | `core/include/core/config/ConfigurationManager.hpp`, `core/database/DatabaseManager.hpp`, `core/plugins/PluginManager.hpp`                                                                                   |
| IPC                        | `core/include/core/ipc/IpcServer.hpp`, `IpcClient.hpp`                                                                                                                                                       |
| Feature engines            | `modules/include/modules/<domain>/*.hpp` (bible, songs, search, presentation, scene, media, content, display, production, recording, broadcast, automation, project, notification, adaptive, rendering, vgr) |
| Platform                   | `platform/include/platform/*.hpp` (+ `platform/linux/`, `platform/windows/`)                                                                                                                                 |
| Apps (reference consumers) | `apps/cli/main.cpp` (full demo of every API), `apps/remote/main.cpp` (IPC client)                                                                                                                            |

> `apps/cli/main.cpp` is the canonical "UI-less consumer": it boots the Kernel and
> exercises every engine's public API end-to-end. Read it as a reference implementation
> of the consumption patterns in §7.
