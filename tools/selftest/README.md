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
- The `selfTestSettingsShell` objectName on the Settings ModalShell.

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
