# tools/selftest — the UI self-test scenario, removed from Main.qml

## What this is

`ndi_golive_scenario.qml.removed` is the UI self-test scenario block that used
to live inside `qml/Main.qml` (lines 352–922 at the time of removal). It is
kept here as a **document**, not as a build input — nothing includes it and
nothing compiles it.

It was removed 2026-10-01 on the user's call ("remove the self test in
main.qml"): ~570 lines of diagnostic Timers in the middle of the main screen,
carrying dated crash-repro notes for bugs that are now fixed and covered by
unit tests.

## What went with it

- Every `Timer { id: selfTest* }` — the NDI toggle/GO LIVE walk, the Quick
  search stages, the monitor-wall style stages, the video-preview sweep.
- Three scratch properties on the root item (`selfTestT6`,
  `selfTestPillT`, `selfTestStyleRow`).
- The `selfTestSettingsShell` objectName on the Settings ModalShell — put
  back when the modal-fit scenario below needed a hook on that card (the shell
  is the app's only one, so it carries no objectName of its own).

## What deliberately stayed

- **`SelfTestDriver`** (`src/SelfTestDriver.*`) and the `SelfTest` QML
  context property (`src/main.cpp`, gated by `VGR_ENABLE_SELFTEST` +
  `VGR_SELFTEST=1`). The driver is inert without the env var and costs
  nothing in a release build (`-DVGR_ENABLE_SELFTEST=OFF` leaves it out
  entirely).
- **The `selfTest*` objectNames on product components** (~41 of them across
  `qml/components/` and `qml/screens/`). They are the hooks a *future*
  scenario finds items by; `objectName` is a plain QObject property, so they
  cost nothing at runtime. Removing them would mean re-adding them the next
  time a rendering bug needs pixel truth.

So the app can still be driven by a self-test scenario — there just isn't one
sitting in the main screen any more.

## If you want to bring a scenario back

Paste the block back inside the root `Item` in `qml/Main.qml` (it needs
`window`, `showScreen`, `quickSearch`, `editScreen` in scope — those are all
root-level ids, which is why it lived there), or write a smaller one. Gate it
the same way:

```qml
Timer {
    id: myStage
    running: typeof SelfTest !== "undefined"   // SelfTest only exists when
    interval: 1500                             // VGR_SELFTEST=1 is set
    onTriggered: { /* drive the UI */ }
}
```

## Probe scripts that referenced it

`tools/ndi_golive_probe.sh` and `tools/ndi_live_session_probe.sh` launched the
app with `VGR_SELFTEST=1` and expected `[SELFTEST-NDI]` / `[SELFTEST-LIVE]`
lines from these Timers. With the scenario gone they still run and still
check the things that matter without a scenario:

- crash-log and dump counts before/after (the actual regression check)
- the engine log's own NDI lines — `sending started`, `frames flowing`,
  `monitor(s) connected` — which come from `LiveOutputService`, not from the
  self-test

What they can no longer do is *press GO LIVE for you*. That step is now
manual: run the app, press GO LIVE with an NDI output enabled, and watch
Studio Monitor.

## Running every scenario

`run_all_tests.sh` runs every `run_*_test.sh` in this directory, one after
another, and prints a line per scenario plus one greppable verdict. Each
scenario keeps its own documented wait (they differ per area, and
`run_media_meter_test.sh` takes an audio file first), so the runner passes no
arguments; full output lands in `build/logs/selftest_all.log`.

```bash
tools/dev_cycle.sh --build-only                 # the scenarios drive the built app
tools/selftest/run_all_tests.sh                 # every scenario, ~25 min
tools/selftest/run_all_tests.sh style split     # only scenarios matching a name
```

The last line is `SELFTEST_ALL: PASS` or `SELFTEST_ALL: FAIL (n of m failed)`;
the exit status is 0 / 1 (2 when the app is not built). A failing scenario
prints its own verdict line in the table, so the row names the area that broke
without the run having to be repeated for it alone.

## The inset self-test

`run_insets_test.sh` is a working scenario again — the first one built on
the surviving driver. It guards one specific, easy-to-drift layout fact:
**every card on Settings · General shares the same 20px internal padding.**
A padding mistake is invisible to property-level checks, so this one reads
the rendered pixels.

```bash
bash tools/selftest/run_insets_test.sh
# ... INSETS_TEST: PASS | FAIL drift | FAIL missing
```

How it fits together:

- **The scenario** is a short `Timer` block at the end of `qml/Main.qml`
  (`selfTestInsetsOpen` / `selfTestInsetsGrab` / `selfTestInsetsDone`).
  Gated on the `SelfTestInsets` context property — set true only when BOTH
  `VGR_SELFTEST=1` and `VGR_INSETS_TEST=1` — it opens Settings · General,
  grabs each card to `insets_<name>.png`, and quits. (The extra env var keeps
  a plain `VGR_SELFTEST=1` run, how the NDI probes launch, from opening
  Settings or quitting early.) It needs `window` and `settingsScrim`, which is
  why it lives in Main.qml.
- **The grab hooks** are `objectName`s on the cards in `GeneralScreen.qml`:
  `selfTestCardAppearance`, `selfTestCardStartup`, `selfTestCardPreferences`,
  `selfTestCardBackups`, `selfTestCardNotifications`, `selfTestCardLibraries`,
  `selfTestCardReset`.
- **The checker** is `measure_insets.py` (Pillow). It finds each card's fill
  and border colours, takes the bounding box of everything else as the
  content, and asserts the left/top/right insets agree across the cards
  (compared to their own median, so a glyph side-bearing cancels out) and
  clear an absolute floor. Its own measurement is validated on synthetic
  cards: `py tools/selftest/measure_insets.py --selftest`.

To cover a new card: add an `objectName` to it in `GeneralScreen.qml` and
append that name to the `cards` array in the Main.qml scenario. The bottom
edge is reported but not asserted (the last control's height differs card to
card, so the gap below it isn't comparable).

## The modal-fit test

`run_modal_fit_test.sh` guards the other half of the same settings-modal
picture: **the card must stay inside the window it opens in.**

```bash
bash tools/selftest/run_modal_fit_test.sh
# ... MODAL_FIT: PASS | FAIL clipped | FAIL missing | FAIL scale | FAIL detached | FAIL desktop
#     | FAIL overlay | FAIL blank
```

**One scale for the whole app, and it is the window's size.** Every surface is
drawn at its 1440x900 design size and scaled by `window.uiScale` (qml/Main.qml) =
`min(window.width / 1440, window.height / 900)`: the screens, the header, the
menu bar, the drag layer, the Settings card, every dialog card and the toasts.
Nothing reflows at a smaller size — the section pages are fixed-pixel layouts (a
title's dropdown, an icon button and a value chip each sit at their own x), so
giving them less width than they were drawn for made rows and icons collide
inside General and Audio & Video. The whole UI shrinking uniformly cannot do
that, and it is why `ModalShell` no longer fits itself page by page.

**The app sizes itself to the screen it starts on.** The window opens filling the
desktop work area, and the panel size comes from the ENGINE
(`EngineBridge.primaryDisplay`, the PAL's IMonitor) divided by Qt's
devicePixelRatio: Qt's `Screen` geometry is the DPI-SCALED logical size, so on a
scaled display it does not say what panel the user actually has. `uiScale`
therefore scales UP on a big monitor as well as down on a small one, and
`[SELFTEST-DESKTOP]` prints the desktop, the engine's own reading, the dpr, the
launch window and `fills=` as the proof.

**Each scaled layer is drawn in the design canvas** — the window expressed in
design units, `window / uiScale` — so the scaled canvas lands exactly on the
window's own edges. The screens sit BELOW the header strip (topMargin
`headerExtra`), so their canvas height is `(window.height - headerExtra) /
uiScale`. A canvas pinned at the design size instead left the bottom library dock
hovering 26px above the window at 820x560 (measured `card=820x513 at 0,21`) —
nothing hung outside, but the project pane and the dock visibly detached from the
window. That is the sweep's `covers=` flag.

**Modals have one extra rule, and it lives in `qml/settings/AppScale.qml`: the
CARD is scaled, the SCRIM never is.** The scrim is the full-window dim that makes
a dialog read as modal; scaling the modal ROOT shrank the backdrop away from the
window's edges, so every popup lost its black box (Settings included).
`AppScale.cardScale(item)` returns the app scale MINUS whatever scale an ancestor
already applies, so a dialog opened inside a screen or inside the Settings card is
not scaled twice. Every modal card, the toasts, the dialogs that reparent
themselves to the window (`Overlay.overlay`) and the dropdown panels (which
reparent, and so cannot inherit a scale at all) read it.

**The dropdowns pop into the MODALS' layer, not the window's `contentItem`.**
`DropdownPanel.openAt()` lifts a panel out of its clipped/scrolling declaration
context and into `Overlay.overlay` — the same layer every modal card parents
itself to — with `z = 40000` against a modal root's `30000`. Reparented to
`contentItem` instead, a panel was a sibling BELOW the whole overlay, so the
Source field of a dialog drew its device list behind the open dialog: the list
was "nowhere in sight" while the panel's own geometry measured perfectly. The
`sweep's `[SELFTEST-DROP]` line is that check — panel and modal in one layer,
panel above it (`layer=true`), with the option count beside it.

The same sweep also covers every popup (the AV + Add dialogs, the bus
dialog, Add Output, New show, Import, quick search, the New category name
dialog, a toast) and the Audio & Video routing board's three columns, because
that board is laid out by hand inside a Flickable that never scrolls sideways:
at 820x560
its columns ran to 684 inside a 518px board, so 166px of the right-hand column —
the video side of the bus routing — was unreachable ("resizing cuts off the
video bus"). The board's columns now scale down together when their minimums do
not fit, and the design-size reading is printed as the control (it must be
identical to the pre-fix layout).

How it fits together:

- **The scenario** is the `selfTestModalFit*` Timers, `modalFitSweep` and
  `reportModalFit()`/`reportScreenCoverage()`/`reportScrims()`/`reportDesktop()`/
  `reportToast()`/`reportAvBoard()` at the end of `qml/Main.qml`, gated on the
  `SelfTestModalFit` context property (set true only with BOTH `VGR_SELFTEST=1`
  and `VGR_MODALFIT_TEST=1`, so a plain `VGR_SELFTEST=1` run — the NDI probes'
  launch — never opens Settings or quits early). It reports the Settings card at
  the launch size (with the desktop reading beside it), shrinks the window to its
  minimum, then walks the sweep: one tick opens a popup (a real click where a
  button opens it), the next tick measures its card, grabs it, closes it. Steps
  may carry their own `report()` (the Show screen, the scale + scrim checks, the
  toast), and the last restores the launch size with the AV page up to re-read
  the board. Nothing presses Save/Add, so a run creates no source, bus, output,
  show or category.
- **The card, not the modal root.** A modal root always fills the window, so its
  own bounds prove nothing: the check looks for `<objectName>_card` (ModalCard,
  NameDialog, ConfirmDialog, ImportDialog, NewShowDialog and NotificationToast
  name their card that way) and otherwise takes the largest child that does not
  fill the root. A root that fills the window and hides its card is reported
  `MISSING fits=false` rather than passing trivially.
- **The checks** are printed flags, so the shell script's greps are the whole
  checker — no pixel library involved. `fits=` (card inside the window), `covers=`
  (a scrim covering the whole window, or a scaled screen reaching its edges),
  `fills=` (the launch window filling the desktop), the dropdown's `layer=`,
  `blank=` on the Add Source dialog, and the toast's `fits=`, which also asserts
  it is drawn at `320 x uiScale`. Sizes come from MAPPED corners, so a card inside
  the scaled Settings card reports its on-screen size, not its local one — which
  is also how `[SELFTEST-UISCALE]` can compare every layer's scale in one line.
- **`blank=` exists because a broken card still "fits the window".** A dialog's
  height is derived (header + a capped Flickable + the footer), so one NaN in that
  chain — a `ReferenceError` on a sibling's property, say — leaves the card
  measuring a comfortable size with a zero-height content viewport: the dialog
  renders as an empty shell while every other check passes. `[SELFTEST-CARD]`
  prints the whole height chain (flick, content, footer, per-child boxes next to
  it) and fails `blank=true` when there is content and no viewport to show it in.
- **The pictures** are `build/modal_fit_<step>.png`, one per popup, grabbed at
  the minimum window size so the numbers can be eyeballed. They are in DEVICE
  pixels: at the 125% scaling this machine runs, the 615x364 logical Settings
  card lands as 769x455.
- **The PATH** is chosen per shell (`/c/...` when it resolves, `C:/...` when it
  does not): the agent shell is busybox ash, where `/c/...` does not exist and a
  Windows-form entry inside a POSIX PATH leaves the child with NO usable PATH
  entries at all — which is exactly how a launch fails with
  `Qt6Quick.dll: cannot open shared object file`.

To widen the coverage: add another measurement step for a section whose layout
is width-sensitive, or a fourth window size, and let `reportModalFit` print it.

## The input-only warning test

`run_ui_warn_test.sh` guards two warnings that **no property trace can see**,
because the input path itself produces them — which is why they only ever
showed up in `build/logs/engine.log` after a human had been clicking:

- `AppMenuBar` — `Binding loop detected for property "settingsFlyoutOpen"`.
  Hovering the logo menu's "Settings" row flies the section flyout out (see
  `recomputeSettingsFlyout`); showing it re-enters that recompute, because the
  flyout appears under a pointer that has not moved and its own row delivers a
  hover. `recomputeSettingsFlyout` therefore coalesces a re-entrant call
  (`flyoutRecomputing` / `flyoutRecomputeQueued`) instead of writing twice.
- `ReferencePane` — `Cannot read property 'playPicked' of undefined`. A verse
  click or double-click that lands while the pane is being rebuilt reads a
  DESTROYED pane through `verseSource.owner` (a property read off a destroyed
  object is `undefined`, not an error), so the row's handlers re-read that
  property per use and skip when it is gone. The read is INLINED, not a helper
  `function ownerPane()` declared on the DragSource: selectVerse can destroy
  that very DragSource synchronously while its handler still runs, and calling a
  function member of a destroyed object throws `Property 'ownerPane' ... is not
  a function` plus `attempted to evaluate a function in an invalid context` —
  the property re-read is the form that safely yields undefined.

```bash
bash tools/selftest/run_ui_warn_test.sh 170   # boot alone is ~55s (QML load)
# ... UIWARN: PASS | FAIL bindingloop | FAIL playpicked | FAIL notrun | SKIP verses
```

How it fits together:

- **The scenario** is the `selfTestUiWarn*` Timer block at the end of
  `qml/Main.qml`, gated on the `SelfTestUiWarn` context property (BOTH
  `VGR_SELFTEST=1` and `VGR_UIWARN_TEST=1` — it opens menus, flips a setting and
  quits, so a plain `VGR_SELFTEST=1` run must not start it). It clicks the logo,
  hovers the "Settings" row, crosses onto the flyout, switches to The Table tab,
  flips `table.splitLongVerses`, then double-clicks a verse row — all through the
  driver's REAL cursor moves and synthetic clicks.
- **Runnable item hooks**: `selfTestMenuLogo`, `selfTestSettingsFlyout`
  (`AppMenuBar`), `selfTestVerseRow_<number>` (`ReferencePane`) and
  `selfTestDropdownRow_<label>` (`DropdownPanel` — leaves only the trailing
  `…` out on labels that have no label data).
- **The scenario reports itself**, including the evidence that matters: the
  flyout's own `visible` and whether any verse row existed. The runner refuses
  to pass a run whose sequence never opened the flyout (`FAIL notrun`) and
  reports `SKIP verses` when there was no verse row to double-click, so a
  scenario that silently did nothing cannot read as a pass.
- **Line DELTA, not the whole file**: `engine.log` is appended across runs, so
  the count before launch is subtracted; warnings from earlier sessions can
  never decide a run.
- **Known limit**: with the guards temporarily removed, this scenario did not
  reproduce either warning (and the flyout ended up HIDDEN instead of open —
  the re-entrant write is real, which is what the guard coalesces). So it
  currently proves "these scripted sequences are clean", not "the reported path
  is fixed". Widening it (hovering with the pane rebuilding mid-click) is the
  next step if either warning returns.

## The Media pane's L/R pill test

`run_pill_test.sh` guards the report "as a bug if i click on the L or R it closes
all". The Media pane's Inputs section is the BUSES (the audio-input and
video-source rosters, and their cards' pill pairs, belong to the
Settings - Audio & Video board); a bus card's L/R pair used to mute the WHOLE
bus, so one click closed both sides and there was no way to close just one. It
now writes that side's own trim (gain 0 = "that side is closed"), the same
convention the audio input cards use.

```bash
bash tools/selftest/run_pill_test.sh 120   # boot alone is ~15s (QML load)
# ... PILL: PASS bus=closed-own-side busBy=click
#     PILL: FAIL launch | FAIL missing | FAIL bus
```

How it fits together:

- **The scenario** is the `selfTestPill*` Timer block in `qml/Main.qml`, gated on
  the `SelfTestPill` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_PILL_TEST=1` — it clicks a channel button, so a plain `VGR_SELFTEST=1` run
  must not start it). It clicks the Media dock tab, sets the pane's Inputs roster
  through the pane itself, then clicks the bus card's pill.
- **Runnable item hooks**: `selfTestBusPill_<row>_<side>` in `MediaLibraryPane`
  (the bus card's pair), plus the pane's own `selfTestMediaPane` and the dock's
  `selfTestTab_media`.
- **Judged from the model, not the pixels**: the clicked side must read 0 and the
  OTHER side must still read open — that second half is the bug.
- **Click first, `toggled()` as the fallback**, and the report says which ran
  (`by=click` / `by=toggled()`): the driver's synthetic clicks need
  window-foreground luck, so a miss must not read as "the fix does not work".
- **Leaves the rosters as it found them**: trims are restored, and a row it added
  is removed only after its own name confirms the index still points at that row.
- **Waits 15s before its first step** so the rosters' own restore cannot land
  between the scenario's add and its cleanup.

## The Scripture tab: do its columns fill?

`run_scripture_test.sh` guards the report "scripture is not showing" — the
Scripture tab listing the installed Bible in its first column while the books,
chapters and verses columns stayed blank and the preview said "Pick a verse",
with nothing in the log, because an empty column is the same pixels whether the
outline read is still in flight, was declined, or came back empty.

```bash
bash tools/selftest/run_scripture_test.sh 70
# ... SCRIPTURE: PASS sources=1 bookRows=66 verses=31 serviceBooks=66
#     SCRIPTURE: FAIL launch | FAIL missing | FAIL empty | FAIL noread
```

**What it caught.** `ScriptureService`'s corpus reads are PUBLISHED (a miss
queues the read on a worker and answers EMPTY — see its header). The pane's
`syncSources` had an early return, `if (root.currentSource) return`, meaning "a
source is already open, nothing to choose". The FIRST `openSource` can land right
in that gap: it set `sourceId`, took an empty outline and left no book open — and
every later notify then returned at that guard, so the outline that landed a
third of a second later (66 books, `ScriptureCorpus` says so in the engine log)
was never consumed. The pane now re-asks an empty outline instead of returning on
it, which is cheap: a read already in flight is not queued twice. The run's own
trace shows the repair (`books 'ENGLISHK21': read queued` → `66 answered` →
`openBook` → `openChapter rows=31`).

How it fits together:

- **The scenario** is the `selfTestScripture` block in `qml/Main.qml`, gated on
  the `SelfTestScripture` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_SCRIPTURE_TEST=1`). It shows the tab exactly as a click does
  (`showScreen.showInLibrary("scripture", "")`) and prints the pane's own state
  (`sources`, `sourceId`, `books`, `visibleBooks`, `chapterVerses`) beside what
  the service answers for the same Bible id — twice, the second time after the
  pane re-asks the way a tab switch makes it (`syncSources`).
- **The pair is the point**: books on the second sample but not the first is a
  notify that never reached the pane; none on either is the read itself.
- **`ScriptureCorpus` in the engine log** (one line per COLD read: `read queued`,
  `read DECLINED — the engine is not booted yet`, `<n> answered`) is what says
  which of those it was. The harness reads it back from `build/logs/engine.log`
  per launch, not from the app's stdout, where only `console.log` lands.
- **Read-only**: it opens the tab, prints, and quits. Nothing is written.

## The bus status light: does it actually go green?

`run_bus_light_test.sh` guards the report "i thought i said we should give the
status light access to check if [the] device in the source is bringing data …
so i expect to see all green". With every device present and every bus routed,
all three lights read amber, and there were two independent reasons:

- **The binding had nothing to re-evaluate it.** The card asks the engine
  through Q_INVOKABLE lookups, which QML does not track, so it kept the answer
  it got when it was first built. The engine's device sweeps announced
  themselves and nobody was listening — the pane now follows those
  announcements (its own `devicesRev`), so a sweep, a hot-plug or the light's own
  ask lands on the card.
- **The roster never carried the device.** The boot enumeration is targeted (it
  walks only the devices the setup named), so a source whose label never reached
  that set was invisible to every reader. The light's ping NAMES its source, and
  one off-thread sweep walks that device — the ping is the half the user asked
  for by name, and the scenario checks it fired rather than trusting the cache.

```bash
bash tools/selftest/run_bus_light_test.sh 100
# ... BUSLIGHT: PASS green=3/3 buses=3 pings=4
#     BUSLIGHT: SKIP noaudio — the video half passed, but this SETUP has no
#                audio-only bus with a routed input (exit 0)
#     BUSLIGHT: FAIL launch | FAIL missing | FAIL norouted | FAIL status
#                | FAIL noping | FAIL pinglabel | FAIL ghostalive | FAIL wrongkind
```

**`SKIP noaudio` is the setup's verdict, not the app's.** The audio half is the
run's positive case (a meter tap hands over levels — silence is data — so a held
meter must read `ok` and its card `connected`), and whether such a bus EXISTS is
something the scenario's own candidate search decides from the stored rosters
(`BusListModel` + `AudioInputListModel`) — it touches no meter and no tap. On a
machine with no audio-only bus carrying a routed input there is nothing to hold,
so the run reports SKIP and prints the half that DID run beside it, by name:

```
BUSLIGHT: SKIP noaudio — video half PASSED (buses=2 pings=3 delivery=ok heldBus=1
  heldStatus=connected green=1/2; not connected: The Table (video) reads offline)
  but this setup has no audio-only bus with a routed input, ...
```

Two details there are deliberate. The **not-connected bus is named** because the
run's own `status` verdict treats any routed-but-not-connected bus as a fault,
while a screen bus whose screen is not on air reads `offline` by design — so the
skip states which bus it was rather than leaving `green=1/2` to be misread as a
light that failed. And **`norouted` is asked before the skip**, so a run with
nothing routed at all can never take the skip path and pass off as "this setup's
limit". To earn the audio half, route an audio input onto an audio-only bus
(Audio / Video screen).

How it fits together:

- **The scenario** is the `selfTestBusLight*` block in `qml/Main.qml`, gated on
  the `SelfTestBusLight` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_BUSLIGHT_TEST=1`). It opens Media · Inputs through the dock tab, waits
  three ticks for the light's ping and the re-enumeration to land, then reads
  each card's own `busStatus` — the very property its colour is drawn from — and
  prints the routing behind it (bus, type, routed counts, the video label and its
  kind, the audio label and its kind, and both rosters' verdicts).
- **Runnable item hook**: `selfTestBusCard_<index>` on the bus card in
  `MediaLibraryPane`, alongside the pane's `selfTestMediaPane` and the dock's
  `selfTestTab_media`.
- **The ping is proved, not assumed**: the scenario also asks about a label no
  device and no window can have (`selftest-buslight-ghost-window`). It must read
  *not* available and leave a `status light for '…' … pinging` line in the engine
  log — so a light that only ever reads a cache it never refreshes fails the run
  (`FAIL noping` / `FAIL pinglabel`), and a source that is genuinely absent can
  never read green (`FAIL ghostalive`).
- **A wrong kind is not a dead source**: the same real window is asked with the
  kind `window` (which the engine does not spell) and must still read available —
  the fallback that keeps a travelling kind string from pinning a real source's
  light amber (`FAIL wrongkind`).
- **Read-only**: the scenario opens the pane and reads. It adds, renames and
  removes nothing, so it cannot leave anything behind in the user's own rosters.
- **Waits 15s before its first step** so the rosters' own restore cannot land
  between the pane opening and the reading.

## A media row's audio bar: the signal, not the stored Volume

`run_media_meter_test.sh` guards the report "the media audio animation is still
stuck even though it shows the opposite in edit". On Settings - Audio & Video a
media row's level bar drew the row's STORED volume — a fixed fill that tracked
the Volume slider and never moved — while the Edit dialog for that same row drew
the bridge's stream tap (the actual audio) and read a flat -inf dB, because that
row's file was gone. One row, two readings that could not agree.

```bash
bash tools/selftest/run_media_meter_test.sh                 # 90s, picks its own audio file
bash tools/selftest/run_media_meter_test.sh C:/Windows/Media/Ring05.wav 90
# ... MEDIAMETER: PASS live=41.30 silent=0 stored=4 agree=PASS file="…" taps=1
#     MEDIAMETER: FAIL launch | FAIL missing | FAIL live | FAIL silent | FAIL agree
#                 | FAIL nolive-T1 | FAIL nosilent-T1
```

How it fits together:

- **The scenario** is the `selfTestMediaMeter*` Timer block in `qml/Main.qml`,
  gated on the `SelfTestMediaMeter` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_MEDIAMETER_TEST=1` — it adds and removes video rows, so a plain
  `VGR_SELFTEST=1` run must not start it). The real file comes from
  `VGR_MEDIAMETER_FILE` when the runner names one, else the scenario's own
  candidate list (Windows system sounds: every machine has them and Media
  Foundation decodes a WAV with no codec pack).
- **Both halves of the report are measured**: a row pointed at a REAL file must
  move its bar, and a row pointed at a file that is NOT there must read 0 while
  its stored level stays 4 — the value from the reporting roster, which fills
  ~53% of the bar's scale (the frozen fill in the screenshot; the report line
  prints it as `oldFill`).
- **Runnable item hook**: `selfTestVideoRowMeter_<row>` on the video row's
  `LevelTrack`; the scenario reads its `value` — the number the fill is drawn
  from — rather than sampling pixels.
- **Agreement is measured, not assumed**: the same rows are read through the
  screen's own `videoChannelLevels`, the function the Edit dialog's channel
  strips bind, reached by walking up from `selfTestAvBoard`. `agree=SKIP` is
  reported (and passes) only when that screen object was unreachable.
- **The engine log is the second witness**: the live file must have been given a
  STREAM TAP (`stream meter tap live for`) and the missing one must have been
  REFUSED (`stream meter refused for`) — so a bar that moved off a made-up number,
  or a silent bar that was silent because nothing was ever attempted, cannot read
  as a pass. Line DELTA, not the whole file, for the same reason as the rest.
- **Leaves the rosters as it found them**: both rows are removed only after their
  own names confirm the indices still point at them (higher index first), and the
  debounced roster write gets a tick before the app quits.

## The Table's result rows: the highlight and where the hit sits

`run_pane_highlight_test.sh` guards two reports about The Table pane's own
search box, both from the same screenshot:

> "the highlighter is not highligting compelling ... i expect the highlighted
> text to show well like in the centre not at the extreme so user can see the
> hits well"

Three separate defects sat behind it, and only the last one needed pixels:

- **The highlight list was missing words the user spelled right.** It was built
  from the engine's resolved terms, so a row could show one typed word in the
  match colour beside the other in plain grey. Every typed word is in the list
  now (`ReferencePane.highlightQuery`).
- **The window ignored the group of words that matched.** A row is ONE line
  (~90 characters) while the engine's snippet is up to 220, and the window
  opened on the FIRST occurrence of any word — an incidental early "that"
  anchored the row while "compelling that" sat past the visible edge. It now
  opens on the opening that carries the MOST of the typed words
  (`ReferencePane.centeredSnippet`), a third of the way before it.
- **Measuring the snippet inside the binding that displayed it was a loop.**
  `shown` wrote a shared `TextMetrics` it then read back, which Qt reported once
  per row, every frame (`Binding loop detected for property "shown"`). The row
  measures its own snippet with a BOUND `TextMetrics` and passes the advance in,
  so `centeredSnippet` stays a pure function of its arguments.

```bash
bash tools/selftest/run_pane_highlight_test.sh 200   # boot + the sermon index
# ... PANEHL: PASS rows=<n> firstHitAt=<pct>% worstHitAt=<pct>% typoRows=<m>
#     PANEHL: FAIL launch | FAIL missing | FAIL unlit | FAIL clipped
#                 | FAIL typosearch | FAIL nohint | FAIL noshot
```

How it fits together:

- **The scenario** is the `selfTestPaneHighlight*` Timer block in `qml/Main.qml`,
  gated on the `SelfTestPaneHighlight` context property (BOTH `VGR_SELFTEST=1`
  and `VGR_PANEHL_TEST=1`). It opens The Table pane's search and TYPES into the
  field (`box.text`, whose own debounce runs the search) — calling `runSearch()`
  behind the field's back leaves `highlightQuery` empty and measures a
  highlight the user would never get.
- **Two passes**: "compelling that" (the user's own query — both words must be
  wrapped, the hit cluster in the middle) and "complling that" (the same query
  misspelled — the pane's box must show the typo fixer's correction while
  results are on screen; it used to appear only when nothing was found).
- **Runnable item hooks**: `selfTestTablePane`, `selfTestSearchBox-table`
  (named per content type — both reference panes own one) and
  `selfTestPaneHitSnippet` (the row's own Text).
- **Judged from two witnesses**: the markup the row Text carries
  (`«compelling»`, read off the live object), and the pixels it painted. The
  pane also holds `Theme.accent` text, so a warm-pixel count over the whole pane
  can come back gold for the wrong reason; the row's own grab isolates it.
- **`gold_pixels.mjs`** takes the row PNG apart (node's `zlib`, no
  dependencies) and counts pixels within tolerance of the highlight colour,
  reporting the most common warm colour too — a render in the OTHER gold is then
  visible instead of a silent zero. Run it on any grab:
  `node tools/selftest/gold_pixels.mjs build/panehl_row0.png --min 20`.
- **Screenshots to look at**: `build/panehl_pass1.png` (the user's query, rows
  and hint line), `build/panehl_pass2.png` (the misspelled query, correction
  shown) and `build/panehl_row0.png` (the first row alone).

## The Table's result lists: a viewport of rows, not the whole model

`run_pane_rows_test.sh` guards the ask that followed the paragraph list:

> "Make the reference pane's search-results list virtualized as well, since a
> two-character query can still build about a thousand rows at once."

A row COUNT cannot answer that — a long list is fine, building it in one go is
what parks the pointer — so the scenario measures the two things only a windowed
list does, for each of the three lists this pane owns (the tab search's citation
matches, the hover peek's sermon paragraphs, and the pill search's hits):

- **built << rows.** With the query's rows on screen, the list has materialized a
  viewport's worth of delegates (plus its cache buffer) instead of one per row.
- **The tail is still reachable.** Scrolling to the END materializes the LAST row
  (`lastRow=true`) while the total ever built stays below the row count — a
  windowed list must reach its end, or the tail of a 641-match query would be
  unreachable. A Column-and-Repeater passes neither test: it builds every row the
  moment the model lands, which is exactly what the three lists used to be.

```bash
bash tools/selftest/run_pane_rows_test.sh 150   # boot + the sermon indexing
# ... PANEROWS: PASS judged=3 skipped='' \
#     match=641/21/32/true peek=277/5/8/true hit=60/20/30/true
#     PANEROWS: FAIL launch | FAIL missing | FAIL whole | FAIL tail
#               | FAIL lastrow | FAIL small | FAIL qml
```

The verdict line reads `<list>=<rows>/<built>/<scrolledBuilt>/<lastRow>` — the
rows the model holds, the delegates ever built with those rows on screen, the
total ever built after scrolling to the last row, and whether that last row is
truly on screen. Measured on this machine's library (1,206 sermons), the user's
own two characters ("th"):

| list | rows | built | after scrolling to the end |
|---|---|---|---|
| the citation matches (the tab search) | 641 | 21 | 32, last row on screen |
| the hover peek (that match's sermon) | 277 | 5 | 8, last row on screen |
| the pill search's hits | 60 (the engine's cap) | 20 | 30, last row on screen |

The two-character query's own worst case is the pair the user named: 641 rows
built by the typing itself, and then 277 more in one go when the pointer opened
ONE of them — 918 delegate builds for a single query, and 26 for the same query
now.

How it fits together:

- **The scenario** is the `selfTestPaneRows*` Timer chain in `qml/Main.qml`,
  gated on the `SelfTestPaneRows` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_PANEROWS_TEST=1`), so no other run drives the pane's search or its hover.
- **The real path**: the query is TYPED into the tab bar's box
  (`SelfTest.focusItem("selfTestSearchInput")` + `SelfTest.type`), whose provider
  answers per keystroke; the peek is the pane's own `peekSuggestion()` on its
  LONGEST matching sermon (the hover's worst case); the pill's field is set the
  way the pane-highlight scenario sets it, since that field's own debounce runs
  the search.
- **Runnable item hooks**: `selfTestPaneMatchList`, `selfTestPaneHitList`,
  `selfTestPanePeekList`, and the row names `selfTestMatchRow_<i>`,
  `selfTestHitRow_<i>`, `selfTestPeekRow_<i>`. The pane's counters
  (`matchRowBuilds`/`Drops`, `hitRowBuilds`/`Drops`, `peekRowBuilds`/`Drops`, the
  same ones the layout probe reads) are what `built`/`scrolledBuilt` come from.
- **itemVisible, not "exists"**: the last row is looked up by name and judged on
  screen, because a RECYCLED row of the same name is one the user cannot see.
- **Library-independent**: the two numbers are compared to each other, so a small
  library still decides it. A list with fewer than 60 rows is reported as SKIP
  rather than passed (there is nothing to measure), and the citation list — the
  one the user's two characters drive — must be big enough to judge, so a run
  whose search returned nothing fails as `FAIL small` instead of passing quietly.
- **Screenshot to look at**: `build/panerows_pane.png`, the pane in the state it
  measured (the matches list with the peek open).
- **Still eager, deliberately out of scope**: the two sidebar columns
  (`visibleBooks`, and a book's `chapters`) are still Flickable + Column +
  Repeater. They are bounded by the LIBRARY rather than the query (a Bible's 66
  books; The Table's year-books — a filter that matches no book name falls back
  to the full list), so a two-character query does not grow them, but they do
  rebuild per keystroke. The numbers to beat are in this scenario's log the day
  someone takes them on.

## A mark on a verse: what the click costs, on the operator's own store

`run_mark_sidecar_test.sh` guards the note/highlight path — the one the operator
uses mid-service, and the one that used to be a whole-corpus rewrite.

`setNote`/`setHighlighted` are `Q_INVOKABLE` on a QML singleton, so their cost is
the INTERFACE thread's, and the path used to read `bibles.json`, parse it and
write it back — megabytes, under the engine lock every scripture read takes. The
schema-3 split moved the user data into its own sidecar file
(`bibles.userdata.json`), so a mark writes kilobytes and the corpus is not
opened at all. The engine tests pin that (`bible-mark-sidecar`: the corpus must be
byte-identical, not merely re-readable; `bible-mark-timing`: the two wall-clock
figures side by side on a synthetic 6.78 MB corpus). What they cannot cover is the
OPERATOR's store, through the pane's own handlers — so this drives the real app on
it:

```bash
tools/dev_cycle.sh --build-only                 # the scenario drives the built app
bash tools/selftest/run_mark_sidecar_test.sh 150
```

It snapshots both store files first (5.77 MB corpus here), then `VGR_MARK_TEST=1`
opens the first Bible's first chapter and, on a verse that carries no mark at all,
toggles the highlight ON and OFF again and adds and removes a note — through the
star button's own `toggleHighlight` and the note editor's own `saveNoteDraft`,
each timed with `Date.now()` around the call, because that is the operator's
latency. Every mark is undone, so the store it leaves behind is the store it
found: that is what makes it safe to run against a real library, and the script
checks it byte for byte afterwards.

The verdict counts the engine's own lines for THIS run (the log is appended
to across runs, so only new lines count): sidecar writes must go UP with
kilobytes, and `store written (corpus)` must stay at ZERO.

```
MARK: PASS verse=1 onMs=52 offMs=36 addMs=52 rmMs=35 sidecarWrites=4 \
      sidecarMs=2-11 sidecarBytes=264 corpusWrites=0 corpusUnchanged=true \
      sidecarUnchanged=true
```

A failure names the area: `FAIL corpuswrite` (a mark wrote the corpus),
`FAIL corpuschanged`/`FAIL sidecarchanged` (the store did not come back to the
state it was found in), `FAIL slow:ui` (a mark cost the interface thread more
than 250 ms) and `FAIL restore` (the scenario did not undo its own marks).

## The cursor: does the OS actually show what the app asked for?

`run_cursor_scale_test.sh` guards two reports, both about a cursor the app's own
model said it was showing:

> "for the app rescaling the cursor doesnt show as well" ... "i tested my self
> the cursor is just flashing not yet showing"

Both turned out to be true, and neither was visible in the QML model:

- **A per-MouseArea `cursorShape` on an ordinary control is fine — scaled or
  not.** Parked on `+ Add style` (an `AppButton`) the OS shows HAND at uiScale 1
  and at 1.2, so the codebase's older "cursorShape stops applying under a scaled
  ancestor" note is not what breaks cursors here. Keep this park in the test:
  it is the control that proves a change to the cursor layer did not break the
  ordinary path.
- **The window frame was showing an arrow.** Parked on a `WindowResizeHandles`
  grip (unscaled, 8px, drawn over the content) the stack held `SizeHor` with the
  grip hovered, and Win32 `GetCursorInfo` still returned ARROW — the QML
  item-cursor claim is applied on the NEXT pointer event and never reaches the
  strip the OS paints itself, which is why the arrow appeared only as a flash
  while the pointer moved. Fixed by `qml/services/CursorService` — an
  application OVERRIDE cursor, applied ahead of item cursors and deactivated by
  `AppCursorCatcher.syncShape()` the moment the override stack empties.

```bash
bash tools/selftest/run_cursor_scale_test.sh   # ~60s: boot, two parks, the frame
# ... CURSORSCALE: PASS at1=HAND atScale=HAND windowed=true frameEdge=SIZEHOR frameCorner=SIZENWSE
#     CURSORSCALE: FAIL launch | FAIL missing | FAIL scaled | FAIL frame
```

How it fits together:

- **The scenario** is the `selfTestCursorScale*` Timer block in `qml/Main.qml`,
  gated on the `SelfTestCursorScale` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_CURSORSCALE_TEST=1`). Phase A parks on the button at the design size,
  phase B at a window 20% over it, phase C closes the Settings card (its scrim
  covers the frame, as it should) and parks on the window's left grip and
  top-left corner.
- **`SelfTest.cursorAt(x, y)` is the measurement**: it moves the real pointer,
  pumps the event loop, verifies the park actually LANDED (and that the window
  under the point is ours), and reads Win32 `GetCursorInfo` in the same tick.
  Reading the cursor from a SEPARATE process does not work — the first cut of
  this script did that and measured the user's own pointer, 46px from the park,
  with complete confidence.
- **Measured in one tick on purpose**, so it is immune to a human at the
  keyboard: a probe that polls the live cursor races them.
- **`tools/selftest/cursor_shape.ps1`** is the out-of-process reader, kept for
  eyeballing the cursor by hand (`powershell -File tools/selftest/cursor_shape.ps1`).

## The header vs the caption buttons

`run_header_clear_test.sh` guards the report:

> "the header tile is overlapping the minimize, maximize, close button ... it
> doesn't happen at max scale, only when it scaled down"

Two separate facts made that true, and the test checks both at THREE window
sizes (launch, minimum, and one between):

- **The tiles and the buttons share one column — that is the design.** GO LIVE
  holds the tab row's right end, the caption buttons hold the title row's, and
  the two rows must never overlap: a tile that drifted up into the caption row
  would sit under the buttons, and a cluster that grew down into the tab row
  would bury the tile. Both are measured in the HEADER's own design space, so
  scale cannot hide a collision.
- **The resize grips are the frame and do not scale.** An 8px strip that grazed
  the buttons at full size covered nearly half an 18px-tall close button at
  820x560 — a SIZE cursor, and a press that resized the window instead of
  closing it. Each grip's rect is checked against the cluster in window pixels,
  and the LIVE OS cursor is parked 3px inside the close button's top edge
  (`SelfTest.cursorAt`, Win32 `GetCursorInfo` — the same call a user's eye makes).

The last step leaves the window at its smallest size and grabs the WHOLE WINDOW
(`SelfTest.grab("", …)` — an empty name means the content item), and
`corner_empty_pixels.mjs` reads that picture: the caption COLUMN, one row below
the buttons, must be empty. That is the pixel half of the report, and it holds
whatever the layout arithmetic says.

```bash
bash tools/selftest/run_header_clear_test.sh   # ~30s: three sizes, a park, a grab
# ... HEADER_CLEAR: PASS
#     HEADER_CLEAR: FAIL missing | FAIL overlap | FAIL grips | FAIL cursor
#                 | FAIL pixels | FAIL grab
```

How it fits together:

- **The scenario** is the `selfTestHeader*` Timer block in `qml/Main.qml`, gated
  on the `SelfTestHeader` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_HEADER_TEST=1`). Each size is measured on the beat AFTER the resize, so
  the scaled layers have laid out.
- **The names it reads** — `selfTestAppHeader`, `selfTestCaptionButtons`,
  `selfTestGoLive`, `selfTestSettingsTile`, `selfTestFrameGrips` — are in
  `AppHeader.qml` / `WindowResizeHandles.qml`.
- **`clear=` on each line** is the whole verdict per size: no tile/cluster
  intersection, the tiles still in the row BELOW the caption row, no grip over
  the cluster, and an arrow (not a size arrow) on the close button. The
  `clearance=` it prints is for the record, not asserted — the tiles are meant
  to end at the window's edge.

## The metadata palette: click and drag

`run_meta_palette_test.sh` closes the gap the boot check left: the palette (the
Edit screen's `+` menu, **Metadata** chip) can add a field by CLICK or by DRAG,
and nothing had ever driven the gesture itself.

```bash
bash tools/selftest/run_meta_palette_test.sh   # ~50s: a new show, the palette, a click, a drag
# ... METAPAL: PASS
#     METAPAL: FAIL missing | FAIL palette | FAIL hint | FAIL click | FAIL drag
```

How it fits together:

- **The scenario** is the `selfTestMeta*` Timer block in `qml/Main.qml`, gated on
  the `SelfTestMetaPalette` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_METAPAL_TEST=1`). It drives the real path: a new show, the `+` chip, the
  **Metadata** chip in the `+` menu — a zone-hover chip, so the hover move is
  part of the gesture — then hovers a field chip, clicks one, and finally presses
  another and drags it onto the canvas.
- **The fields come from `MetadataFields.all`**, the registry the palette draws
  and the drag payload carries, so a field added there is covered without
  touching the scenario: the first field is clicked, the last is dragged (today
  The Table's `{table_code}`).
- **Each added item is read off its live DELEGATE** (`canvasItem_<key>`), the
  thing the user sees — a model row with no delegate does not count as added.
  The click's item must carry the token as its text; the dragged item's own
  centre must sit within 60 canvas units of the drop point ("land it where the
  pointer is"), the tolerance covering the drag chip's own hotspot offset.
- **`hintOk=`** is the palette explaining itself: hovering a chip publishes that
  field's own sentence from the registry. The palette's hint slot is a FIXED
  height on purpose — see the comment there: a line that appeared grew the
  palette (which is placed above the `+` menu by its own height) and the chips
  moved under the pointer, which read as flicker.

## The sermon folder: what the refusal notice actually says

`run_table_folder_test.sh` answers the report behind The Table's folder import:

> "I pointed it at a folder of sermons beside a pile of spreadsheets and it said
> \"40 imported\" — what about the other 300?"

The engine now names EVERY file it will not read (`<file>: <reason>` — the same
sentence the single-file flow shows), and the service draws that list in one
notice. Both halves were covered only by the engine's unit tests, which see the
`ImportReport` and never the notice drawn from it — so this runs the REAL sweep
and reads what the operator reads.

```bash
bash tools/selftest/run_table_folder_test.sh   # ~40s: the sweep, the notices, the cards
# ... TABLEFOLDER: PASS unsupported=7 failed=1 named=8 cards=3 matched=yes
#     TABLEFOLDER: FAIL launch | FAIL missing | FAIL summary | FAIL named=…
#                 | FAIL truncated | FAIL toast | FAIL library
```

How it fits together:

- **The scenario** is the `selfTestTableFolder*` Timer block in `qml/Main.qml`,
  gated on the `SelfTestTableFolder` context property (BOTH `VGR_SELFTEST=1` and
  `VGR_TABLEFOLDER_TEST=1`) with the folder in `VGR_TABLEFOLDER_DIR` — its own
  env vars like every other scenario, so a plain `VGR_SELFTEST=1` run (how the
  NDI probes launch the app) never imports anything.
- **The sweep is the real one**: `TheTableService.importSermonFolder(root)` is the
  function the folder picker itself calls, so the same worker thread, the same
  progress and the same notices run; only the dialog is skipped.
- **The folder holds nothing importable** (six spreadsheets, a PNG, and a `.txt`
  that is really a spreadsheet), which is what makes the run safe to repeat: with
  0 imported the sweep skips its save and its reindex, so the operator's own
  `%LOCALAPPDATA%/bps/the_table.json` is untouched — and its SHA-256 before and
  after is asserted, so a future change that writes on an empty import fails here.
- **Eight refusals is three more than the old cap of five**: the announcement
  that used to say "and 3 more" is exactly what `FAIL truncated` looks for, so a
  list that stops early cannot pass.
- **`cards=`/`matched=`** are the on-screen half: the overlay stacks one card per
  notice (`selfTestToastCard` children) and the NEWEST card must have been handed
  the refusal sentence verbatim — the same string collected from the notice
  center, so a wording that reached the queue but not a card is a failure.
- **The wording itself** is printed by the runner (`summary :` / `refusals :`),
  because the sentence is the deliverable: `Imported 0 sermons · 0 already in ·
  1 failed · 7 not sermon files`, then one clause per refused file.

## What each output's style does to a slide (Settings · Outputs says it)

`run_outputs_layout_test.sh` guards the report "scripture and The Table work,
shows do not". A style that restyles the content tabs' designs while a show keeps
its own authored layout, a style that also lays shows out, and a style with no
template layout at all are three different pictures on air — and the Outputs
screen made them indistinguishable: a row said WHICH style it wears and nothing
about what that style does to the slide. The row now carries one muted line under
its Style pill, and the line is the ENGINE's own answer
(`OutputListModel::styleLayoutNote` → `SlideRules::LayoutScope`, the same call
`SceneBuilder::ComposeSlide` composes with), applied to the very spec that row
pushes — so the sentence on screen cannot drift from the picture on the wire.

This run is also what found the second half of the fault: the composition rule
had to tell an EXPLICIT shows pick apart from a style that merely names "shows"
as its own type. A template chosen in the style's Shows slot (Settings · Styles
→ "Template for Shows" → Change) lays a show out — the picker promises exactly
that — while the style's whole-style template (and a contentType that is merely
the default) leaves a slide the operator authored on the canvas alone. The row
prints whichever of the three answers the engine's rule gives.

```bash
bash tools/selftest/run_outputs_layout_test.sh 60
# ... [SELFTEST-OUTS] row=0 output="Main Output" style="New Style 1" scope="Lays out shows too" note="..."
#     OUTS: PASS rows=2 styled=2 described=2 scopes=2/2
#     OUTS: FAIL launch | FAIL notrun | FAIL missing | FAIL empty | FAIL undescribed
```

How it fits together:

- **The scenario** is the `selfTestOuts*` Timer block in `qml/Main.qml`, gated on
  `VGR_OUTS_TEST` (+ `VGR_SELFTEST`), armed in `src/main.cpp` (a plain
  `VGR_SELFTEST=1` run must never open Settings or quit early).
- **It reads the ROW AS RENDERED**, not the model: each card is given an
  `objectName` (`selfTestOutputRow_<index>`) and the scenario reads the
  delegate's own `styleLayout` — the value the line under the Style pill binds
  to — so a role that resolved in the model but never reached the row reads as
  `described < styled` and fails.

The same scenario also checks that EVERY row prints a runtime line of its own
(`reported == rows`, `statusText` opening with the row's own kind), and
`run_outputs_live_test.sh` takes that line through a live session: it puts one
real slide on air, opens Settings · Outputs, and prints every row's status while
live, while blacked out (a real click on the wall's Blackout control, with the
dialog closed so the click lands on the control), and after the blackout ends.

The report behind it: "anything the notification does for one output should be
implemented for all — it seems like the notification is only instantiated in one
output … fps change, blackout, whatever." The row's live line used to exist only
for the kinds with a sender (NDI, SDI), so an HDMI row said "Inactive" and
nothing else. Every kind of output now reports the same facts, measured by the
engine (`RenderEngine::OutputFeedAges` — frames delivered and the age of the
last present) and published per row by `LiveOutputService.outputRuntime`.

```bash
bash tools/selftest/run_outputs_live_test.sh 120
# ... [SELFTEST-OUTSLIVE] live row=0 kind="HDMI" status="HDMI · live · 2 frames delivered"
#     [SELFTEST-OUTSLIVE] blackout row=1 kind="NDI" status="NDI · sending · 502 frames · blackout up"
#     OUTSLIVE: PASS rows=2 live=2 blackout=2 restored=2
#     OUTSLIVE: FAIL launch | notrun | missing | stale | blackout | restore
```

The scenario is the `selfTestOutsLive*` Timer block in `qml/Main.qml`, gated on
`VGR_OUTSLIVE_TEST` (+ `VGR_SELFTEST`), armed in `src/main.cpp` like the rest.

The same component is on the monitor wall: each tile carries its output's state as
a colour dot (`components/OutputRuntimeDot.qml`, built on the reusable hover
wrapper `components/HoverDetail.qml`) — green frames are landing, amber blacked
out or stalled, grey off air / waiting / switched off — and the frames and the age
of the last one are in its hover tip ("Main Output · live · 3 frames · last 222 ms
ago"). The runner parks the real pointer on the dot with `SelfTest.move` and
fails if no tip with a frame count comes back, because a colour-only indicator is
unreadable if its detail cannot be reached. Both surfaces (wall and Outputs rows)
instantiate that one component rather than deriving the state again.
- **`described` must equal `styled`**: every row wearing a style has to print one
  of the three documented answers. `scopes=` prints how many rows answered, so a
  machine whose styles are all background-only is visible in the verdict instead
  of passing in silence.
- **The row's wording has one home.** The three phrases live in `Main.qml`'s
  `outsScopePhrases` and in `OutputListModel::styleLayoutNote`; the scenario
  matches the row against the former, so a reworded sentence that never reached
  the row (or one changed on only one side) fails the run rather than passing as
  a vague success.

## The media-only GO LIVE: the outputs, not the tile

`run_blank_live_test.sh` guards the report "the moment it is only media it doesn't
go" — scripture or The Table on air renders the camera/screen take and the text
fine, but GO LIVE with only a take (or media on air) and no content left every
real output black. GO LIVE with nothing staged sends ONE blank carrier slide
(`LiveOutputService::goLiveBlank`: no blocks, no title, no text) so the outputs
show the compositor's layers over the active style's background — and the tile
did show the take, because the QML preview already refuses to bake a template
into a blank slide. The ENGINE did not: it rendered the style's template on the
carrier, and a template's decorative blocks are painted in the TEXT pass, i.e.
ABOVE the compositor's video layer, so a full-stage background slab became pure
cover once there was no text to sit behind. `SceneBuilder` now skips the style's
template (whole-style and per-family) when the slide has no blocks, no title and
no text; a slide with content still renders through it.

The run covers BOTH halves, because they fail differently: the blank carrier
(with nothing to sit behind, so a template's slab is pure cover) and the same
take with CONTENT on air (the operator's own styles wear designs whose leading
blocks cover the stage together — a full-stage slab, or a panel plus a bottom
bar — which is the state that used to bury the feed). Its engine-side rule is
the layer split: a design's backdrop paints on the scene's `bg` layer, the taken
input and media on `media`, the design's decoration and text on `text`, so the
feed is the picture and the design sits over or under it by design.

```bash
bash tools/selftest/run_blank_live_test.sh 45
# ... BLANK_LIVE: PASS backdropUnderVideo=2/2 stageCovered=yes coveringScenes=2/2 feedInScene=yes tileLayers=backdrop2<feed3<content6
#     BLANK_LIVE: FAIL nolive | FAIL nopremise | FAIL noframe | FAIL black
#     BLANK_LIVE: FAIL backdrop | FAIL tilelayers
```

How it fits together:

- **The scenario** is the `selfTestBlankLive*` Timer block in `qml/Main.qml`,
  gated on `VGR_BLANKLIVE_TEST` (+ `VGR_SELFTEST`), armed in `src/main.cpp`. It
  takes the first captured window that is not this app's own window (the same
  `VideoSourceListModel` roster the Media pane's click-to-preview uses), presses
  GO LIVE — which lands on the blank carrier because the run opens no show —
  pushes a real content slide through the same live session, then quits,
  clearing the take on the way out.
- **The verdict is the ENGINE's own pixels**, not a screenshot: `RenderOutputs`
  logs `output frame sanity: '<buffer>' … allBlack=` on every state change, and
  the runner reads the lines after `live output started from content` (the
  blank carrier) and after `live output replaced with content` (the content
  slide) separately, so the content half can never hide a black carrier or the
  other way round. A black frame there is the frame the output window, HDMI and
  the NDI sender consume, so it needs no window to be trustworthy.
- **"Not black" is not the whole check**, because a translucent backdrop over
  the feed is not black either (the app's own designs cover the stage with a
  40%-black slab, so a buried feed there is a dimmed feed): the runner also asks
  the engine what the content phase was COMPOSED of — `taken input layer
  present` (the feed really was in the built scene) and `backdrop layer <n> of
  <m> blocks under the video layer (stage covered: yes|no)` (both halves of the
  split `SceneBuilder`'s rule produced). With the scene's layer table (`bg` 0 <
  `media` 1 < `text` 2), a demoted block sits BELOW the feed in that same scene,
  so **paint that reaches the stage must be demoted**: `stage covered: yes` with
  `n = 0` is covering paint left as content, i.e. painted over the live feed, and
  the run reports `FAIL backdrop`. Every content-phase scene is checked (each
  output wears its own style), and `stage covered: no` — a design whose paint
  never reaches the stage, correctly left as decoration over the feed — is said
  out loud in the verdict rather than passed over in silence.
- **The check is measured, not assumed**: defeating the rule on purpose (forcing
  the leading-run verdict false, so covering paint stays content) makes the run
  report `BLANK_LIVE: FAIL backdrop — the design's paint covers the stage but was
  left as content: it paints OVER the live feed instead of under it` (exit 14)
  while the frames stay non-black, which is exactly the failure a frame check
  alone cannot see.
- **The TILE is checked, not just the engine**: the same phase must also compose
  that order on the monitor tile, read off its own layer stack — the design's
  backdrop paint BELOW the taken-input layer, the block content ABOVE it
  (`tileLayers=backdrop2<feed3<content6`; `FAIL tilelayers` when they are not in
  that order, `FAIL nopremise` when the pane has no such layer to read). This is
  the half a preview can get wrong while every engine-side check still passes:
  the tile is a second renderer, and it only knows where a design's paint
  belongs because it ASKS the engine
  (`EngineBridge.backdropBlockIndexes` → `SceneBuilder::PlanBlockLayers`) rather
  than carrying its own copy of the rule.
- **Both halves of the phase split are read by TITLE, not by verb**: pushing
  content while the carrier is on air usually REPLACES it, but the platform also
  restarts the live session on its own (an output-roster change logs 
  `live output stopped`), which turns the same push into a fresh START. Keying on
  "replaced with" alone reported the whole content phase as missing on those
  runs, so the carrier is matched as a live start with an EMPTY title and the
  content phase as the first live line carrying the scenario's own slide title.
- **Latest frame per output decides**: one black frame while a per-output pass
  warms up is normal, a feed that STAYS black is the defect.
- **Premises are checked first**: a machine with no capturable window (or a take
  that was refused) reports `FAIL nolive`, and a phase whose engine side never
  said what it was built from reports `FAIL nopremise`, rather than passing
  vacuously — the same trap the style-background scenario fell into when its
  tile and its model described two different outputs.

## The blackout control: an EMPTY frame, on every output, on purpose

`run_blackout_test.sh` guards the wall toolbar's Blackout control. "Nothing at
all on screen" used to be a side effect of Clear all (it sent a `clearStage`
carrier); Clear all now keeps the output's own branded look — the colour and the
background image survive a clear, which is what an operator clearing content
expects — so the app had no way left to ask for a genuinely empty screen. The
Blackout row on the wall's toolbar (`MonitorWall.qml`) is that gesture, and it
is deliberately separate: clearing CONTENT and hiding the picture from an
audience are different actions.

```bash
bash tools/selftest/run_blackout_test.sh 60
# ... [SELFTEST-BLACKOUT] pressed visible=true blackedOut=true live=true
#     [SELFTEST-BLACKOUT] ended blackedOut=false live=true blocks=1
#     BLACKOUT: PASS content=28 blackout=19 outputs=checked
#     BLACKOUT: FAIL nocontrol | FAIL nolive | FAIL notpressed | FAIL noend
#     BLACKOUT: FAIL noframes | FAIL lit | FAIL notblack | FAIL staged
```

How it fits together:

- **The scenario** is the `selfTestBlackout*` Timer block in `qml/Main.qml`,
  gated on `VGR_BLACKOUT_TEST` (+ `VGR_SELFTEST`), armed in `src/main.cpp`. It
  puts ONE real slide on air, clicks the control with `SelfTest.clickItem` (a
  real click at the rendered button — not a direct service call), clicks it
  again, and quits.
- **A click and its state are different ticks.** `clickItem` posts a real mouse
  event, so every press is reported from its own timer a couple of seconds
  later; reading `blackedOut` right after the click reads the state from BEFORE
  it and makes a working control look broken (which is how the first run of this
  harness failed).
- **The verdict is the ENGINE's pixels.** `RenderOutputs` logs `output frame
  sanity: '<buffer>' … allBlack= maxAlpha=` whenever a presented frame changes.
  The runner cuts the log into the three phases the engine marks itself — live
  content `'Blackout Probe'`, a live replacement with an EMPTY title (the
  carrier), and the title again (the restoration) — and requires each output's
  LATEST frame to be non-black, then black (`meanLuma=0 … allBlack=yes`,
  `maxAlpha=0`), then non-black. That is the frame the HDMI window and the NDI
  sender consume, so "the tile looks cleared" can never pass for the outputs
  being cleared.
- **The way out is part of the test.** The second press must come out of the
  blackout (`blackedOut=false`, the slide's blocks back) through the same live
  session — no STOP/GO LIVE in between — which is what `endBlackout()` restores
  from the raw set it saved before the carrier went up.
- **STOP while blacked out has its own answer.** The last phase blacks out a
  second time and presses STOP: the staged slot must then hold the CONTENT the
  blackout was hiding (`stagedClearStage=false`, its block back), never the
  carrier, or the next GO LIVE would put the black frame on the screens again.

## The startup-forensics verdict: said out loud, and only when it matters

`run_forensics_test.sh` guards the report "the app says a crash was captured when
nothing was written down" from the other end: at every launch the app proves the
tooling it would capture a crash WITH actually runs on this machine (see
`CrashHandler.h`'s `VerifyForensicsTooling`), and that verdict used to reach
`engine.log` and nothing else — so the operator who most needed it, the one
whose crash left no evidence, was the one who would never read it.

```bash
bash tools/selftest/run_forensics_test.sh   # ~80s: two launches, ~40s each
# ... FORENSICS: PASS broken=Incomplete+noted healthy=Ready+quiet
#     FORENSICS: FAIL launch | FAIL missing | FAIL verdict | FAIL row | FAIL note
```

How it fits together:

- **Two cases, because a verdict that is always loud is as wrong as one that is
  always quiet.** `VGR_FORENSICS_DIR` (the probe's own override) is pointed at a
  scratch folder holding an EMPTY `analyze.bat` for the broken case — a
  packaged-looking layout that cannot read a report — and at an empty folder for
  the healthy one (a build that ships no capture tools, which is a WORKING
  configuration: the text record and the minidump remain).
- **The verdict travels through the real path**: the probe (worker) →
  `EngineBridge.publishCrashTooling` (interface thread) → the two properties QML
  binds, so nothing here reads a parallel copy of the state.
- **The row is read as DRAWN**: the scenario looks the pill and the detail line up
  in the live item tree (`selfTestCrashToolingPill` / `…Detail` on Settings ·
  General's "Crash reports" card) and prints their own text, so a row bound to
  the wrong property fails instead of passing on the raw value.
- **The note needs a crash to ride**, and only a previous run that went down
  produces one: the runner SEEDS one record into the app's own crash log
  (`%APPDATA%/VGR/VGRPresenter/crashes/crash.log`) in the shape the handlers
  write and `ConsumePendingCrashSummary()` parses. The launch under test consumes
  it exactly as it would a real one (reported once, then rotated aside) — which
  is why the script also prints that it seeded it.
- **`inToast=`** is the user's half: the note must be in the notification centre,
  which is where the notice is read from, not merely handed to `EventBus`.

## The layout probe — a measurement, not a verdict

`run_layout_probe.sh` answers "does this interaction park the interface thread, and
does it throw the lists away while it does" for the two interactions the hang captures
name: a filter applied to a library list, and a pane switch. It is the one harness here
with no PASS/FAIL line — it prints numbers, because the question is "how much", not
"is it right".

```bash
tools/dev_cycle.sh --build-only
tools/selftest/run_layout_probe.sh        # ~40 s (the walk is 30 steps, one per second)
```

Each `LayoutProbe phase=<tag> …` line carries, for that step:

- `stallMax` / `stallSum` — how long the interface thread was PARKED, measured by the
  thread itself: a 16 ms heartbeat timer reports its own overshoot (under 8 ms of
  overshoot is timer noise). This is the freeze, not the work behind it, so a
  DirectWrite/relayout pile-up cannot hide from it.
- `<list>Built` / `<list>Dropped` — the delegates the shows table, both design grids,
  the media grid and the passage list built and THREW AWAY during the step. builds ==
  drops is a list rebuilding itself; builds > 0 with drops == 0 is a list growing a new
  tail.
- `<list>Rows` — what each model holds after the step, so a rebuild of a 6-card library
  cannot be read as one of a 680-row one.

The same log carries the C++ `FrameProbe …` lines (`build/logs/engine.log`): the frame
to frame time the render thread actually painted, 1/s, with >33/>50/>100 ms counts. The
freeze and the frames are the same stall seen from two threads.

The walk: a cold switch through every library tab; the SAME-WIDTH pane pairs against the
WIDTH-CROSSING ones (a reference tab used to take the template column's whole width); a
filter on the shows table and on both design grids; the passage pane's own filter and
citation search; the scroll phase (below); then the pane walk again, warm.

### The scroll phase — the cost paid per frame, not per settle

Every step above is a SETTLE (a switch, a filter, one second). The lag the operator
actually reports — "scripture, templates and the table are sluggish when I scroll" — is
the other half: cost paid continuously while rows enter and leave the viewport. So the
walk has a scroll phase: seven steps, each scrolling ONE named list through its whole
content with a linear 700 ms animation (a fast wheel drag, no flick inertia, so two runs
compare), and each writing its own `LayoutProbe phase=scroll-… scrollStart=1 …` record:

- `frames` / `fps` / `avgFrameMs` / `worstFrameMs` / `jank33` / `jank50` — the frame times
  the app produced DURING the scroll, from a `FrameAnimation` on the interface thread
  (`frameTime` and `elapsedTime` are in seconds; the record converts). The first frame is
  skipped: its `frameTime` is the idle gap since the last painted frame, not a scroll
  cost. `wallMs` is the wall clock the same run took and `meterMs` the meter's own
  window — so a scroll the app produced NO frames during (meterMs 0, wallMs 700) cannot
  be read as an instant one.
- `scrollBuilt` / `scrollDropped` / `scrollEvals` — the scrolled list's own delegates and
  row binding evaluations over the scroll window. `scrollEvals` is the attribution: a
  scroll with a large `scrollBuilt` and small `scrollEvals` is paying row construction,
  one with evals scaling with the distance is paying bindings.
- `contentPx` / `viewPx` / `scrollPx` / `listFound` — how far it actually moved. A short
  list, an unlaid-out one and a missing objectName are each visibly different from a
  cheap scroll (the armer retries a missing list a few times first — a pane mid-switch can
  hide its children for a moment — and reports `listFound=0` only after that).

The record is written when the scroll STOPS, not at the step timer's next tick: a step
whose list is slow to draw overruns the 1 s cadence (measured: the templates grid's 37
cards took 7.1 s of a 1 s step), and a record taken at the tick would be written before
the scroll began. The row binding counts live in a plain JS accumulator, not a QML
property — incrementing a QML property from inside a binding is reported as a binding
loop (the first cut of this did exactly that, once per row in the pane).

The lists it scrolls: the shows table, The Table's passage list, Scripture's passages,
bible list and chapter list, and both design grids.

#### What it measured (one run, this machine)

Run-to-run numbers move with what else the machine is doing (this is a measurement, and
the machine is the instrument), so read the columns against EACH OTHER, not against a
millisecond someone quoted once:

| list | rows it scrolls | scrolled | fps | worst frame | jank > 33 ms | built / dropped | evals |
|---|---|---|---|---|---|---|---|
| shows table (`reuseItems`) | 680 | 21 478 px | 46.5 | 22.2 ms | 0 | 22 / 0 | 617 |
| The Table's passages | 945 | 32 831 px | 29.1 | 47.2 ms | 12 | 299 / 307 | 299 |
| Scripture's passages | 31 | 872 px | 40.9 | 37.7 ms | 2 | 14 / 13 | 14 |
| Scripture's bible list | 85 (1 856 px) | 1 539 px | 46.7 | 23.6 ms | 0 | 0 / 0 | 0 |
| Scripture's chapter list | 56 (1 408 px) | 1 091 px | 44.6 | 22.8 ms | 0 | 0 / 0 | 0 |
| templates grid | 37 | 2 174 px | 5.2 | 614.9 ms | 6 | 26 / 25 | 75 fit solves |
| overlays grid | 6 (382 px) | 73 px (nothing else to scroll) | — | — | 0 | 0 / 0 | 0 |

Read together, those columns say where the scroll lag is, and it is not the same cause in
each list:

- the SHOWS table is the shape to copy: `reuseItems` means a 680-row scroll built 22 rows
  and threw away none, and the rows' bindings evaluated about once per row the scroll
  passed (617 ≈ 21 478 px ÷ 32 px rows) — it holds ~47 fps.
- The Table's PASSAGE list was the surprise: comparable distance, but ~300 rows built AND
  ~300 thrown away, ~12 janky frames, ~29 fps — it had no `reuseItems`, so it re-created
  every row it scrolled past. **Fixed** (see the after table): the rows the viewport
  finishes with are pooled and re-bound now, so the same scroll builds ~18 and throws away
  none, and the list holds ~45 fps with no frame over 33 ms.
- the TEMPLATES grid could not scroll without stalling: ~26 of its 37 cards built and
  ~25 dropped for a 2 174 px scroll, several frames over 33 ms and a worst frame of
  several hundred ms (371-615 ms across runs; the whole animation stretched from 700 ms to
  1.2-1.5 s because the thread was that busy). The `evals` column is the attribution:
  those are the cards' fit solvers — an 11-step bisection re-measuring a hidden twin Text,
  per text block of every card built — the same cost the `cardFitSolves` field of a filter
  step shows. **Fixed, to the limit of what pooling can do** (after table): the grid now
  keeps the cards it has built and throws away none (26/25 → 0/0), so it stops stalling —
  the 614.9 ms frame becomes 84-108 ms and the animation keeps its own 831-857 ms. It is still
  the slowest scroll in the app (~23 fps), because showing a different template in an
  already-built card still re-makes that card's block delegates and re-runs their fit
  solvers (`cardFitSolves` 75 → 83): that is content work the pool cannot remove, and it is
  the next thing to fix.
- the SCRIPTURE sidebar lists (bible, chapter) scroll free: a Repeater inside a Flickable
  builds everything once and then costs nothing per frame — no builds, no evaluations,
  ~46 fps. Scripture's own PASSAGE list behaves like The Table's (14 / 13 here) — its
  chapter is simply shorter.

So the report's "scripture, templates and the table" splits into three different causes:
the scripture SIDEBAR is not the problem (it is already free), the passage lists rebuild
rows they need not (a `reuseItems`-shaped fix, worth ~300 reconstructions per scroll in The
Table), and the design grids rebuild whole cards on nearly every scroll — the filter-cost
finding below, arriving through a scroll instead of a filter.

#### The after table — the same phase, after the two fixes

The two fixes are a `ListModel` the design grids are fed instead of a fresh array (it
reports the rows a filter inserts and removes, where handing over the array was a model
RESET that destroyed and rebuilt every card in view), and `reuseItems` on both the design
grids and the passage list. The same probe, same machine, next run:

The AFTER column is three runs (the same build, run back to back), given as a range where
they differ — the churn columns are 0/0 every time, which is the part that matters. The
BEFORE column is the one run in the table above.

| list | scrolled | built / dropped BEFORE | built / dropped AFTER | fps BEFORE → AFTER | worst frame BEFORE → AFTER |
|---|---|---|---|---|---|
| shows table (`reuseItems` already) | 21 478 px | 22 / 0 | 22 / 0 | 46.5 → 46.4-46.9 | 22.2 → 22.8 ms |
| The Table's passages | 32 831 px | 299 / 307 | **18 / 0** | 29.1 → **44.0-44.5** | 47.2 → **22.5-25.7 ms** |
| Scripture's passages | 929 px | 14 / 13 | 11 / 0 | 40.9 → 36.6-40.8 | 37.7 → 41.0-46.2 ms |
| Scripture's bible list | 1 539 px | 0 / 0 | 0 / 0 | 46.7 → 44.8-45.0 | 23.6 → 22.8-22.9 ms |
| Scripture's chapter list | 1 091 px | 0 / 0 | 0 / 0 | 44.6 → 45.4-45.8 | 22.8 → 23.2-24.3 ms |
| templates grid | 2 174 px | 26 / 25 | **0-1 / 0** | 5.2 → **22.1-22.9** | 614.9 → **83.7-108 ms** |
| overlays grid | 73 px (fits the view) | 0 / 0 | 0 / 0 | — → 44.2-44.9 | — |

The grids also check themselves now: after every sync each design grid logs
`[DesignRows] <noun> rows=N want=M match=1` — its model against the service's own answer,
row for row — and the probe prints the distinct lines and the mismatch count under the
walk. Every after-run logged the same 18 checks (37/37 and 14/14 templates, 6/6 and 3/3
overlays) with **0 mismatches**, which is what makes the churn columns mean what they say:
a cheap filter that showed the WRONG cards would score exactly the same on built/dropped.

What the columns say, and equally what they do NOT say:

- the two lists that built AND threw away delegates — The Table's passages and the templates
  grid — now build a pool-sized handful and throw away NONE. That is the whole of
  `reuseItems`: the row an item view has finished with is re-bound to the next index instead
  of destroyed. The Table's passages went from 299/307 to 18/0 at ~45 fps, and its ~300
  reconstructions per scroll are gone (the 350 `refVerseEvals` are the re-bound rows' own
  label bindings, not rebuilds).
- the templates grid stops STALLING, not being slow: 26/25 → **0/0** and a 614.9 ms worst
  frame → 84-108 ms, with the animation finishing in 831-857 ms instead of stretching 700 ms
  of motion past 1.2-1.5 s. But it still spends ~40 ms on an average frame (22.9 fps, 12 of 19
  frames over 33 ms) and its `cardFitSolves` barely moved (75 → 83 solves). **Those two
  numbers are the honest limit of this fix**: pooling removes constructing and destroying a
  card, and cannot remove the cost of putting a DIFFERENT design into a card that already
  exists — the preview's block delegates are still re-created and every text block still
  runs its 11-step fit solver. The grid is therefore the one list whose scroll is still
  expensive, for content reasons, and that is the next thing to fix (a cheaper card at card
  size — see the filter finding below for the same cost arriving through a filter).
- Scripture's own passage list moved the same way (14/13 → 11/0) on a shorter scroll; its fps
  reading moved the other way in the first after-run (40.9 → 36.6) and back in the second
  (40.8), so it should not be read as a regression either way: a 929 px scroll is ~20 frames
  and one slow frame swings it (46.2 ms in one run, 41.0 ms in the other). The lists that
  build nothing (bible, chapter) are the controls here, and they did not move.
- the shows table is the control that did not need the fix, and it did not change (22/0,
  ~47 fps, both runs) — which is what makes the other rows readable as the fix and not as
  the machine.

Where the same reset showed up in the WALK (the filter steps, `stallMax` in ms, delegates
built/dropped on the grid the step filters):

| step | before | after |
|---|---|---|
| overlays filter: 6 → 3 cards | 130-281 ms; 3 built / 6 dropped | **59-141 ms; 0 / 0** |
| overlays filter cleared: 3 → 6 cards | 250-392 ms; 6 built / 3 dropped | **116-176 ms; 0 / 0** |
| templates filter: 37 → 14 cards | 502-694 ms; 13 built / 13 dropped | 621-694 ms; **10 built / 0 dropped** |

The overlays grid is where the model earns its name: with only 6 cards the whole roster is
in view, so a filter removes and re-inserts rows whose CARDS ARE THE SAME CARDS — and the
first panel of this table started rebuilding all of them on every one of those steps (3
built/6 dropped, then 6 built/3 dropped, each with a 130-390 ms stall). Now the same steps
build and destroy nothing at all, and the stall is the pointer work: 59-176 ms.

The templates filter is the same step one size up, and it is where the row model cannot
help: filtering 37 templates down to 14 leaves a DIFFERENT ten designs on screen (the query
matches a different set), so those cards genuinely have new content and their block
delegates and fit solvers are re-made whatever the model does — `cardFitSolves` is 52-54 on
both sides of the fix. What the model did remove is the destruction: 13 cards were thrown
away before, none now. The 621-694 ms this step still costs is that content work, and it is
the same cost the templates grid pays while scrolling; the fix for both is a card that is
cheaper to re-fill, not a better model.

### What it measured

Aggregate over the whole walk (34-37 one-second windows):

| run | worst frame gap | frames > 33 ms | > 50 ms | > 100 ms |
|---|---|---|---|---|
| before the layout work | 466.5 ms | 134 | 67 | 18 |
| after                 | 356.6 ms |  35 | 11 |  5 |

Per interaction — `stallMax` in ms, and the list churn in delegates built/dropped:

| step | before | after |
|---|---|---|
| switch to/from a reference tab (the dock used to resize the whole pane stack) | 95-208 ms; 10-13 template cards churned while the pane was HIDDEN | 15-31 ms; 0 |
| shows filter: 680 → 57 rows | 190 ms; 57 built / 680 dropped | 12 ms; 19 built / 19 dropped |
| shows filter cleared: 57 → 680 rows | 399 ms; 680 built / 57 dropped | 23 ms; 19 built / 19 dropped |
| overlays filter: 6 → 3 cards | 130-281 ms; 3 built / 6 dropped | 68-141 ms; 0 built / 0 dropped |
| overlays filter cleared: 3 → 6 cards | 250-392 ms; 6 built / 3 dropped | 116-176 ms; 0 built / 0 dropped |
| templates filter: 37 → 14 cards | 502-694 ms; 13 built / 13 dropped | 621-694 ms; 10 built / 0 dropped |
| passage filter and citation search (945-row chapter) | 21-141 ms | 13-19 ms |
| passage rows rebuilt, whole walk | 0 built / 0 dropped | 0 built / 0 dropped |

The findings the numbers settled — each one is why the code reads the way it does now:

- the shows table was a Flickable + Repeater over 680 rows (the engine's own log line:
  `Show library scanned: 680 show(s)`): every row realized at boot, and every row
  re-shaped through DirectWrite — the frames the hang captures show the interface thread
  parked in — whenever the dock resized or the subtree was re-polished. It is a ListView
  now: ≈15 rows instead of 680, and 19 built/dropped per filter instead of 737.
- the dock used to resize the whole pane stack when a reference tab was selected
  (Scripture/The Table took the template column's space), so the panes that were HIDDEN
  re-laid-out, re-shaped their text and rebuilt their grids too. The panes state their
  own width now, and a tab switch changes only which pane is visible.
- the PASSAGE list is the one list that was already clean: 28 rows built once for a
  945-row chapter and never dropped, through every filter, search and switch. It is
  already a ListView, and with the dock no longer resizing under it, its text is not
  re-shaped on a switch either (its own switches went 141 ms → 20 ms).
- the design grids rebuilt every card on a filter change (~30 ms per card: 13 built / 13
  dropped either way, 370-527 ms for one templates filter). Two cheap attempts were
  measured and reverted: `reuseItems` alone changes nothing THERE (a model reset DESTROYS
  the released cards rather than pooling them, so the previews are remade either way), and
  `cacheBuffer: 0` bought only ~35 ms of that phase while moving card building into the
  scroll. The fix is the one the app's other lists already use: a model that reports the
  rows a filter removes and inserts instead of resetting. `DesignLibraryPane` now keeps a
  `ListModel` and syncs it to the service's answer row by row — remove what is gone, move
  what only changed places, insert what is new, and write a row again only when that row's
  design actually changed (the row's JSON is its signature) — and both grids scroll with
  `reuseItems`. Measured: the overlays grid's filter steps build and destroy nothing now
  (3/6 and 6/3 → 0/0) and their stalls fall to 59-176 ms; the templates grid's filter no
  longer destroys anything (13 dropped → 0) but still costs 621-694 ms, because the ten
  designs the query leaves on screen are different designs — their block delegates and fit
  solvers are genuinely remade (`cardFitSolves` 52-54 on both sides). The model removes
  churn; it cannot remove that. Making the card cheaper to re-fill is the open item.

## The show document's queued jobs: does the verdict actually arrive?

`run_show_io_test.sh` guards the change that took `ShowService`'s document work off the
interface thread (docs/gui_thread_audit.md §3.15): the ten operations a QML entry point
used to drive `PresentationDocument` with — save, save-as, save-copy, open, peek, next
timer, the slide edits, the two template calls and the text rewrite. They answer with a
TOKEN now and the verdict arrives later, through `documentJobFinished`. That contract is
invisible to a build and to a clean boot: a save whose verdict never comes back and a
save that never writes its file produce exactly the same pixels, and so does a read that
is still in flight when someone looks at it.

```bash
bash tools/selftest/run_show_io_test.sh 90   # boot + the engine's stores ~30s
```

It drives the chain end to end in the real app, against real files in `build/` that it
deletes before the launch — so anything it finds afterwards is this run's own work — and
checks both halves every step: what the verdict said AND what is on disk or in the
published answer.

| step | what it proves |
|---|---|
| save | the verdict arrives (not just the request), the file exists when it does, and the show reads back with its slides |
| open | the same file opens back through the call the show screen uses, with the payload's slides |
| peek1 | a show nobody has read yet answers `pending` — not "there is no such show", which is a different fact and blanks the centre page |
| peek2 | the answer landed, and `peekRevision` MOVED — the centre page binds to it, so a "changed" signal whose value stays put is nothing to re-evaluate |
| timer | the next timer applied, then read back out of the peek |
| peek3 | the timer wrote the show, so the published answer is GONE — this read must be a fresh miss, never the old answer (the bug a published cache gets wrong) |
| copy | the recovery copy, the one write nobody asked for: verdict and file |
| quit | the app's loop RETURNED (`app event loop returned` in the log). A finished scenario in a still-running app reads like a pass otherwise, which is why it is checked |

Two of those are in the table because they caught something. `peekRevision` never
incremented before this harness existed — `publishPeek` only emitted the signal — so
`revision=0` under `step=peek2` is a FAIL now (it reads 1). And a quit issued from inside
the job-verdict handler does not take: with `SelfTest.quit()` called straight from that
handler, engine.log's `UI event-loop heartbeat` kept ticking every 5 s for the ~80 s the
probe watched after the verdict and `app event loop returned` never appeared, while the
same call from a `Timer`'s own tick returns the loop within a second — which is how
Main.qml's scenario quits. If you write a scenario of your own against a queued job,
quit from its own tick and let the harness check for that line.

```
SHOWIO: PASS save=ok open=ok peek=ok timer=ok copy=ok steps=9
SHOWIO: FAIL launch | missing | save | nosavefile | open | peek | pending | timer
        | copy | nocopyfile | revision | quit
```

The harness force-kills the app after the loop returns rather than waiting the process
out: the teardown behind a returned loop takes ~150 s on this machine (Qt's window/DXGI
shutdown — the same slow exit `run_canvas_cursor_test.sh` documents), and it says so on
the `==` line so a normal run is not read as a failure. What it does NOT do is wait for
a verdict that never comes: the grace period is bounded, and a run that needed the kill
after a missing `app event loop returned` fails on the quit check above.

## The quit path: unsaved work, a live output, and what the teardown costs

`run_quit_test.sh` covers the one user path that ends the process, and the one the
exit path was never measured on: **closing the window with a show that has unsaved
changes while an output is live.** Everything about it is invisible to a build.

```bash
bash tools/selftest/run_quit_test.sh 90 240   # scenario 90s, teardown measurement 240s
```

The scenario (Main.qml, `VGR_QUIT_TEST=1`) saves a two-slide show to a real file in
`build/` first — so the close-time save needs no native save-path picker, which
cannot be driven from here — makes it dirty again, calls `goLive()`, and then closes
the window the way the title bar does. The steps it prints, and what each proves:

| step | what it proves |
|---|---|
| seeded | the show has a file, so the close-time save is the one being judged |
| armed | the show is DIRTY and an output is LIVE when the window is closed — the case every "it hangs when I close it" report is about |
| close1 | the unsaved-changes guard REFUSED the close and the dialog came up. A close that went straight through loses the show silently |
| answer | a real synthetic click on the dialog's Save. `saveButton=` is probed here, after the dialog's 130 ms fade, and not in `close1`: a card at opacity 0 is not a visible item, so probing it earlier read `false` on a dialog that was up and clickable |
| saved | the queued save's VERDICT landed, `ok=true`, file on disk. That verdict is what sets `closeConfirmed` and closes the window |
| quit | `app event loop returned` — the loop really returned. A finished dialog in a window that never closed reads like a pass otherwise |
| teardown | the process is gone, and `QUIT: PASS … teardown=<s>s` carries how long it took |

It reads two files on purpose: the `aboutToQuit` step timings come from
`logs/engine.log` (only this run's lines — the line count is taken before the
launch), while `main: leaving` and the post-routine marker come from the console
log. Until `EngineBridge.shutdown` runs, a line goes to the engine's logger and not
to the console; after it, the logger is gone and the same call reaches stdout only.
Reading one file for both is what made the first fixed run report a marker that
"never landed" while the line sat in the other file.

Measured on this machine, three runs: **69–87 ms across all aboutToQuit steps**
(largest `TheTableService.shutdownIndexing`, 56–69 ms), **~570 ms** from
`main: leaving` to `QCoreApplication teardown reached` (the QML engine and the
windows), and **1 s** from the loop returning to the process going. The ~150 s the
audit recorded for `run_show_io_test.sh` does NOT reproduce here — with the load
this harness applies (live output, dirty show), the exit is a second — which is why
the step timings and the two tail markers exist: a recurrence names its own half
instead of needing a stopwatch at the end. See `docs/gui_thread_audit.md` §8.

The close path also exercises the cancellation entry points end to end: the window
closing runs `AppBehaviors.cleanExit()`, which abandons the queued READS
(`ShowService.cancelDocumentJobs(["open", "templateLoad"])`) and leaves a save the
operator asked for to finish its write — the two halves of `bps::JobTicket`
(`Abandon` vs `WithdrawVerdict`), whose semantics the `job-cancel` unit suite pins
down (`./build/bps_unit_tests job-cancel`).

## GO LIVE latency: the first frame, and the live frame rate as the load grows

`run_live_latency_test.sh` covers the two numbers behind every "going live takes a
few seconds, and it gets worse as the show grows" report — and it covers them as a
TREND: the app drives a ladder of live sessions, each bigger than the last, and
measures each one itself.

```bash
bash tools/selftest/run_live_latency_test.sh          # the default ladder
LIVELAT_MAX_FIRST_FRAME_MS=1200 bash tools/selftest/run_live_latency_test.sh
```

```
== the ladder the app drove ==
[SELFTEST-LIVELAT] step=plan rungs=4 driveMs=150 windowMs=8000
[SELFTEST-LIVELAT] step=cycle n=1 slides=2 blocks=4 outputs=2 driveMs=150 windowMs=8000 goLiveCall=74ms live=true
[SELFTEST-LIVELAT] step=firstframe n=1 firstFrameMs=1455
[SELFTEST-LIVELAT] step=mode n=1 mode=overlay overlay='clock'
[SELFTEST-LIVELAT] step=measured n=1 slides=2 blocks=4 outputs=2 firstFrameMs=1455 frames=10 changes=0 windowMs=8433 fps=1.19 live=true mode=overlay
...
== per-rung measurement (the app's own counters) ==
rung  slides  blocks  outputs  firstFrameMs  frames/reps  windowMs  fps       mode      comment
1     2       4       2        1455          10/0        8433      1.19      overlay
2     8       24      2        429           11/0        8611      1.28      overlay
3     20      100     2        331           10/0        8527      1.17      overlay
4     20      100     4        378           6/0         8796      0.68      overlay
LIVELAT: PASS rungs=4 worstFirstFrame=1455ms minFps=0.68 maxFps=1.28 drop=46.9% budget=2000ms/0.5fps/60%
LIVELAT: FAIL launch | FAIL missing | FAIL cycles | FAIL firstframe
        | FAIL nomove | FAIL fps | FAIL fpsdrop | FAIL roster
```

**The app measures, the script judges — and neither number can come from the log.**
The log's own frame line (`output frame sanity`) is written when the presented state
CHANGES, not per frame, and the live loop only rasterizes when the on-air scene
changes at all (change-driven rendering with a 1Hz heartbeat for a static scene): a
static session has no frame rate to degrade, and counting log lines would count state
changes. So the scenario reads the loop's two publish points instead —
`LiveOutputService.previewFramesNow()` (the main pass: the first thing the operator
can see, which is what the first frame is timed on) and `framesSentNow()` (the whole
iteration, once every per-output pass has landed, which is what the cadence is
counted on).

**The window is loaded on purpose, and the app says which way.** The first version of
this scenario advanced the on-air slide every 150 ms and called the result a frame
rate. It was not: MEASURED, the loop delivered the same 7-8 frames per 8 s window at a
150 ms beat and at a 3 s beat, i.e. the 1Hz heartbeat and not its own throughput — an
average of the policy, insensitive to the load the harness exists to test. The window
now takes an **overlay on air** first: the change-driven skip and the heartbeat are
both bypassed while the compositor has live content
(`LiveOutputController::RenderOnce`), so the loop rasterizes every iteration it can
and the delivered rate IS its throughput. `mode=` travels with every row because the
fallback (`slides` — the overlay library was empty, so the scenario advances the
on-air slide far faster than the loop can serve) does not mean quite the same thing.
Either mode is a state an operator can really be in; nothing synthetic is taken on
air, and the overlay is released before the session stops.

| line | what it proves |
|---|---|
| `plan` | how many rungs this run will drive, and the beat — the harness reads the count from here rather than parsing its own env string |
| `cycle` | the rung's document is built and GO LIVE was asked for; `goLiveCall=` is what the CALL cost the interface thread (a different thing from the first frame, which lands later, on the loop) |
| `firstframe` | the loop's own publish moved: something is on the monitor |
| `mode` | which load was applied to the window, and the overlay it took when it took one |
| `measured` | this rung's numbers, `live=true`, and its mode — a rung measured after the live session had already dropped reads as a pass otherwise |
| `noframe` | the rung waited out its whole deadline without a frame, and it is still reported as `measured` (`firstFrameMs=-1`): never going live is the worst case of the number this harness polices, so it must not be a missing row |
| `restored` | the output roster the ladder grew is back to what the operator had |
| `done` | every rung ran |

The ladder's default is `2:2:1`, `8:3:1`, `20:5:1`, `20:5:4` — SLIDES, LAYERS PER
SLIDE and ENABLED OUTPUTS: the three things the loop pays for per frame (`SceneBuilder`
composites every block; the loop renders one extra pass per enabled, styled roster
row). The last rung holds the content still and grows only the outputs, so a cost that
arrives with outputs is not read as a cost of content. `VGR_LIVELAT_LADDER`,
`VGR_LIVELAT_DRIVE_MS` and `VGR_LIVELAT_WINDOW_MS` override the ladder, the beat and
the window; a rung asking for one slide is skipped with a line saying so (the slides
mode has nothing to advance on it).

**What the load actually costs here (one run, this machine).** The last rung's two
extra outputs took the cadence from 1.28 fps to 0.68 — a 47% throughput loss for two
more 1080p passes per frame, and the load-shape half of the verdict is built on
numbers like that. Slides and blocks, at 2 → 8 → 20 slides and 4 → 100 blocks, moved
the same number by nothing measurable (1.19 → 1.28 → 1.17, i.e. run-to-run noise): on
this machine the iteration is dominated by the fixed per-output raster cost, not by
the scene's block count. That is the harness reporting what is there rather than a
target it wants to see, and it is why the outputs rung exists at all.

**The roster is the operator's, not the harness's.** A rung asking for more enabled
screens than the roster has DUPLICATES a styled row, and the run names what it adds
so it can remove exactly those before it quits (`OutputListModel::saveRoster`
persists, so a selftest must not leave junk outputs behind). Duplicating rather than
adding, because the engine only renders a per-output pass for an ENABLED, STYLED row
— a bare added row measured as "outputs are free" in the first version of this
scenario, which is exactly the kind of thing a load harness must not get wrong.
`FAIL roster` fires if the `restored` line never lands.

The budgets are a regression gate set from the measured runs, not a target — three
runs on this machine, and the gate has to survive the spread they show. The worst
first frame may not exceed 2000 ms (measured 998–1455, and the worst rung is always
the FIRST: a cold first raster, which is also why it varies most), the slowest rung
may not fall under 0.5 fps (measured 0.68–0.81), and no rung may sit more than 60%
under the fastest (measured 44–47%, which the outputs rung spends by design).

## A colour picked for a shape: does it reach the ITEM, the CANVAS and the PIXELS?

`run_item_color_test.sh` covers "the opacity/colour in the picker shows well but it
never applies to the shape — the text item does take it", and it reports three
witnesses per item, because the first two can agree while the screen still shows
nothing: the ITEM's own value, its DELEGATE's own read, and the PIXELS the delegate
actually paints (each solid apply is grabbed to a PNG and sampled).

Both shape render paths are measured — a RECTANGLE (the delegate's own fill box) and
a CIRCLE (`drawsOwnBody`, where `shapeContent` paints the body from the same style) —
and the text item's colour runs in the same launch as the known-good control.

```bash
bash tools/selftest/run_item_color_test.sh
```

```
[SELFTEST-ITEMCOLOR] row key=item-1 selected=["item-1"] styleNull=false primaryKind=shape rowTarget=itemBackground hint=itemBackground
[SELFTEST-ITEMCOLOR] apply title=Background Color target=itemBackground
[SELFTEST-ITEMCOLOR] rect solid key=item-1 before=#3a3f55 item=#ffffff itemA=1.000 delegate=#ffffff delegateA=1.000
[SELFTEST-ITEMCOLOR] rect faded item=#4dffffff itemA=0.302 delegate=#4dffffff delegateA=0.302
...
[SELFTEST-ITEMCOLOR] cycle click1 key=item-3 selected=["item-3"]
[SELFTEST-ITEMCOLOR] cycle click2 key=item-3 selected=["item-2"]
[SELFTEST-ITEMCOLOR] summary rectPropsOk=true circlePropsOk=true cycleOk=true
ITEMCOLOR: APPLIED
```

The picker is opened through the **Background row's own expression**
(`itemBackground` when something is selected), not a target the scenario hardcodes: a
hardcoded target would keep passing while the row itself decided otherwise for a
selected shape — which is the reported "the shape's colour goes somewhere else".

The `cycle` pair covers the other half of that report, "you pick text item instead of
the shape": `(100,90)` lies inside the circle's box AND inside the text item's box
above it, so Qt hands the tap to the text item. A second tap on the same point
(350–900 ms later, which is why one tick of the scenario's 700 ms timer separates
them) must step down to the shape — EditScreen's click-through. A *rapid* second
click is deliberately not a click-through: that is the text item's edit gesture.

Verdict codes: `APPLIED`, `FAIL missing` (the scenario never reached its verdict),
`FAIL select` (the click-through still lands on the text item), `FAIL item` (the item
never received the colour — a missing value counts as missing, never as "faded"),
`FAIL text` (the known-good control broke too, so it is not shape-specific), and
`FAIL canvas` (the item and its delegate hold the colour, the pixels do not).

## The live video path under CPU load: frames and age, per output

The report: *"when I'm showing HDMI and the laptop is stressed the video lags badly"*.
That is a claim about a NUMBER, so `run_videoperf_test.sh` measures it:

```bash
bash tools/selftest/run_videoperf_test.sh            # the shipped path, under load
bash tools/selftest/run_videoperf_test.sh --compare  # legacy behaviour, then shipped
VIDEOPERF_HOGS=8 bash tools/selftest/run_videoperf_test.sh   # heavier stress
```

The app's own scenario (`VGR_VIDEOPERF_TEST=1`) puts the first camera/HDMI input on
air, goes LIVE with it, and samples for ten seconds what the engine already publishes
per output — delivered frames and the age of the last presented frame — plus the live
loop's throughput and the pump's frame revisions (`inputRev`: one bump per frame the
pump actually took, i.e. the capture path's own rate, independent of rendering). The
runner launches it with N busy processes, so the stressed machine of the report is the
machine being measured.

**Before/after on the same machine.** The engine keeps two measurement hooks — not
settings — that restore the pre-fix behaviour for one run:

| env | restores |
|---|---|
| `BPS_LAYER_REFRESH_ALWAYS=1` | every live layer re-copied and re-uploaded on every build, every output |
| `BPS_TAP_COPY_UNDER_LOCK=1` | the PAL pixel pull copies under the tap's mutex (capture waiting on consumers) |

Measured 2026-10-24, Integrated Webcam on air, 2 enabled outputs, 10 s window, this
build's default **software** renderer. Three `--compare` runs plus one idle reference:

| load | leg | capture (`input`) | slowest output | mean last-frame age |
|---|---|---|---|---|
| 4 cores | legacy | 12.8 fps | 0.21 fps | 1474 ms |
| 4 cores | shipped | 24.6 fps | 0.42 fps | 1205 ms |
| 4 cores | legacy | 11.8 fps | 0.32 fps | 2060 ms |
| 4 cores | shipped | 10.2 fps | 0.21 fps | 2018 ms |
| 12 cores | legacy | 2.4 fps | 0.17 fps | 2009 ms |
| 12 cores | shipped | 3.8 fps | 0.10 fps | 2070 ms |
| idle | legacy | 14.3 fps | 0.33 fps | 1992 ms |
| idle | shipped | 14.9 fps | 0.30 fps | 1881 ms |

**What this actually says** — and it is not a win to report proudly: the run-to-run
spread on this machine is larger than the effect, so the before/after delta is **not
measurable here**. The reason is visible in the `loop` number: this build rasterizes
in software, one live iteration is a 1920×1080 CPU raster per output, and the loop
manages 0.2–0.7 fps whether or not the copies happen. At that cadence the render
thread pulls the tap twice in ten seconds and the GUI pump's copies are ~90 MB/s of a
very idle budget — the tap's mutex is simply not contended, so removing the contention
cannot show up.

So the copy/lock work is a correctness-and-headroom change, not a measured speed-up on
this box. What the measurement DOES establish is the shape of the user's lag on this
machine: the video path is not the bottleneck — the software rasterizer drawing every
output is, and a machine under load stretches that from "tens of ms" to seconds. The
sensitive configuration for re-running this is one where the loop can actually run
(30–60 fps): hardware rendering on (Settings · General), or fewer/smaller outputs.

Budgets are floors for "the feed is alive and reachable" (`VIDEOPERF_MIN_INPUT_FPS=8`,
`VIDEOPERF_MIN_FPS=0.15`, `VIDEOPERF_MAX_AGE_MS=3000`); the printed numbers, and the
`--compare` spread, are the point.
