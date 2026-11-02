# The long-file audit — every file that is too big, what is tangled inside, and how it comes apart

The request behind it: *"audit the long files that have plenty of code in them, handling too many
concerns, making debugging difficult — break them down using shared modules, separate logic
concerns."* Not only `LiveOutputService` — the whole tree.

This is that audit. It measures first, names the **two different diseases** it found, maps what is
tangled inside each offender, and gives the decomposition as **an ordered list of extractions**,
each with the seam it introduces and why it is safe on its own. Nothing here is a rewrite: every
step is "move code that already exists behind a name", so each step reviews by itself.

Measure it yourself:

```bash
node tools/audit_long_files.mjs            # the deep table (default file list)
node tools/audit_long_files.mjs --all      # every file over 1 200 lines
node tools/audit_long_files.mjs --all 800  # a tighter threshold
node tools/audit_long_files.mjs qml/Main.qml
```

---

## 1. The inventory

**41 source files were over 1 200 lines; together 102 434 lines** — about 41 % of the
248 930 lines of `qml/` + `src/` (tests included). This table is the state BEFORE the first
split; §6 records what has moved since (re-run `node tools/audit_long_files.mjs --all` for the
current numbers). Function lengths are measured by **brace
matching**, not by "the next definition starts here": `ReferencePane.qml`'s `onOnAirChanged()`
measures 1 905 lines the crude way and 27 the honest way, because 1 900 lines of *other* code
follow it inside the same component. (Headers and the macro-style unit tests report 0 functions
— the scanner reads `function name(` / `Type Class::member(`; a header is a declaration list and
`tests_*.cpp` are tables of `TEST(...)` cases. Both are called out where they matter.)

| lines | funcs | worst | file |
|---|---|---|---|
| 10 475 | 213 | 120 | `qml/Main.qml` |
| 1 596 | — | — | `qml/services/LiveOutputService.cpp` (was 7 092; split in §6) |
| 6 092 | 88 | 161 | `qml/screens/edit/EditScreen.qml` |
| 5 628 | 116 | 148 | `qml/components/ReferencePane.qml` |
| 4 375 | — | — | `src/…/tests/unit/tests_styles.cpp` (tests) |
| 4 015 | 90 | 41 | `qml/screens/settings/AudioVideoScreen.qml` |
| 3 789 | 133 | 86 | `qml/screens/settings/RoutingScreen.qml` |
| 3 671 | 90 | **382** | `qml/services/EngineBridge.cpp` |
| 3 267 | 12 | **371** (+`AddStageBlocks` **449**) | `src/…/presentation/SceneBuilder.cpp` |
| 3 091 | 31 | **791** | `src/…/library/TheTableLibrary.cpp` |
| 2 630 | 31 | 34 | `qml/screens/main/VGRPresenterMainScreen.qml` |
| 2 550 / 2 448 | — | — | `tests_rendering.cpp` / `tests_core.cpp` (tests) |
| 2 432 | 45 | 151 | `qml/components/MediaLibraryPane.qml` |
| 2 062 | 20 | — | `src/…/platform/windows/WindowsVideo.cpp` |
| 1 879 | 74 | 167 | `qml/models/OutputListModel.cpp` |
| 1 829 | — | — | `qml/services/include/services/LiveOutputService.h` (declarations) |
| 1 695 | — | — | `src/…/core/events/Events.hpp` (event types) |
| 1 695 | 70 | 173 | `src/…/automation/FlowEngine.cpp` |
| 1 692 | 74 | 135 | `src/…/recording/RecordingEngine.cpp` |
| 1 653 | 92 | 169 | `qml/services/ShowService.cpp` |
| 1 638 | 39 | 77 | `qml/components/ShowCenter.qml` |
| 1 636 | 55 | 216 | `qml/models/BusListModel.cpp` |
| 1 558 | 65 | 130 | `src/…/bible/BibleEngine.cpp` |
| 1 551 | 52 | **408** | `src/…/rendering/RenderEngine.cpp` |
| 1 521 | 30 | 251 | `src/…/platform/windows/WindowsAudio.cpp` |
| 1 516 | 23 | 25 | `qml/screens/settings/GeneralScreen.qml` |
| 1 482 | 33 | **280** | `src/…/presentation/LiveOutputController.cpp` |
| 1 451 | — | — | `qml/components/IconGlyph.qml` (icon table) |
| 1 427 | — | — | `src/main.cpp` |
| 1 401 | 14 | 66 | `qml/components/OutputMonitorTile.qml` |
| 1 389 | 48 | 191 | `qml/services/TheTableService.cpp` |
| 1 380 | — | — | `tests_presentation.cpp` (tests) |
| 1 371 | 73 | 147 | `src/…/production/ProductionGraph.cpp` |
| 1 351 | 42 | **549** | `src/…/search/SearchEngine.cpp` (`RawSearch`) |
| 1 336 | 54 | 181 | `qml/services/ScriptureService.cpp` |
| 1 316 | 9 | 103 | `qml/components/DesignPreview.qml` |
| 1 305 | 22 | **308** | `qml/services/RoutingService.cpp` (`refresh`) |
| 1 289 | — | — | `src/…/rendering/D3D11Backend.cpp` |
| 1 244 | — | — | `tests_bible.cpp` (tests) |
| 1 212 | 20 | 30 | `qml/screens/settings/StylesScreen.qml` |

---

## 2. The two diseases (and why "split the functions" is only half the answer)

**A. A handful of giant FUNCTIONS.** These are the files where debugging means finding the line
in a five-hundred-line body with a dozen early returns:

| function | lines | file |
|---|---|---|
| `TheTableLibrary::Search` | **791** | `library/TheTableLibrary.cpp:2000` |
| `SearchEngine::RawSearch` | **549** | `search/SearchEngine.cpp:581` |
| `AddStageBlocks` (free) | **449** | `presentation/SceneBuilder.cpp:913` |
| `RenderEngine::DrawTextObject` | **408** | `rendering/RenderEngine.cpp` |
| `LiveOutputService::stageSlides` | **385** | `services/LiveOutputService.cpp:985` |
| `EngineBridge::startRelay` | **382** | `services/EngineBridge.cpp:2994` |
| `SceneBuilder::ComposeSlide` | **371** | `presentation/SceneBuilder.cpp:2264` |
| `RoutingService::refresh` | **308** | `services/RoutingService.cpp` |
| `EngineBridge::enumeratePlatformDevicesAsync` | 292 | `services/EngineBridge.cpp:635` |
| `LiveOutputService::styleRunLinesForText` | 289 | `services/LiveOutputService.cpp:1981` |
| `LiveOutputController::RenderOnce` | 280 | `presentation/LiveOutputController.cpp` |
| `AddCompositorLayers` (free) | 272 | `presentation/SceneBuilder.cpp:1723` |
| `BusListModel::create` | 216 | `models/BusListModel.cpp:31` |
| `LiveOutputService::applyReadingRestage` | 239 | `services/LiveOutputService.cpp:3327` |
| `WindowsAudio::MeterThread` | 251 | `platform/windows/WindowsAudio.cpp` |

**B. Giant FILES with no giant function.** `Main.qml` (10 475 lines, 213 functions, worst 120),
`RoutingScreen.qml` (133 functions, worst 86), `AudioVideoScreen.qml` (90, worst 41),
`EditScreen.qml` (88, worst 161), `ReferencePane.qml` (116, worst 148), `Main.qml`'s siblings
in the models (`OutputListModel` 74 functions, `BusListModel` 55). Nothing inside is individually
unreadable — the *coexistence* is: 213 functions on one `Window` share one `id` namespace, one
property surface and one 10 000-line diff. Splitting a function fixes nothing here; separating the
concerns does.

**C. A third, quieter one: the 1 829-line HEADER.** `LiveOutputService.h` declares the whole class
in one place with seventeen `// ---- …` banners — properties, mutexes, thread handles, reading
run state, NDI plan, media state, blackout undo, self-test hooks. The header is the map of the
class; at this size it stops being a map and becomes a second file to read.

---

## 3. What is tangled inside the worst offenders

### `qml/services/LiveOutputService.cpp` — 7 092 lines, one class, seventeen concerns

The header already admits it with its banners. The .cpp's definitions cluster like this:

| concern | where | size |
|---|---|---|
| NDI program sender: worker thread, plan hand-over, send clock, roster recheck, rate check, listen/silence | 452–768 **and** 6215–6967 | ~1 070, in **two disjoint halves** |
| Go live / stop / blank carrier / blackout / program playout | 789–985, 4830–5063 | ~460 |
| Reading run: lines, prewarm, text source, page, char span, style runs, fit, column width, page base, restage, glide, line step | 1370–3330 | **~1 960 — the largest concern** |
| Taken input / preview / NDI frame pump / failure state | 3849–4519 | ~670 |
| Bus video windows (`refreshBusFeeds` alone: 187) | 4241–4471 | ~230 |
| Media on air: preflight, take, apply, fit, transport | 5063–5483 | ~420 |
| Wall transport: next/previous/step/jump/steppers | 5483–5647 | ~165 |
| Feed health + poll tick telemetry (`sampleFeedHealth` 176) | 5813–6215 | ~400 |
| Overlays on air | 3976–4030 | ~55 |
| Env self-test | 4691–4830 | ~140 |

The two NDI halves are the clearest evidence of accidental coupling: the *sender* is defined at
the top of the file and the *worker/plan/clock* at the bottom, 5 500 lines apart, both touching
the same five members. Reading "what does GO LIVE do to the NDI feed" means jumping between them.

### `src/…/library/TheTableLibrary.cpp` — `Search()` is 791 lines

One body does, in order: needle normalisation, citation parsing, per-book scan, ranking, snippet
extraction, part/chapter walk, and hit assembly — all while holding the store lock it takes
itself. This is *the* debugging surface for "search is wrong", and it is unsteppable as written.
(`SearchEngine::RawSearch`, 549 lines, is the same disease in the other search path.)

### `qml/services/EngineBridge.cpp` — 3 671 lines, five subjects

`boot()` (188), device enumeration (`enumeratePlatformDevicesAsync` 292 +
`enumerateDevicesOffThread` 103), the plugin/NDI feature family (`refreshPluginFeaturesOffThread`,
`applyNdiFeatureState`, firewall probes), meters (`startInputMeter` … `stopAllInputMeters`), and
the whole preview/tap/relay family (`startVideoPreview`, `startScreenPreview`, `startNdiPreview`,
`openTapOffThread`, `startRelay` **382**, `releaseStreamTap`). Five modules whose only shared
thing is "talks to the engine".

### `src/…/presentation/SceneBuilder.cpp` — 3 267 lines: policy + ~1 900 of helpers

The bulk is *translators*: `AddStageBlocks` (449), `AddCompositorLayers` (272),
`RefreshBusPlanes` (90), plus `ComposeSlide` (371), `BuildSlideScene` (257). The painting rules
live in an anonymous namespace next to the slide-composition policy.

### The QML screens

`Main.qml` is the app's shell **and** its self-test harness **and** its action table **and** its
event router: `runActionById` (a dispatch table), `onEventPosted` (a topic router), `metaFacts`,
plus **210 `selfTest*` scenario objects and 426 `SelfTest.` driver calls** spread from line 264 to
the last line of the file — scenarios that ship in the production file behind env-var gates.
`RoutingScreen` (133 functions) and `AudioVideoScreen` (90) are each a screen *plus* a graph
editor *plus* a connector model. `MediaLibraryPane` (45 functions, worst 151) is a pane plus a
thumbnail pump.

### The models

`OutputListModel.cpp` (1 879, 74 functions, `reloadFromStore` 167) and `BusListModel.cpp` (1 636,
55 functions, `create` 216) each carry the model's roles **and** the store round-trip **and** the
persistence of view state. `RoutingService::refresh` (308) is the routing graph's whole rebuild in
one body.

### The platform layer

`WindowsVideo.cpp` (2 062) and `WindowsAudio.cpp` (1 521, `MeterThread` 251) hold device
enumeration, format negotiation, the capture loop and the meter tap in one translation unit each.
`RenderEngine.cpp`'s `DrawTextObject` (408) holds the glyph cull, the three passes, the atlas
upload and the reading-flow viewport in one function.

---

## 4. The decomposition — ordered by (benefit ÷ risk)

Each step names **the module**, the **seam** it introduces, and **why it is safe on its own**. The
first two are mechanical; the last few need care.

### Step 1 — `qml/js/` : pure logic out of the QML files (no component moves)

QML can `import "js/foo.mjs" as Foo`. Everything that is a **pure function of its arguments**
leaves first: `metaFacts`, the fit helpers, `tfLine`/`tpLine` formatting, `layoutProbe*`, the
`runActionById` switch (→ a data table of `{ id, run }`), `onEventPosted`'s topic switch (→ a
route map), `RoutingScreen`'s `menuFor` / `pairOk` / `busChannelMix`, `AudioVideoScreen`'s
`connectorPairs` / `dropRefusalTextFor`, `EditScreen`'s `canvasMenuModel`.

*Seam:* plain `.mjs` modules imported by QML — testable with `node`, no Qt, no ids.
*Why safe:* nothing in the object tree moves, so no binding can break. Collapses 4 files by
roughly 1 500 lines and gives the logic a place to be unit-tested.

### Step 2 — `TheTableSearch` and `SearchRanking` : the two search bodies

`TheTableLibrary::Search` (791) → `modules/library/TheTableSearch.{hpp,cpp}` with
`ParseNeedle()`, `ScoreHit()`, `MakeSnippet()`; the library keeps the lock and orchestrates (~120
lines). `SearchEngine::RawSearch` (549) → `modules/search/SearchRanking.{hpp,cpp}` for scoring and
`SearchScan` for the walk.

*Seam:* pure functions on plain data (needle, text, citation) → no lock, no store, no engine.
*Why safe:* both searches already have unit tests that pin behaviour; a pure function can be
tested hit-for-hit.

### Step 3 — `NdiFeed` : the sender out of `LiveOutputService` (~1 070 lines)

`qml/services/NdiFeed.{h,cpp}` owns exactly what is scattered today: plan hand-over, worker +
`QThread`, send clock, listen/silence, frame + audio send, roster recheck, rate check.
`LiveOutputService` keeps the QML-facing properties and delegates.

*Seam:* `arm(plan) / disarm() / stats()` — one direction of calls, and the worker thread already
belongs to this concern alone.
*Why safe:* the thread boundary does not move, so the lock/thread contracts the guards check do
not move either. This is a move, not a redesign.

### Step 4 — `ReadingRun` : the reading column out of `LiveOutputService` — **done**, as `LiveOutputReading.cpp` (§6)

Move the run state and its computations (`readingLineForStyle`, `readingCharForScroll`,
`readingPageLineCount`, `styleRunLines*`, `noteReadingFit`, `rebuildReadingRun`, `shiftPickBase`,
`readingWindowOffset`, `applyReadingScroll`) into `qml/services/ReadingRun.{h,cpp}`; the service
keeps the **commit** path (`applyReadingRestage`, `lineStep`) and calls it.

*Seam:* `attach(flow, styleId, fit)`, `lineFor(char)`, `offsetFor(line)`, `pageLines()` — one
value in, numbers out.
*Why safe:* those functions are already pure computations over members; extraction is a struct
holding them.

### Step 5 — `SlideComposer`, `BlockPaint`, `CompositorLayers` (SceneBuilder)

`ComposeSlide` (371) → `SlideComposer`; `AddStageBlocks` (449) and `AddCompositorLayers` (272) →
`presentation/BlockPaint.{hpp,cpp}` and `presentation/CompositorLayers.{hpp,cpp}`. `SceneBuilder`
keeps the **policy** (`PlanBlockLayers`, the `Build*Scene` entry points, `RefreshReadingFlow`).

*Why safe:* the painters are already free functions in an anonymous namespace — they move with
their helpers and lose nothing but visibility.

### Step 6 — `MediaLayer`, `StageInputs`, `BusWindows` (LiveOutputService) — **done**, as `LiveOutputMedia.cpp` / `LiveOutputInputs.cpp` (§6)

Three extractions of state + its own pump: media on air (take/preflight/apply/transport), taken
inputs (take/preview/failure/pump), bus windows (`refreshBusFeeds`/`publishBusFeeds`). All three
already push into `CompositorState` — the shared module they should keep using rather than
growing a second path.

### Step 7 — `EngineBridge` → `PlaybackProbes`, `Meters`, `PreviewTaps`

`startRelay` (382) belongs with `openTapOffThread`/`releaseStreamTap` in `PreviewTaps`; device
enumeration + plugin/NDI feature probing become `PlaybackProbes`; the five meter entry points
become `Meters`. What stays in `EngineBridge` is boot, history (undo/redo) and the event pump —
genuinely one subject.

### Step 8 — the models: `OutputListModel` / `BusListModel` role + store split

Each model keeps its roles and view state; the **store round-trip** (load, save, restore,
`reloadFromStore` 167, `create` 216) moves to a `*Store` helper that takes and returns plain
rows. That is the shape `TheTableService`/`ScriptureService` already use for their async paths.

### Step 9 — `RoutingService::refresh` (308) → graph build + status resolve + notify

Three concerns in one body: read the roster, resolve the graph's edges/pairs, and publish status
colours. `RoutingGraph` (pure) + `RoutingStatus` + the service's notify.

### Step 10 — the platform layer: `WindowsVideo` / `WindowsAudio`

Device enumeration + format negotiation (`Capabilities`), the capture loop (`CaptureLoop`) and the
meter tap (`MeterTap`, `MeterThread` 251) become three TUs each. The PAL interface does not change,
so no consumer moves.

### Step 11 — the self-test scenarios out of `Main.qml`

Every `selfTest*` block is already env-gated. They move to `qml/selftest/Scenarios.qml` (one
`QtObject` the window builds only when `SelfTest` is defined). `Main.qml` loses its largest
unrelated tenant — 210 objects and 426 driver calls — and the scenarios gain a file that can be
read without scrolling past a shell.

---

## 5. What is deliberately NOT split

* **`ShowService`** (1 653 lines, 92 functions) — one-line delegations (`addBlock`, `moveSlide`,
  `updateTemplate`) onto an engine that owns the document. That is the *right* shape, and the
  model every extractions above copies: **service thin, engine the owner**.
* **`IconGlyph.qml`** (1 451) — an icon path table. Data, not logic.
* **`Events.hpp`** (1 695) and the `tests_*.cpp` files (1 244–4 375) — a type list and case
  tables. Long is their job.
* **`MonitorWall.qml` / `OutputMonitorTile.qml`** — long because they are one component with a lot
  of documented rendering nuance; their functions are already short (max 9 and 20 lines).
* **The `// ---- section` banners themselves** — they are why this audit could be written at all.
  One concern with ten named sections is not the disease.

---

## 6. What has been split (first pass, done)

The scene builder went from **one 3 267-line file** to four named modules, each with
its own header and a stated rule for what belongs in it:

| module | owns | lines |
|---|---|---|
| `presentation/BlockMeta.hpp` | reading a design's `metaJson` (the probes were private to SceneBuilder) | 144 |
| `presentation/MediaPixels.{hpp,cpp}` | a placed media file: decode + cache, its own values, the pixel work, fit rects | 92 + 467 |
| `presentation/SlideBinding.{hpp,cpp}` | what a slide SHOWS as data: binding, placeholders, blank-composition test, block fingerprint | 69 + 226 |
| `presentation/ScenePaint.{hpp,cpp}` | how blocks become draw commands: the block pass, the fit solver, video/bus/overlay layers | 121 + 1 153 |
| `presentation/SlideComposer.cpp` | policy: which template/own-blocks a slide is made of | 426 |
| `presentation/SceneBuild.cpp` | the entry points: build a slide into a named engine scene, the reading flow, presets | 731 |
| `presentation/SceneBuilder.cpp` | what is left: the backdrop rule, colour parsing, scene ids | **270** |

`LiveOutputService.cpp` **7 092 → 5 970 lines** with the NDI feed in its own module:
`qml/services/LiveOutputNdi.cpp` (1 196 lines) now holds the sender, the worker, the plan
hand-over, the send clock, the roster recheck and the rate check **next to each other** (they
used to sit 5 500 lines apart), and `services/NdiLog.hpp` declares the logging category so
any TU can log through it. `preferredNdiOutput` — the one file-local helper the NDI code
reached for — moved with it.

The **second pass** took the same file the rest of the way, by concern:

| module | owns | lines |
|---|---|---|
| `services/LiveOutputService.cpp` | the presses and the live loop: GO LIVE / STOP, staging, clear/restore, navigation, the poll, feed health, the preview frame feed | **1 118** |
| `services/LiveOutputReading.cpp` | **the reading run**: the measured line table (block pick, font, fit, wrapped spans), `StyleRunLines` per style, rebuild/rebase/apply/animate/drain, `setScrollMode`, `lineStep` | 1 953 |
| `services/LiveOutputInputs.cpp` | **the input layers**: a taken camera/screen/NDI feed, card previews, bus video windows, per-input health, and the env self-test that drives all three | 755 |
| `services/LiveOutputMedia.cpp` | **media on air**: pre-flight off the disk, the decoder, the frame hand-off, fit + transparency, transport | 460 |
| `services/LiveOutputNdi.cpp` | the NDI program sender (first pass) | 892 |
| `services/SlidePreviewMap.cpp` | the engine composition → the QML preview shape — **shared** by all of the above | 157 |

Those numbers follow a second, deliberate cut inside the reading run: the four bodies that were still long are now named steps — `applyReadingRestage` (239 → 40, with `RebuildReadingRunUnderCommit`, `DeriveReadingScrollFromRun`, `PublishReadingFlowOnly`, `CommitReadingPageOnAir`, `StageReadingPageOffAir`), `rebuildReadingRun` (213 → 30, with `BuildReadingRunText`, `CountReadingRunPageLines`, `LogReadingRunShape`, `RebaseReadingWindow`), `noteReadingFit` (203 → 40, with `ReadingFitKeepsWalkColumn`, `PurgeReadingTablesForStyle`), `lineStep` (162 → 45, with `LogLineStepRequest`, `LineStepOwnLines`, `OwedLineStepAnswer`, `MoveLineStep*Page`) — and `styleRunLinesForText` (185 → 58) lost its three copied cache/evict/build loops to `CachedStyleRunLines`, `CacheStyleRunLines`, `ScrollColumnKey`, `ReadingBoxForPage`, `ReadingPageSizeFor`, `QueueScrollColumnMeasure`.

A second batch took the bodies that were still long in the other modules: `sampleFeedHealth`
(136 → 53, with `ClearFeedHealthWhenIdle` / `FeedBufferForRow` / `PublishOutputRuntime` /
`StalledOutputsFrom` / `AnnounceFeedStalled` / `AnnounceFeedRestored`), `refreshBusFeeds`
(135 → 13, with `FeedForSourceRow` / `FeedForBusRoute` / `CollectBusFeedWants` /
`AddTemplateWindowsFor` / `ReleaseStaleBusFeeds` / `ClaimWantedBusFeeds` / `EnsureBusFeedPump`),
`applyMediaTake` (106 → 29, with `EnsureMediaPlayer` / `ArmMediaTake` / `StartMediaPlayback`)
and `recheckNdiPlanOffThread` (114 → 30, with `CollectNdiSendRequests` / `LogNdiRoster` /
`WarnSilentNdiOutputs` / `ForgetRetiredNdiProviders` / `AnnounceNdiOutputStates` /
`StopNdiWithoutPlan` / `StartNdiPlanProbe`, plus a named `NdiSendRequest` replacing the
function-local `Request`). `clearInput` was on the list but measures 21 lines — the earlier
number came from an estimate that ran into the next function; there was nothing to split.

The third batch took the three bodies still long in the NDI and input modules: `runEnvSelfTest`
(102 → 15, with `PickSelfTestInputLabel` / `RunInputTakeProbe` / `RunInputReactivateProbe` /
`RunMeterProbe` / `CheckMeterProbe`, which also made the four nested `singleShot` lambdas
ordinary members), `checkNdiFeedRate` (104 → 20, with `PruneNdiRateWindows` /
`MeasureNdiRateWindow` — returns whether a window closed, and whether it recovered by itself — /
`ResetNdiSenderForRecovery` / `ReportNdiRateStep`) and `applyNdiPlanProbe` (95 → 18, with
`ActiveNdiOutputNames` / `BuildNdiPlan` / `AddNdiPlanFor` / `MakeNdiSendPlan` /
`AnnounceNdiSending` / `LogNdiProbeResults`). `MakeNdiSendPlan` returns a nested type by
qualified name; `BuildNdiPlan` returns the `NdiPlanBuild` the header already declared, so the
per-output loop and the state it accumulates are separate concerns.

### Dead declarations, pruned

A declaration audit over `LiveOutputService.h` — 220 function declarations after signals, slots and
`Q_PROPERTY` blocks are set aside, each checked against a definition in the tree and against every
`.cpp` / `.hpp` / `.qml` / `.js` mention — found four members with no reachable caller, all
removed: `previewFrame(QSize)` (declared only, never defined — a call would have failed at link),
`confirmInputFrame` (a `Q_UNUSED` no-op `Q_INVOKABLE`), `onAirSlideAt(index)` (21 lines that
scanned the on-air snapshot for the nth visible slide; the `onAirSlide` property already covers
what QML reads) and `ndiConfiguredFps()` (31 lines reading the preferred NDI row's refresh rate;
the live plan reads each output's own `fps` per probe instead). Every signal in the header is
emitted somewhere.

One finding is left in place because removing it and reviving it are different products:
`prewarmReadingLines()` is unreachable **because its scheduler is broken** —
`schedulePrewarmReadingLines()` sets `readingPrewarmQueued_ = true` and then calls *itself*, so the
recursive call hits its own guard and returns. The queued flag stays true for the life of the
object, and the one other call site (`LiveOutputReading.cpp:1707`) is a no-op from then on. The
intent was clearly a `QTimer::singleShot(0, this, &LiveOutputService::prewarmReadingLines)`. So the
reading-line cache prewarm (warm ±`kReadingPrewarmRadius` slides around the current one) has never
run. Fix the hop to turn it on; delete `prewarmReadingLines` / `schedulePrewarmReadingLines` /
`readingPrewarmQueued_` / `kReadingPrewarmRadius` and the call site to drop the idea.

### The repetition audit

A run detector over the five service TUs (runs of >= 4 identical, whitespace-normalised code lines,
reported once per pair, longest first) found the repetition the splits had left behind. Five shared
helpers now serve it:

* **`StartTap` / `StartTapKeepingCamera` / `StopTap`** (file-local in `LiveOutputInputs.cpp`) — the
  kind -> `EngineBridge` dispatch existed **nine times**: start and stop, each with the
  screen/ndi/camera branch, for the three roles an input can be held in (`output`, `card`, `bus`).
  `role` is now a parameter. The asymmetry is deliberate and kept: a bus claim honours a camera's
  refusal (so the retry backoff arms), while a taken input and a card preview do not, because
  camera refusals surface through empty frames — hence the second, named entry point rather than a
  bool parameter.
* **`EnsureInputPump`** — the 16 ms pump's create-and-start block was copied at three sites
  (`takeInput`, `previewInput`, `EnsureBusFeedPump`).
* **`AddWantedBusFeed`** — `CollectBusFeedWants` ran the same "register the bus number, skip what is
  already wanted, resolve its route" block twice, once over the on-air slide's blocks and once over
  the output rows. 24 lines -> 12.
* **`ResetReadingPick`** — "a new pick" reset the run, the rebase origin, the scroll and its
  animation in three places; two of them called `resetReadingRun()` **twice**, the second call after
  nothing that feeds it (it only clears the run fields), so the duplicate went with the extraction.
* **`ClearReadingTextSource`** — the nine-field passage reset (`lineSourceTextOnly_`, the page/end/
  text triples, the start char, the pending-activation pair) at three sites.
* **`SpansToRunLines`** — the wrapped-text -> `{lines, charStarts}` build, copied in both cache
  paths of `styleRunLinesForText`.

`readingBoxForWrapping` and `readingBlockOf` were the same block scan with a different ranking
rule (largest area vs longest non-empty text); they now share `readingBlockScored(slide, score)`,
where a score of 0 or less disqualifies a candidate and the first one wins a tie — which is exactly
what both copies did, so the two names survive as one-liners that say *which* rule they use.

Left in place: the two run-lines cache eviction loops (`.cpp:1031` / `.cpp:1163`) differ only in
which map and bound they use.

The fourth batch was the inputs module: `takeInput` (53 → 30, with `InputKindHasTap` /
`StopPreviousInputTap` / `StartInputTap` / `StartInputPump` — validate, release, acquire, prime)
and `pumpTick` (59 → 19, with `SampleTakenInput` / `SampleCardPreviews` /
`PushTakenInputFrame` / `PushBusFeedFrames`, one step per source family, each answering whether
it moved). `pumpTick` also lost a dead condition: it compared `producing_.size()` against a value
captured after every insertion that could change it, so `producing_.size() != producedBefore` was
always false and only `cardMoved` ever mattered.

### The prose went tree-wide

A comment audit over `src/` + `qml/` (606 files) found **49 245 comment-only lines, 38 134 of them in runs of 3 or more** — the "what this used to do, what was measured, what it broke once" essays. The rule applied: a run of **3+** comment lines is removed; a one-line note stays (it explains the next line); section banners stay; `gui-ok:` / `lock-ok:` / `NOLINT` / SPDX lines are protected; comment lines inside a raw string or a macro continuation are never touched (a shader source's own `//` lines are code).

Result: **49 245 → 1 289 comment-only lines** (1 017 of the survivors are in `tools/`, which was left alone — those comments *are* the guards' rule documentation). Largest single cuts: `Main.qml` 2 389, `ReferencePane.qml` 1 593, `EditScreen.qml` 1 578, `LiveOutputService.h` 1 009, `AudioVideoScreen.qml` 877, `OutputMonitorTile.qml` 638, `WindowsVideo.cpp` 607.

Verification: every touched file was compared against a pre-strip snapshot with comments and blank lines ignored — **code byte-identical in 606/606 files**, so this is a pure deletion. The rationale the comments carried lives in this audit, `docs/scroll_mode_thread_audit.md`, `docs/gui_thread_audit.md` and the history, not inline.

The two pieces of state that two modules both need are now named rather than file-local:
`services/PressTimeline.h` (a press of GO LIVE **or** STOP: what it cost and the media frames it
waited behind — it was called the “GO LIVE trace”, which was wrong twice, since it also traces
stop and it is a timeline) and `services/MediaOnAirState.h` (the take’s fit and its real alpha —
owned by the media module, read by the preview map). `SlidePreviewMap` exists because the
reading run and the input layers both need it: a second copy of that mapping is how a tile and a
real output start drawing two different pictures for the same slide.

The reading run is the one split by *behaviour* rather than by data flow: everything a scroll
bug can be in — what a line is, where the window sits, what a step does to it — is now in one
file, so debugging it no longer means searching a file that also decodes video and drives NDI.
That is the split that matters here: **the file a bug hunt opens**.

Verification done here: every new/changed TU was compiled with its own project flags
(`-fsyntax-only`, no build directory touched) — BlockMeta/MediaPixels/ScenePaint/SlideBinding/
SlideComposer/SceneBuild/SceneBuilder and LiveOutputService/LiveOutputNdi/LiveOutputReading/
LiveOutputInputs/LiveOutputMedia/SlidePreviewMap all clean, and all three guards (lock scope,
GUI-thread, settings keys) report the same numbers as before the split. A member-level diff of
the old service against the five new TUs shows **no definition lost and none duplicated**. The
link needs the new sources in `build.ninja`, i.e. one CMake re-configure (ninja does it by
itself when `CMakeLists.txt` changes).

## 7. Structure: shared vs OS-specific

The split above is deliberately portable. The rule it follows, which every further
subdivision should keep:

* **Shared (writes once, every OS uses it):** `modules/**` (engine logic, this pass added four
  modules), `core/**`, `interfaces/**`, `qml/**` (the whole UI), and the PAL *interfaces* in
  `platform/include/**`.
* **OS-specific (the only thing rewritten per platform):** `platform/windows/**`,
  `platform/macos/**`, `platform/linux/**` — and nothing above them. A new OS supplies a
  `IVideo`/`IFilesystem`/`IAudio` implementation and a renderer backend; it does not need to
  touch `ScenePaint`, `MediaPixels` or `SlideBinding`, because those reach the platform only
  through `PlatformAccessor` and `media::MakePlatformFrameSource()`.
* **The test for a new file:** if it needs a Win32/COM/GDI symbol, it belongs under
  `platform/<os>/`; if it names a `rendering::`/`presentation::` type and a PAL interface, it
  is shared. `MediaPixels.cpp` is the worked example: file + image decode through the PAL,
  pixel work in plain C++ → portable as written.

Placeholder scaffolding was removed with this pass: 18 README-only stub directories
(`core/{cache,common,devices,diagnostics,network,performance,security}`, `docs/design`,
`include`, `modules/{ai,cloud,midi,ndi,remote,streaming}`, `scripts`,
`tests/{integration,performance}`) — every one of them had a zero-byte README and no code.
`docs/api/README.md` was kept: it carries real notes.

## 8. The guard (proposed, not built)

This repo already fails the build on three classes of silent mistake (lock scope, GUI-thread
blocking, settings keys). The same shape fits here, and without it these files grow back:

`tools/check_file_size.mjs` — per-file and per-function budgets read from a **baseline file**,
failing only on **growth** (`Main.qml 10475/10475`, `LiveOutputService.cpp 7092/7092`, …), so
today's state passes and every extraction ratchets the number *down*. Targets to ratchet toward:
**1 200 lines per file, 120 per function** — the shape `ShowService` and `MonitorWall` already
meet.
