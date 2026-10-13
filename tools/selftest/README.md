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
#     BUSLIGHT: FAIL launch | FAIL missing | FAIL norouted | FAIL status
#                | FAIL noping | FAIL pinglabel | FAIL ghostalive | FAIL wrongkind
```

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
