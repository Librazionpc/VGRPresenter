// tools/ndi_golive_probe.qml — NOT a build input.
//
// The GO LIVE + NDI crash repro, kept as a document rather than as a live
// Timer block inside qml/Main.qml. The scenario that actually shipped in
// Main.qml is the `selfTestNdiGoLive*` Timers; this file is the same logic
// written out on its own so a future reader can see the intent without
// reading around the other self-test stages.
//
// HOW IT RUNS: Main.qml instantiates these Timers only when
// `typeof SelfTest !== "undefined"` (i.e. the app was built with
// -DVGR_ENABLE_SELFTEST=ON, the default) AND VGR_SELFTEST=1 is set in the
// environment. Without the env var the driver is inert and these Timers
// never fire.
//
// WHY IT EXISTS: the 2026-10-01 crash (five byte-identical records reading
// "filesystem error: cannot make absolute path: Invalid argument []") was
// reproduced by: enabling the NDI plugin feature, then pressing GO LIVE with
// an NDI output row enabled. The fix is in
// src/PresentationEngine/platform/common/Filesystem.cpp (Absolute/Metadata
// now answer an empty path instead of erroring). This scenario is the
// regression check: it must run to completion WITHOUT a new
// crash.log.<epoch>.log appearing in
//   %AppData%\VGR\VGRPresenter\crashes\
// and without a new .dmp in %LocalAppData%\CrashDumps.
//
// READING THE RESULT: every stage logs a [SELFTEST-NDI] line. Run with
// stderr redirected to a file (tools/ndi_golive_probe.sh does this) and
// check the last line is "survived".

import QtQuick

QtObject {
    // Stage 1 — open Settings on the Plugins tab (where the NDI feature
    // toggle lives) and confirm the toggle is reachable.
    // Stage 2 — enable NDI, which pushes the gate into the engine's
    // BroadcastEngine and re-runs device discovery.
    // Stage 3 — GO LIVE. This is the step that used to kill the process.
    // Stage 4 — hold live for ~8 s so the NDI send clock (30 fps default)
    // gets many ticks through the worker thread and the per-output render
    // pass; a crash on the Nth frame would show up here, not at GO LIVE.
    // Stage 5 — stop, then quit cleanly.
    readonly property var stages: [
        "open Settings > Plugins",
        "enable the NDI feature toggle",
        "LiveOutputService.goLive()",
        "hold live ~8s (NDI send clock ticking)",
        "LiveOutputService.stop() and quit",
    ]
}
