# Scroll mode: the threading audit

**Report:** *"The Table now works, and I want a deep dive into it, thread-wise — the app
was lagging seriously, which it never did before. It did not even work when my laptop was
under stress, it just froze."*

This is the audit of what Scroll mode does to the threads, measured on this build
(2026-11-01). It is written to be read beside `docs/gui_thread_audit.md` (the whole-app
audit) and the `tools/selftest/` scenarios; the guards those documents describe both pass
today, so nothing here is a single blocking call the scanners can see — it is the *shape*
of the scroll path.

## 1. What the mode actually changes

`Alt+Down` / `Alt+Up` are gone (`qml/settings/Hotkeys.qml`, the `previousSlide` note). The
wall's transport switch — `selfTestWallScrollMode` in `qml/components/MonitorWall.qml:770`
— decides what its ‹ › do:

| mode | ‹ › call | what the outputs are given |
|---|---|---|
| **Slide** | `LiveOutputService.stepPassage(±1)` | the page as authored; no passage attached to the slide |
| **Scroll** | `LiveOutputService.lineStep(±1)` | the passage, attached as `Slide::flowText` + a character offset |

Only Scroll attaches a passage (`LiveOutputService::setSlideReadingFlow`, gated on
`scrollMode_`, `qml/services/SlidePreviewMap.cpp:192`), and `setScrollMode` re-stages
the pick when it flips (`:1400`). So the mode is not a view preference: it selects a
different render path.

## 2. The path, thread by thread

One press in Scroll mode, and every hop it makes:

```
QML ‹ ›  (GUI thread)
  └─ LiveOutputService::lineStep            :qml/services/LiveOutputReading.cpp:2655
       ├─ activatePendingTextSource          (text-only picks)
       ├─ commitPendingRebase                :2944
       ├─ LineWalk::step                     (pure arithmetic)
       ├─ applyReadingRestage                :3039   GUI THREAD
       │    ├─ rebuildReadingRun / styleRunLinesForText       :1760  (pool worker for
       │    │                                                        text-only picks,
       │    │                                                        synchronous otherwise)
       │    ├─ SetFlowChar  → two atomic stores, no lock       (LiveOutputController.hpp:142)
       │    ├─ QTimer::singleShot(0, prewarmReadingLines)      :3115  → 9 pages × styles
       │    └─ page commit only: goLiveWithSlides             :764
       │         ├─ ShowConverter::slideFromVariant           (GUI thread, per slide)
       │         └─ LiveOutputController::StartFromSlides      :232
       │              └─ QUEUED to the render worker           :265-276
       └─ animateReadingScroll → applyReadingScroll (60 Hz)    :2857
            ├─ SetFlowChar                     (atomics)
            ├─ emit readingScrollChanged       (every tile + preview binding re-evaluates)
            └─ near a passage-window edge: refreshReadingTextWindow + styleRunLinesForText
               + previewWithActiveStyle / goLiveWithSlides    :2862-2900
                 (render thread)
                   RenderOnce                       LiveOutputController.cpp:710
                     └─ control_ held for the WHOLE body: compose → per-output passes
                        → Distribute → RenderPerOutputPasses              :893
```

Two facts from that map matter more than the rest:

* **The scroll position push is already clean.** `SetFlowChar` is two atomic stores
  (`LiveOutputController.hpp:142`, `FlowCharPush`), and the live *content replacement* is
  queued to the render worker precisely because the synchronous version froze the UI —
  the comment at `LiveOutputController.cpp:265` names `AppHangB1` as the measured symptom.
  The GUI thread does **not** wait on `control_` while scrolling.
* **Scroll mode disables the live loop's change-skip.** `RenderOnce` normally skips a
  frame whose scene and style have not moved. While a scroll is in flight it must not
  (`LiveOutputController.cpp:764`: `scrollMoving` forces a compose per tick, and
  `FlowCharMoving` keeps composing for 60 ms after the last push). So Scroll mode is the
  one mode that asks for a **full compose of every output, continuously**.

## 3. What that costs, measured on this build

The app's own telemetry, from `build/logs/engine.log` after a 46-press walk
(`tools/selftest/run_line_step_test.sh`):

```
live hotspot: Text · tplf1 — 37% of the composed frame (≈397.9 ms of it; ...)
live hotspot: Text · tplf1 — 37% of the composed frame (≈197.9 ms of it; ...)
live hotspot: Text · tplf1 — 38% of the composed frame (≈171.6 ms of it; ...)
```

* the reading column alone is **150–400 ms** of a composed frame, so a composed frame is
  **≈0.4–1.1 s** with the software renderer this build uses (the same 0.2–0.7 fps
  `run_videoperf_test.sh` measured in `tools/selftest/README.md`);
* the loop wants 60 Hz and composes every tick while scrolling, so the machine is
  saturated by the renderer for the length of the walk; the QML animation, the tiles and
  the interface thread all run on the same cores;
* **93 % of all log records come from the interface thread** — 280,450 of 302,325 lines
  in this log, 44 MB. The top tags are per-frame diagnostics: `RefreshOnAirSlide` 149,094,
  `ReadingDrawn` 45,314, `PreviewShows` 24,810, `StagedIsOurs` 10,783,
  `DrawsFlowChanged` 5,938, over 3,689 presses.

That is the lag: while the reader scrolls, this app rasterises roughly a frame a second
per output in software *and* writes a log record per tile per evaluation on the thread
that draws. A laptop that is also doing something else has no cores left for the
interface thread — which is what "it just froze" looks like from the outside.

## 4. Findings, worst first

### F1. The scroll bypasses the change-skip and rasters every output every tick — **FIXED (§7)**

`LiveOutputController.cpp:764-800` composes whenever the reading position is moving;
`control_` is held across the compose, **every per-output pass** (`:893`) and the
distribute. The per-output passes are one extra styled 1920×1080 raster per enabled
output, and the reading column is 37 % of each.

*Effect:* a walk is a continuous CPU raster at a rate the software back end cannot reach,
so the loop falls behind by seconds and never catches up until the walk stops.

*Fix (smallest first):* cap the compose rate while a glide is in flight — the NDI feed is
already paced by the output's configured rate, and no audience sees 60 fps of a 190 ms
glide. One frame per (1000/fps) ms during a move keeps the scroll smooth without
re-rasterising 37 % of the frame 60 times a second.

### F2. 93 % of the log is written by the interface thread, per frame — **FIXED (§7)**

`RefreshOnAirSlide` (149k), `ReadingDrawn` (45k, `DesignPreview.qml:131` on every
`onLineCountChanged`), `DrawsFlowChanged` (5.9k) and `StagedIsOurs` (10.8k) are INFO
records produced by QML bindings and the service on the GUI thread during a walk.

*Effect:* a formatted string and a queue push per tile per evaluation, on the thread that
must answer the pointer. It also makes `engine.log` 44 MB per session, which is its own
problem when it is the file operators are asked to send.

*Fix:* demote the per-frame diagnostics to trace level (or gate them on a
`VGR_TRACE_READING=1`-style flag the self tests set), keep the one-per-page and
one-per-press lines. The `LineStep` / `ReadRestage` records are the ones that answer
reports; the per-tile refresh is not.

*Done:* `ReadingDrawn`, `PreviewShows`, `DrawsFlowChanged` (`DesignPreview.qml`) and
`RefreshOnAirSlide` (`OutputMonitorTile.qml`) are now **trace**, so the default run drops them at
the logger's gate — no formatting, no queue push, no line. `VGR_LOG_LEVEL=trace` turns them back on
for both the console and the file. `LineStep` stays INFO: one per press, and it is the record a
report is answered from. `RenderOutputs`' frame-sanity line keeps its INFO level but is now gated on
the frame's *shape* (size / `allBlack` / scene) instead of its mean luma, which moved by a rounding
step on a still picture and made that line one per frame.

Measured on the last 8 MB of a real session (42 514 lines): those five tags were **26 117 lines
(61 %)**. `engine.log` also stops growing across runs: `FileSink` now counts the size already on
disk when it opens, so it rotates at its 16 MB cap with 3 backups instead of appending to whatever a
previous run left behind (the file was at 60 MB because only this run's own bytes were counted).

**Second pass — what else was still writing per frame.** With the bindings demoted, the live
60 MB file still had a tail of writers on the interface thread, and three of them had the *same*
bug the frame-sanity line had: their change gate compared a value that moves every frame.

| line | why it fired | now |
|---|---|---|
| `LiveOutputService: NDI probe result` (per feed, ~10 Hz) | signature included `sent=<frames>`, which grows on every probe | signature is `buffer/enabled/frame` only; `sent` still printed when the state changes |
| `livepreview frame sanity` (per preview frame) | gate compared `meanLuma`/`maxLuma` | gate on size + `allBlack`; the numbers print on a change |
| `ScrollFollow selection is now` (per walk step) | re-set to the same selection each step | one line per *different* selection; the walk's own moves are `ScrollFollow walk` |
| `Qt DirectWrite: CreateFontFaceFromHDC() …` (16xx a session) | Qt retries a legacy bitmap font on **every layout pass**, at WARN | one line per distinct failure |
| `UI event-loop heartbeat` (every 5 s, forever) | a *startup* probe that never stopped | stops after 60 s with one closing line; liveness is the hang watchdog's |

The gate itself was copied three times (a mutex, a map and a string compare per site), so it is
now one thing — `bps::logging::ChangeGate` in `core/logging/ChangeGate.hpp` — used by
`RenderOutputs`, the preview provider and the Qt message handler. Its header states the rule the
repeats kept breaking: *the signature must be the thing whose change is the news, never a
measurement that drifts*. `core/logging` is not in the lock-scope guard's default scope; the gate
holds its mutex only for a map lookup and a string assign, so it cannot park a thread on I/O.

**The verbosity is now a real preference.** `notifications.logLevel` (Settings · General · Log
level — "How much the engine writes to engine.log") used to move the *global* gate, which cannot
express that: a quiet file also silenced the console, and picking `debug` changed nothing in the
file because the file sink still filtered at Info. It now sets **that sink's own level**, so
`debug` really puts the app's debug lines in `engine.log` and `error` gives a quiet file without
taking the console away from a self-test. `LogLevelFromName` / `LogLevelEnvOverride` in
`core/logging/Logger.cpp` are the single mapping — `EngineBridge` no longer parses
`VGR_LOG_LEVEL` itself, and it still wins over any stored preference. The kernel's own
`engine.log.level` config key (seeded in `EngineDefaults` and, until now, never read) is applied
right after the `ConfigurationManager` opens, so boot lines obey it; the stored user preference
overrides it when the settings store loads.

### F3. A page commit re-commits the whole pick instead of moving the offset — **FIXED (§8)**

At a page boundary `applyReadingRestage` calls `goLiveWithSlides` (`:3225`), which queues
a **content replacement**: the render thread swaps the presentation, rebinds the runtime,
rebuilds every output's scene and drops the composed frame
(`ApplyPendingLiveContent`, `LiveOutputController.cpp:546`).

*Effect:* a full recompile plus a first-frame raster of every output lands exactly when the
reader crosses a paragraph — the one moment they are watching for smoothness.

*Fix:* the passage is already the same text across the window, so a page commit only
changes the *reference/title* blocks and the offset. Rebuild only what the page boundary
actually changes (or skip the recompile when the reading block's text is unchanged) — the
scene fingerprint already has what it needs to decide that.

### F4. Near a window edge, the 60 Hz animation does the expensive work — **the call site is fixed (§8)**

`applyReadingScroll` (`:2867-2900`): inside 1200 characters of the window's start or end it
refreshes the window, re-asks `styleRunLinesForText` and, if the window moved, calls
`previewWithActiveStyle` — or, live, `goLiveWithSlides` — **from the animation's own
callback**, up to 60 times a second.

*Effect:* a burst of exactly the work F3 describes, triggered by a *position* rather than
a page turn, while the reader is dragging the position.

*Fix:* the `pending` path already exists (`readingWindowRefreshPending_`); make it the only
path (refresh on the tick after the animation settles), or widen the window so a glide
cannot reach the edge.

### F5. The styled preview is composed on the GUI thread per restage — **FIXED (§10)**

`stagedSlide_ = previewWithActiveStyle(shown)` (`:3235`, and `:2916` from the scroll path):
a full styled composition for the active style, on the interface thread, for every page
commit and every window move when nothing is live.

*Fix:* reuse the per-page memo the tiles already keep, or compose on the thread pool and
assign on the way back (the `styleRunLinesForText` worker branch at `:1782` is the shape
to copy).

### F6. Prewarm bursts per press — **FIXED (§10)**

`QTimer::singleShot(0, &prewarmReadingLines)` (`:3115`) queues **nine pages × every style**
per press. Cached per page, so cheap — but on a stressed machine it competes with the
render thread for pool workers at the exact moment the next frame is being composed.

*Fix:* prewarm the next page in the direction of travel, and skip the pass while a
previous one is still running.

## 5. What is not the problem (checked, so it is not re-checked)

| claim | verdict |
|---|---|
| the scroll position push blocks the GUI thread | **no** — two atomic stores, no lock, no engine call (`LiveOutputController.hpp:142`) |
| a page commit waits on the render loop's lock | **no** — queued to the render worker (`LiveOutputController.cpp:265`); the synchronous version was the `AppHangB1` bug and is gone |
| the tile's frame snapshot is expensive | **no** — `LastFrameSnapshot()` is a `shared_ptr` copy under a short mutex; the 8 MB pixel copy is `Present()`'s, on the render thread |
| a module lock is held across a blocking call | **no** — `node tools/check_lock_scope.mjs`: clean, 418 files, 19 justified waivers |
| a QML-reachable entry point parks on a device/OS call | **no** — `node tools/check_gui_thread_blocks.mjs --all`: 0 new HARD. The 12 soft entries (`lineStep`, `stageSlideSet`, `readingLines`, `noteReadingFit`, `scrollColumnSize`) are the lock-acquire inventory, not device calls |
| a deadlock | **not found.** If one is suspected, `src/main.cpp:949` writes `UI event-loop heartbeat at N ms`; a **gap** in those lines is a parked interface thread, while lines that keep coming under a stalled picture are starvation (F1), not a lock |

## 6. The order to fix them in

1. **F1** — rate-limit the compose during a glide. This is the lag itself; everything else
   multiplies it.
2. **F2** — trace-level the per-frame log lines. Free win on the GUI thread and on the log.
3. **F3 / F4** — make a page commit (and a window move) an offset update, not a content
   replacement.
4. **F5 / F6** — move the styled compose and the prewarm off the interface thread.

## 7. What was fixed in this pass (F1's root cost + F2), and the numbers

**The reading block no longer pays for the whole passage on every frame.**
`RenderEngine::DrawTextObject` built one draw command per GLYPH of the flow text and
then threw the invisible ones away inside `ClipQuad` — so a scroll through a sermon
constructed the commands of every page, every frame, and drew one page. The three
passes (bands, shadow, glyphs) now iterate only the lines the viewport can see
(`firstLine`/`lastLine` computed from the clip rect and the scroll offset), with one
line of slack so a half-covered edge line still cuts exactly as before.

Measured with the engine's own `render dispatch:` line from the new
`rendering · reading-flow-draw-cost` suite (software backend, 400×200, six-line page):

| frame | draw commands | glyph commands |
|---|---|---|
| a 6-line passage (one page) | 127 | 126 |
| a **60-line** passage, scrolled to line 40 | **133** | **132** |
| the same 60-line passage at offset 0 | 127 | 126 |

Before the cull the two 60-line rows were the passage's whole glyph count (~1,300 per
frame) — a ten-fold cut in the text pass, which was 37 % of a composed frame. The suite
pins it: a long passage may cost at most twice a single page, and the frame must still
carry ink and differ between scroll positions (a "cheap" frame that drew nothing would
fail).

**The live loop no longer rasterises per tick while the words move.** `RenderOnce`
composes at most once per `kScrollComposeMs` (50 ms) during a move instead of per tick,
and the **per-output passes are skipped while the position is moving** — they catch up on
the settle frame (the one compose after the last push, `FlowCharMoving`'s own mechanism).
A frame that has not yet been composed at the final position is treated as a change
(`scrollUncomposed`), so the settle frame always lands: the outputs can never be left
on an intermediate offset by the budget.

**The interface thread stopped writing a record per tile per evaluation.**
`RefreshOnAirSlide` no longer logs on a memo HIT (149k records of "nothing changed" in
one session), `ReadingDrawn` and `StagedIsOurs` write on a change of their answer rather
than on every ask (45k and 10.8k). A 46-press walk now writes **1,501** engine-log lines
in total, against tens of thousands before; what is left is change-driven
(`RefreshOnAirSlide` 200, `ReadingDrawn` 53, `StagedIsOurs` 103 over 46 presses).

At the time of that pass the page commit still re-committed the pick (F3) and the styled
preview compose still ran on the interface thread per page (F5). F3 is fixed in §8; F5 and
F6 in §10.

## 8. The page commit is an offset update, not a new pick (F3, and F4's call site)

The passage IS one slide. In scroll mode `activatePendingTextSource` installs the whole
run as a single page (`lineSetRaw_ = QVariantList{passagePage}`, `lineSourceTextOnly_ =
true`), so every page boundary of a walk re-staged **the identical slide** — the same
blocks, the same title, the same page — with only `flowText`/`flowChar` moved. That used
to go through `goLiveWithSlides`, i.e. a live **content replacement**: presentation copy,
recompile, `SwapLiveContent` (runtime rebind + navigator + clock reset), every output's
scene rebuilt, composed frame dropped. All of it to move a number.

*What it is now.* `LiveOutputService::applyReadingRestage` (and the window-edge refresh in
`applyReadingScroll`, F4's call site) hand the slide to
`LiveOutputController::UpdateReadingSlide` instead. The GUI thread only queues it; the
**loop thread decides**, because only it may read the on-air content, and its answer is
one of two:

* **the same pick apart from the reading fields** (`SamePickApartFromReading`: title,
text, notes, tags, sections, assets, hidden, transition, duration, blocks, background,
filters, meta, category, clear flag — the id is excluded because `StartFromSlides`
generates it) — the on-air slide's `flowText`/`flowChar` are updated **in place** and the
scene that is already on screen is re-attached to the new window
(`SceneBuilder::RefreshReadingFlow`: the reading object keeps its box, its type size and
its page, and only re-reads the passage and the position). `lastRenderedScene_` is
cleared so the next frame is composed at the committed offset. **No recompile, no
runtime rebind, no scene rebuild, no dropped frame.**
* **a different picture** (slide mode's real page change, a new reference, a document
show on air) — the whole pick is re-committed, queued as a content replacement the same
way it always has been. The fallback is a QUEUE and not a call: `StartFromSlides` takes
the lifecycle gate, and `StopLive` holds that gate while it joins this very thread.

Per-output scenes need no extra bookkeeping: `RenderPerOutputPasses` resolves their ids
every frame, and it re-attaches any scene whose reading window moved before it composites
(`readingFlowWindowMoved_`, taken once per pass so a pass that is skipped during a move
leaves it set for the settle frame instead).

*Cost of a page commit now:* one `TextLayout::Measure` of the window per scene that draws
the flow, plus one compose — against a recompile and a rebuild of every output's scene.

*And the reverse move — the wall switching back to **Scroll mode** — has to put the passage
back on the slide.* It arrives with the passage byte-identical to the one the service
already had (`activatePendingTextSource` re-stages the run, `commitPage=false`), so the
`flowChanged` gate that guards the window move skipped it and the outputs stayed on the
page while the preview scrolled — measured by the table-walk scenario's own mode contract
(`mode=scroll step=3 slideFlow=0` against `readingFlow=2476`). `setScrollMode` now calls
`republishReadingFlow` on the way back in: the engine compares and no-ops when its slide
already carries this passage at this position, so the cost is a comparison.

*The other direction — **Slide mode** — sends an empty flow for the same slide.* `RefreshReadingFlow` now DETACHES the flow in that case
(`TextObject::ClearReadingFlow`): the block draws its page again, and every position
draws the same picture. Without it the scene that was already built kept scrolling the
passage the engine still had attached — the monitor preview returned to the page and the
NDI output did not. Switching back to Scroll re-attaches it to the same scene
(`ApplyReadingFlow` again, since the object no longer carries the flow mark).

*Residual, by design:* crossing into a **different reading unit** (the reference really
ticks over, a slide-mode page) still re-commits, because the scene's own blocks changed.
That is the "Rebuild only what the page boundary actually changes" half of F3's fix, and
it is the honest limit of "the passage is the same text across the window".

*And a staged copy cannot outlive the press that took it on air.* `goLiveWithSlides` clears
it now: a stale `stagedSlideRaw` is a page some readers draw in preference to the live
slide (the wall's own tile, the mode probe), which is how an on-air passage read as "0
chars" while the engine's slide really carried it.

*How to see which path ran,* in `engine.log`:

```
reading page kept on air: window moved (6000 char(s)), resting at 1240.00 — no content replacement
reading commit needs a new pick ('47-1100X'): queued as a content replacement
```

A walk inside one passage should print the first line and **no** `live output replaced
with content` between its pages; only a change of pick should print the second.

## 9. How to prove it

* **Under load, which is how it was reported:** `VIDEOPERF_HOGS=8` (see
  `tools/selftest/run_videoperf_test.sh`) is already the harness that runs the app against
  busy cores; a scroll variant that walks The Table's passage while the hogs run is the
  missing measurement — it should report the interface thread's heartbeat interval and the
  live loop's frame cost side by side, before and after F1.
* **Without load:** `tools/selftest/run_line_step_test.sh` now runs in Scroll mode (the
  mode switch is part of the scenario) and prints the per-press numbers; the composed-frame
  cost is in `engine.log` as the `Telemetry` hotspot lines quoted above. For §8's path,
  count `reading page kept on air` against `live output replaced with content` in the same
  log: the first should be one per page boundary of a walk, the second zero until the pick
  itself changes.
* **The unit suite:** `rendering · reading-flow-window-refresh` pins the in-place refresh
  — the scene keeps its id, the reading block re-reads the moved window, the page's own
  line count is kept, the page's top is the same picture and a position the old window
  clamped now draws. `rendering · reading-flow-draw-cost` still pins the frame cost.
* **The guards stay part of every build** (`check_lock_scope`, `check_gui_thread_blocks`,
  `check_settings_keys`) — this audit added no waiver and needed none.

## 10. The styled compose, the prewarm burst, and the passage that never followed the window

**F5 — the styled preview is composed once per PICTURE, not once per restage.**
`stagedSlide_ = previewWithActiveStyle(shown)` is a full template composition on the
interface thread, and a reading walk asked for one per page commit *and* per window move
while nothing was live. It was composing an identical map every time: `slideToVariantMap`
emits the title, the composed blocks, the background, the body, the fill and the style's
facts — the passage is **not** in it (`flowText`/`flowChar` are the slide's reading fields,
read from the RAW slide by every surface and by the engine). So
`LiveOutputService::readingStagedPreview` keeps the last composition and its picture, and
returns it unchanged when the only difference is where the reader is. The key covers the
picture (the slide minus its reading fields) plus the active style and the compositor's
take state (`live`, `mediaOnAir`, `mediaRev`, `inputLabel`) — the same inputs
`OutputMonitorTile`'s own slide memo keys on, for the same reason: a take changes what a
slide composes to without changing the slide.

*Cost:* a map comparison per restage instead of a composition; a genuine new picture still
composes exactly as before.

**F6 — the prewarm is spread over event-loop turns and queued once.** A pass over the
radius is up to nine `readingLines` compositions (a styled preview **plus** a real text
layout each), and all nine used to land in the turn right after a press — while the next
frame was being composed on the same thread. `schedulePrewarmReadingLines` now coalesces
the request (the restage, the scroll path's window move and `noteReadingFit`'s cascade all
land in the same turn), and `prewarmReadingLines` does **one** uncached slide per turn —
nearest first, ahead before behind, the radius itself unchanged — then re-arms. The loop
paints between them, so a cold pick warms up over several turns instead of in one stall,
and the walk never waits for the whole radius at once.

**The passage that never followed the window — found by the self test, not by the audit.**
`readingFlowText_` was assigned when a pick was STAGED and by the text-source window's
own refresh, and **never** by `rebuildReadingRun` — so for a pick of ordinary slide pages
(any pick longer than the run's radius: 9 pages here, 17 for the whole-pick rule) the flow
every surface draws stayed on the FIRST window while the page, the reference and the offset
all moved on. The wall kept drawing words the reader had left; in Scroll mode the engine's
slide carried that stale window too, so an output scrolled text from the wrong part of the
pick. The audit could not see it (it is not a cost, it is a correctness gap);
`run_line_step_test.sh` named it — from press 13 of a 24-page walk every tile read
`flow=false` with `flowLen` stuck at 398 while the tile itself advanced page by page.

`applyReadingRestage` now re-reads `readingFlowText_ = readingRunText_` after
`rebuildReadingRun` for that case (the text-source pick keeps its own windowed text), so a
slid window is a flow change like any other — which is exactly what routes it through
§8's cheap in-place update instead of being dropped.

**The stepper that stopped moving — the operator's "the scroll feature won't move".**
Two separate livelocks, both in `lineStep`'s own bookkeeping rather than in anything the
threads do:

* **An owed step was counted at the top of the pick.** A press that cannot move *yet*
  (the grid is not measured, the glide has not handed its offset over) is queued and
  played by `drainPendingReadingSteps`. At the passage's FIRST line that is wrong — it can
  never move in that direction — so every press there became a step the walk owed itself,
  and the queue kept them after the presses stopped. Measured by the table-walk
  scenario's rewind (400 `lineStep(-1)` at the first line): the walk then read
  `press 0 abs=61`, `59`, `56`, `54` … while pressing **forward**, i.e. the reader moves
  forward and the picture walks backwards. `lineStep` now refuses at the top
  (`direction < 0 && lineSlideIndex_ == 0 && readingAbsoluteLine() <= 0` → `false`) and a
  press in the opposite direction **cancels** what was owed instead of adding to it.
* **An unmeasured page owed every press instead of taking it.** For a text-only pick the
  page's line count comes from the run's per-page table; while that entry is missing
  `readingPageLineCount` answers 0, and every press was queued — the drain then played each
  one back into the same refusal, so the stepper sat there looking dead while the app was
  perfectly alive. `lineStep` now falls back to the RUN's own grid (the same grid the walk's
  positions are defined in, and the one already cached for the style) before deciding it
  cannot measure, so a press moves immediately.

**The harness had to be trustworthy before any of this could be judged.** Two changes in
`tools/selftest/run_line_step_test.sh` / `run_table_walk_test.sh`:

* **`SELFTEST_BUILD_DIR`** selects the tree under test (`build-agent/`), and every process
  is now matched **by its executable's path** instead of by name — the by-name stop took the
  operator's own running app down with it, and a survivor appended its own walk to the same
  log (that mixture is what produced `shows=-1 text="Probe 10"` on a walk whose tiles all
  read their page), then refused the run (`FAIL stale-instance`) rather than judging it.
* **The rewind is a timer, one step per event-loop turn.** `lineStep` needs a turn between
  presses (the previous press's offset is committed by the restage it triggered); 400
  presses inside one handler could not move at all and only filled the owed queue.

*Measured, same scenario, same build:*

| | before | after |
|---|---|---|
| press 45, `flowLen` | 398 (stuck at the first window) | **721** (follows the window) |
| presses with `flow=true` | 0 from press 13 | **all** |
| verdict | `FAIL findings=132` | **`PASS` — 46 presses (12 back up), one line a press, one passage, wall on the page, no skipped frame** |

Run from the isolated tree (`SELFTEST_BUILD_DIR=$PWD/build-agent`), so the operator's own
instance is never stopped or joined: `LINE_STEP: PASS 46 press(es) (12 back up) in Scroll
mode, one line a press, one passage, wall on the page, no skipped frame`.

## 11. Still open: the real-passage walk crashes the app

The synthetic walk passes repeatedly. The **real passage** walk
(`run_table_walk_test.sh`, The Table → `47-1100X`) does not finish: the app dies soon
after the passage is staged and the rewind begins. Windows records it as a crash, not a
stall:

```
00:45:50  appVGRPresenterUI.exe — Faulting module Qt6Core.dll, exception 0xc0000005
00:46:19  appVGRPresenterUI.exe — Faulting module ntdll.dll,   exception 0xc0000374 (heap corruption)
00:18:00  appVGRPresenterUI.exe — Faulting module ntdll.dll,   exception 0xc0000374
00:14:49  appVGRPresenterUI.exe — Faulting module Qt6Core.dll, exception 0xc0000005
```

`Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000}` is the query
(`tools/agent_crash_check.ps1` prints it). Both runners now notice it and say so
(`FAIL crashed`) with the press count, instead of reporting `too-few-moves` — that
misreading is what made an app crash look like "the scroll feature stopped working",
which is how it was reported.

What is known and not known:

* it does **not** reproduce on the synthetic 24-page walk from an isolated build
  (`SELFTEST_BUILD_DIR=build-agent`): three consecutive `PASS` runs, no new Application
  Error events. The earlier synthetic runs that "stalled at press 5" were in fact this
  crash — the flow fix (§10) removed it from that path;
* it **does** reproduce with the real Table passage, on builds from both trees, including
  the working tree's in-flight `TextEngine` / `TemplateTextMetrics` work;
* a crash without a stack is not diagnosable by reading the code — the next step is a
  build with a crash handler that prints a stack (PageHeap/`gflags` on the exe, or a
  MinGW debug build run under gdb with `handle SIGSEGV stop`), then the frame that owns
  the corrupted allocation.

Until that is understood, treat "the scroll stops" as **two** reports with two causes: the
stepper's bookkeeping (§10, fixed and pinned by the scenario) and this crash (open).

### 11a. One real bug found on the way: the interface thread read the live presentation unlocked

`LiveOutputService::onAirSlidesRaw` / `refreshOnAirSlide` / `onAirSlideAt` and the 10 Hz
on-air mirror read `PresentationEngine::Runtime().ActivePresentation()` **directly**, then
copied the slides' `std::string`s and block vectors. The runtime binds a RAW POINTER to the
controller's own presentation, and the loop thread frees what it replaces — a queued pick
(`ApplyPendingLiveContent` → `SwapLiveContent`) and a reading page commit (`flowText`
rewritten in place). That is a use-after-free on the thread that draws, and it matches the
symptom exactly: heap corruption (`0xC0000374`) plus an access violation inside `Qt6Core`
right where the walk crosses a page boundary.

**Fixed:** the controller publishes a snapshot of the on-air slides + index on every change
(`PublishOnAirSnapshot`, under its own short mutex — never `control_`), and the four readers
above answer from it (`OnAirSlidesSnapshot` / `OnAirSlideSnapshot` /
`OnAirPositionSnapshot`). The interface thread still never waits on a compose: the copy is
taken once per change, not per frame.

*And the on-air copy carries the slide's reading fields.* `slideToVariantMap` composes the
PICTURE and deliberately omits the passage, so a live walk's raw map lost `flowText` once
the staged copy was cleared at go-live — which silently disabled two readers keyed on it:
the monitor tile's composition memo (`walking`, the per-tile storm it absorbs) and the
table-walk mode contract. `refreshOnAirSlide` now re-inserts `flowText`/`flowChar` from the
slide itself.

*Effect on the crash:* it moved. Before, the walk died at press 0–4; after, it survived to
press 58 (and one run completed all 121). The crash is still there — see 11b.

### 11b. The crash is in the NDI path — A/B measured

Two runs of the same real-passage walk, same binary, same profile copy, differing only in
the `ndi` feature flag (`plugins.featureStates`):

| NDI | result |
|---|---|
| **on** | dies mid-walk (press ~58), Windows: `0xc0000005` in `Qt6Core.dll` |
| **off** | completes the whole scenario — `done presses=121`, three jumps, the mode contract — and writes **no** Application Error record |

So the corruption is reached through the NDI path (or through what enabling NDI adds: a
second, separately styled per-output scene and its frame-buffer feed). That is the remaining
work, and it is a much smaller target than "the reading walk".

**Reproduction that does not touch the operator's app:**

```bash
SELFTEST_BUILD_DIR="$PWD/build-agent" VGR_PROFILE_DIR=/tmp/vgr-agent-profile \
    bash tools/selftest/run_table_walk_test.sh 420
```

(`build-agent/` is a second build tree; the profile copy keeps the two instances out of each
other's store. Without them a second instance exits within ~30 s and the runner reports
`FAIL crashed` — correctly, but it is not the bug.)

**What was ruled out:** AddressSanitizer (this toolchain cannot link it — `ld: cannot find
-lasan`), gdb (a fast-fail leaves no signal to catch; the app simply exits), the app's own
crash handler (writes no record for the same reason), and provider lifetime (the NDI plan
holds `shared_ptr`s, so a stale plan cannot dangle).

---

Measured 2026-11-01 on this checkout, software renderer, two enabled outputs. The
composed-frame figures in §3 are the app's own `Telemetry` hotspot samples from a
46-press walk; §7's command counts and log volumes are from the same build after the
fixes. The under-load walk described in §9 is still the missing measurement — the
froze-under-stress claim is explained by the numbers in §3, not re-measured in that
state (the sandbox clock above is the 1,501-line walk, which is warm, not stressed).
