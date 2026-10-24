import QtQuick
import QtQuick.Controls
import VGRPresenterUI
// The app's one keyboard binding type (see qml/shortcuts/Shortcut.qml). Imported
// AFTER QtQuick so `Shortcut {}` here means the app's action-named binding; it
// is a drop-in for QtQuick's (sequence/enabled/activated), so the bindings below
// read `action: "…"` and the chord itself lives in qml/settings/Hotkeys.qml.
import "shortcuts"
import "components"
import "screens"
import "screens/main"
import "screens/edit"

ApplicationWindow {
    id: window
    // ---- the desktop this app opens on and scales to -------------------------
    // The panel size comes from the ENGINE's platform layer (EngineBridge.
    // primaryDisplay, filled by the PAL's IMonitor at boot and refreshed on every
    // monitor hot-plug), not from Qt's Screen: Qt reports the DPI-SCALED logical
    // size, so on a scaled display it does not say what panel the user actually
    // has. Qt still supplies the WORK AREA (the screen minus the taskbar, which
    // the engine does not report) and is the fallback before the engine has
    // enumerated a display.
    //
    // Everything here is in the layout pixels Window.width/height use: the
    // engine's real pixels divided by Qt's devicePixelRatio (Screen geometry is
    // already in that unit).
    readonly property real pixelRatio: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
    readonly property var engineDisplay: EngineBridge.primaryDisplay
    readonly property real realDisplayWidth: (engineDisplay && engineDisplay.widthPx > 0)
                                             ? engineDisplay.widthPx / pixelRatio : 0
    readonly property real realDisplayHeight: (engineDisplay && engineDisplay.heightPx > 0)
                                              ? engineDisplay.heightPx / pixelRatio : 0
    readonly property real desktopWidth: realDisplayWidth > 0 ? realDisplayWidth
                                          : (Screen.width > 0 ? Screen.width : 1440)
    readonly property real desktopHeight: realDisplayHeight > 0 ? realDisplayHeight
                                           : (Screen.height > 0 ? Screen.height : 900)
    // What the window may fill: the work area (taskbar-aware, so no edge of this
    // frameless window lands under the taskbar), capped by the real display — a
    // logical geometry larger than the panel would otherwise open a window whose
    // own edges sit off the screen.
    readonly property real availableScreenWidth: Math.min(window.desktopWidth,
                                                          Screen.desktopAvailableWidth > 0 ? Screen.desktopAvailableWidth : window.desktopWidth)
    readonly property real availableScreenHeight: Math.min(window.desktopHeight,
                                                           Screen.desktopAvailableHeight > 0 ? Screen.desktopAvailableHeight : window.desktopHeight)
    // The app OPENS filling the desktop (minus a small margin), not at its
    // 1440x900 design size: `uiScale` below then follows the window it opened in,
    // so the whole app is drawn scaled to the screen it started on — up on a large
    // display, down on a small one. The design size is the layout's UNIT, never
    // the size the app opens at.
    width: Math.max(Math.min(820, availableScreenWidth), availableScreenWidth - 32)
    height: Math.max(Math.min(560, availableScreenHeight), availableScreenHeight - 64)
    minimumWidth: Math.min(820, availableScreenWidth)
    minimumHeight: Math.min(560, availableScreenHeight)
    // Keep manual/native resizing inside the active display. The frameless
    // window can otherwise be dragged larger than a lower-resolution screen,
    // leaving the right-side controls and library search outside the visible
    // area. Qt reports Screen geometry in the same device-independent units
    // used by Window.width/height, including when DPI scaling is enabled.
    maximumWidth: Math.max(minimumWidth, availableScreenWidth)
    maximumHeight: Math.max(minimumHeight, availableScreenHeight)

    // ---- ONE app scale, enforced on every layer -------------------------------
    // The app was laid out at 1440x900: every coordinate in it (screens, header,
    // menus, drag chip, dialogs) assumes that space. Rather than reflow each
    // surface into whatever room a smaller window leaves — which is what put
    // icons on top of each other in General and Audio & Video — the whole UI keeps
    // its design size and is scaled uniformly.
    //
    // `uiScale` is the single source of that scale. EVERY window-level layer reads
    // it (screens, header, menu bar, drag layer, the Settings modal, the app-level
    // dialogs, and the components that reparent to the window overlay); anything
    // nested inside those layers inherits their transform and must NOT apply the
    // scale again. tools/selftest/run_modal_fit_test.sh asserts that every layer
    // reports the same scale, so a new layer that forgets it is caught.
    //
    // It does scale UP: the app opens filling the desktop (see above), so on a
    // display bigger than the design the whole UI is drawn larger instead of
    // leaving a strip of empty window around a 1440x900 island. Every layer is
    // scaled by the same factor, so the design's proportions are preserved.
    //
    // Modals follow the same rule with one extra part: the CARD is scaled and the
    // SCRIM behind it never is (AppScale.qml holds both the number and the
    // helper) — a scaled scrim shrinks away from the window's edges, and the
    // dialog loses the full-window black box that makes it read as modal.
    //
    // Two window-level items deliberately stay OUT of it, each for a reason:
    // WindowResizeHandles (it IS the window's frame — scaling it would move the
    // resize borders inward) and AppCursorCatcher (its whole point is to force a
    // cursor shape from outside any transform).
    readonly property real designWidth: 1440
    readonly property real designHeight: 900
    readonly property real uiScale: Math.min(window.width / designWidth, window.height / designHeight)
    // The DESIGN CANVAS the window-level layers are drawn in: the window itself,
    // expressed in design units (`window / uiScale`). Because uiScale is the
    // SMALLER of the two ratios, each axis of the canvas is at least its design
    // size — 1440 wide, 864 tall below the header (the design height minus the
    // 48px the screens keep under the strip) — so the layout is never squeezed
    // below what it was built for, while the scaled canvas still lands exactly on
    // the window's own edges. A canvas pinned at the design size instead left the
    // bottom library dock hovering ~26px above the window at 820x560 — "the
    // project and bottom sidebar are not attached to the window".
    //
    // The screens sit BELOW the header strip (topMargin headerExtra), so it is the
    // room UNDER that strip that the canvas has to match vertically.
    readonly property real canvasWidth: window.width / (uiScale || 1)
    readonly property real canvasHeight: (window.height - window.headerExtra) / (uiScale || 1)
    // A screen's own top 48px is designed to sit UNDER the header strip, so the
    // strip's scaled height (design height x uiScale) is what pushes a screen down:
    // headerExtra is that height minus the 48px the screen already accounts for.
    // In design units (what the layout math below uses), the screens are offset by
    // the strip's design height minus 48.
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    // (The taskbar/alt-tab icon is set from C++ — main.cpp grabs this window
    // after load and calls QWindow::setIcon(). A QQuickWindow does NOT
    // reliably adopt QGuiApplication::setWindowIcon() on Windows, and there
    // is no Window.icon property in the QML API to do it from here — the
    // exe's own icon (RC_ICONS) is not what the RUNNING taskbar button shows.
    // This window is frameless, so the taskbar button is its only OS chrome.)
    // The open show's name, with a dot while it has unsaved changes.
    title: (ShowService.hasShow ? ShowService.showName + (ShowService.showDirty ? " •" : "") + " — " : "") + "VGRPresenter"
    color: Theme.windowBg

    // The devices this app is SET UP with, gathered from the three rosters that
    // own them. Anything a device picker can reach is deliberately NOT in here:
    // those pickers ask for the full machine themselves when they open.
    function requestBootDevices() {
        const audio = []
        for (let i = 0; i < AudioInputListModel.rowCount(); ++i) {
            const row = AudioInputListModel.getInput(i)
            if (row && row.kind === "device" && String(row.sublabel || "").trim() !== "")
                audio.push(row.sublabel)
        }
        const video = []
        const screens = []
        for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
            const row = VideoSourceListModel.getSource(i)
            const label = String(row && row.sublabel ? row.sublabel : "").trim()
            if (label === "")
                continue
            // "screen" rows name a monitor or a captured window (both live in
            // the screen roster); camera/NDI rows name a capture device.
            if (String(row.kind || "") === "screen") screens.push(label)
            else video.push(label)
        }
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const row = OutputListModel.getOutput(i)
            const screen = String(row && row.screenName ? row.screenName : "").trim()
            if (screen !== "") screens.push(screen)
        }
        EngineBridge.requestUsedDevices(audio, video, screens)
    }

    // ---- The engine boot report (see Component.onCompleted) ------------------
    // One notice per launch, and only once the device rosters are actually in:
    // the summary is a pull, and the enumeration that fills it lands asynchronously.
    // The gate is "the rosters have something in them", NOT "a devicesChanged
    // arrived" — the signal also fires for refreshes that carry no new roster
    // (and one of those lands before the async sweep does), and latching on it
    // printed exactly what this is here to fix: "· 0 audio device(s):". Every
    // later enumeration calls back and is a no-op once the toast has gone out
    // (the flag), so a hot-plug still can't re-announce boot.
    property bool bootReportShown: false
    function reportBoot(force) {
        if (window.bootReportShown)
            return
        // The gate is the BRIDGE's word that the sweep which carried this app's
        // own device set has landed (bootSweepDone) — the engine enumerates
        // once while booting, before QML has said which hardware this setup
        // uses, and a summary read off THAT says "0 audio device(s)". The old
        // "the rosters have something in them" test passed on it as soon as the
        // primary display turned up, which is exactly how the startup toast
        // ended up reporting zero devices for a machine full of them.
        if (!EngineBridge.bootSweepDone && !force)
            return
        const haveRosters = EngineBridge.audioDevices.length > 0
                            || EngineBridge.screenDevices.length > 0
                            || EngineBridge.videoDevices.length > 0
        if (!haveRosters && !force)
            return
        const boot = EngineBridge.bootSummary()
        if (boot === "" && !force)
            return
        window.bootReportShown = true
        if (boot !== "") {
            // Also one line in the engine log: the toast is gone in a few
            // seconds, and "the engine found no audio devices" is exactly the
            // kind of claim that has to stay answerable afterwards.
            EngineBridge.log("info", "Engine", boot)
            // Its crux is WHICH hardware is in use: those names are what the eye
            // needs to catch in a five-second toast (the rest of the sentence is
            // context), so they are handed over to be drawn bold.
            EventBus.notify(boot, "success", qsTr("Engine"), "engine.boot",
                            EngineBridge.bootInUseDevices())
        }
    }
    // ---- A recovered crash, and whether this machine could report one -------
    // "Recovered from an unexpected shutdown last run — details were written to
    // the crash log" is only half the story on a machine where the tooling that
    // writes those details cannot run: the operator is being sent to a file with
    // nothing in it, and nobody ever finds that out, because a crash that leaves
    // no trace leaves nothing to find. So the startup forensics probe's verdict
    // (CrashHandler.h's VerifyForensicsTooling, published through EngineBridge's
    // crashTooling* properties) is delivered as a note beside the report it
    // qualifies.
    //
    // The probe runs on a WORKER — it STARTS the debugger a real capture would
    // attach and waits for it to answer — so its verdict is never in by the time
    // the recovery notice goes out. Both orders are real, and both end in the
    // same single note:
    //   * verdict already known broken → noteCrashTooling() right after the
    //     recovery notice below;
    //   * verdict lands later (the usual one) → the Connections handler fires it
    //     when it does.
    // A session that recovered nothing says nothing: tooling that cannot capture
    // a crash is a fact for Settings · General · Crash reports, not a warning to
    // interrupt a launch with.
    property bool crashRecoveryReported: false
    property bool crashToolingNoted: false
    // The note's text, in one place: the notice that carries it and the self-test
    // that checks it was seen read the same string.
    function crashToolingNoteText() {
        return qsTr("Note: the tools that capture a crash on this machine are not working, so the report from that shutdown may be incomplete — %1")
            .arg(EngineBridge.crashToolingProblem)
    }
    function noteCrashTooling() {
        if (window.crashToolingNoted || !window.crashRecoveryReported)
            return
        if (EngineBridge.crashToolingState !== "incomplete")
            return
        window.crashToolingNoted = true
        // "error", the level its sibling above carries — deliberately. A user who
        // turned notices off (only errors interrupt then) is told their previous
        // run crashed; leaving the note at warning level would drop the half of
        // that story that says the report has nothing in it.
        EventBus.notify(window.crashToolingNoteText(), "error", qsTr("Stability"), "ui.crash.tooling")
    }
    Connections {
        target: EngineBridge
        function onCrashToolingChanged() { window.noteCrashTooling() }
    }

    // ---- Startup-forensics verdict self-test (VGR_FORENSICS_TEST=1) ----------
    // The two surfaces that verdict reaches, read back as the user meets them:
    //   * the Settings · General "Crash reports" row — the pill's own text and
    //     the detail LINE as drawn (looked up in the item tree, so a row that
    //     binds the wrong property fails), and
    //   * the note that rides a recovered crash — delivered at all, and present
    //     in the notification centre, which is where the user reads it.
    // The runner forces a broken tooling layout (VGR_FORENSICS_DIR at a folder
    // with an empty analyze.bat beside it) and seeds one crash record, so both
    // halves run the real probe and the real recovery path; state=/problem=/row=
    // are printed as facts for the script to judge.
    // Armed only with BOTH VGR_SELFTEST=1 and VGR_FORENSICS_TEST=1: a plain
    // VGR_SELFTEST=1 run must never open Settings or quit early.
    property int forensicsTries: 0
    property int forensicsNoteTries: 0
    property bool forensicsNoteSeen: false
    Timer {
        id: selfTestForensicsPoll
        running: typeof SelfTestForensics !== "undefined" && SelfTestForensics
        interval: 700
        repeat: true
        onTriggered: {
            // A notice is on screen for a few seconds and then DISMISSED, so the
            // note has to be caught while it is in the centre — the same place,
            // and the same window of time, the user has to catch it in.
            const note = window.crashToolingNoteText()
            const items = NotificationCenter.items
            for (let i = 0; i < items.length; ++i)
                if (items[i].message === note)
                    window.forensicsNoteSeen = true
            // The probe STARTS a debugger on a worker, so the verdict is not in
            // by the first frame: wait for it to stop saying "checking" (bounded
            // — the probe itself is bounded).
            if (EngineBridge.crashToolingState === "checking" && window.forensicsTries++ < 20)
                return
            // The note is delivered with the verdict, so give it a beat to be in
            // the centre. It is only ever delivered in a session that recovered
            // a crash, so a broken verdict without one waits this beat out and
            // reports noted=false — which is the truth the script reads.
            if (EngineBridge.crashToolingState === "incomplete"
                && !window.forensicsNoteSeen && window.forensicsNoteTries++ < 6)
                return
            selfTestForensicsPoll.stop()
            window.settingsSection = "general"
            settingsScrim.visible = true
            selfTestForensicsRead.restart()
        }
    }
    Timer {
        id: selfTestForensicsRead
        interval: 1200          // let the General screen lay out and bind
        onTriggered: {
            const pill = SelfTest.findItem("selfTestCrashToolingPill")
            const line = SelfTest.findItem("selfTestCrashToolingDetail")
            console.log("[SELFTEST-FORENSICS] state=" + EngineBridge.crashToolingState
                        + " problem=" + JSON.stringify(String(EngineBridge.crashToolingProblem))
                        + " pill=" + (pill ? JSON.stringify(String(pill.text)) : "MISSING")
                        + " detail=" + (line ? JSON.stringify(String(line.text)) : "MISSING")
                        + " recovered=" + window.crashRecoveryReported
                        + " noted=" + window.crashToolingNoted
                        + " inToast=" + window.forensicsNoteSeen)
            // ...and the card itself for the human: the row is the deliverable,
            // and a passing run should leave something to look at rather than a
            // line of text about it.
            // Named after the state it drew, so both runs' pictures survive
            // (the healthy case would otherwise overwrite the broken one).
            SelfTest.grab("selfTestCardStability", "forensics_card_" + EngineBridge.crashToolingState + ".png")
            // Let the grab's saveToFile() land before the process goes away.
            selfTestForensicsDone.restart()
        }
    }
    Timer {
        id: selfTestForensicsDone
        interval: 1200
        onTriggered: SelfTest.quit()
    }

    // ---- The missing-device notice (the other half of the boot sweep) -------
    // A device this setup was remembered with that is NOT on the machine this
    // time is worth saying out loud: the row that names it meters nothing /
    // previews black, and "why is that input dead?" is not something the user
    // should have to open Settings to answer.
    //
    // It is published here rather than by the engine bridge for one reason:
    // ORDER. The bridge knows a device is missing at the moment its sweep
    // completes, which is the same moment the boot toast goes out — and a toast
    // published before that one would sit ABOVE it in the overlay (toasts stack
    // oldest first). So the bridge leaves the answer on missingUsedDevices()
    // and this runs immediately AFTER reportBoot(), which is what "after the
    // engine boot toast" means on screen. One notice per launch (the flag).
    // The rosters that own the devices the boot sweep is asked for. Any change
    // in them re-states the used set (see requestBootDevices).
    Connections {
        target: AudioInputListModel
        function onRowsInserted() { window.requestBootDevices() }
        function onRowsRemoved() { window.requestBootDevices() }
        function onModelReset() { window.requestBootDevices() }
        function onDataChanged() { window.requestBootDevices() }
    }
    Connections {
        target: VideoSourceListModel
        function onRowsInserted() { window.requestBootDevices() }
        function onRowsRemoved() { window.requestBootDevices() }
        function onModelReset() { window.requestBootDevices() }
        function onDataChanged() { window.requestBootDevices() }
    }
    Connections {
        target: OutputListModel
        function onRowsInserted() { window.requestBootDevices() }
        function onRowsRemoved() { window.requestBootDevices() }
        function onModelReset() { window.requestBootDevices() }
        function onDataChanged() { window.requestBootDevices() }
    }

    property bool bootMissingNoticeShown: false
    function reportMissingDevices() {
        if (window.bootMissingNoticeShown)
            return
        // Same wait as the boot report: the check runs with the used sweep, so
        // an empty list before it lands means "not asked yet", not "all here".
        if (!EngineBridge.bootSweepDone)
            return
        const missing = EngineBridge.missingUsedDevices()
        if (missing.length === 0)
            return
        window.bootMissingNoticeShown = true
        const shown = missing.slice(0, 3)
        let text = qsTr("Not connected this time: %1").arg(shown.join(", "))
        if (missing.length > shown.length)
            text += qsTr(" and %1 more").arg(missing.length - shown.length)
        text += qsTr(" — the inputs that use them will read no signal until they are back (or re-pick a source in Settings · Audio & Video).")
        // Same reason as the boot summary: the toast is gone in seconds, and
        // "which device did it say was missing?" has to stay answerable.
        EngineBridge.log("warning", "Devices", text)
        // The names ARE the notice ("which device went missing?"): bold, so the
        // operator reads them without scanning the sentence around them.
        EventBus.notify(text, "warning", qsTr("Devices"), "engine.devices.missing", shown)
    }
    Connections {
        target: EngineBridge
        function onDevicesChanged() {
            window.reportBoot(false)
            window.reportMissingDevices()
        }
    }
    Timer {
        id: bootReportFallback
        // The safety net: if the sweep never lands (no platform backend, an
        // enumeration that fails), the toast still reports what the engine
        // knows instead of staying silent forever.
        interval: 6000
        onTriggered: {
            window.reportBoot(true)
            window.reportMissingDevices()
        }
    }

    property string settingsSection: "general"
    // The Hotkeys scope Settings pushes while it is open (Esc closes it) — see
    // the settingsScrim's onVisibleChanged and qml/settings/Hotkeys.qml.
    property int settingsHotkeyScope: 0
    // The header is now two rows — the title bar (logo, menu, window buttons), a
    // separator line, then the Show / Edit / Stage tabs. The screens were designed
    // around a 48 px header, so they are pushed down by however much taller it is
    // (their own top 48 px sits underneath the header, hidden).
    // The header strip is chrome BUT it is part of the same design: it was laid
    // out at 1440 too (logo, menu, centred tabs, gear, GO LIVE), so it is scaled
    // with the screens rather than left to crowd. The screens' offset and the
    // offset the app pushes them by are therefore both in SCALED units: a screen's
    // own top 48px must sit under the scaled header, which is what makes the two
    // line up at any scale (at scale 1 the numbers are exactly what they were).
    readonly property int headerHeight: Math.round(appHeader.height * window.uiScale)
    readonly property int headerExtra: Math.round((appHeader.height - 48) * window.uiScale)
    // "show" | "edit" | "stage" — Stage has no screen yet, so its tab click
    // is accepted but doesn't switch the view.
    property string currentView: "show"
    // The window-level layer that carries a drag between panes (DragSource / DropArea); see DragLayer.qml.
    readonly property var dragLayer: dragLayerItem

    // Previous-run crash report, delivered on the standard notification
    // pipeline the moment QML is alive. (See main.cpp: the CrashHandler can
    // only leave a log file from inside a dying process — publishing to the
    // bus it may have crashed is unsafe by design — so this is the UI half
    // of that contract. Rotation in ConsumePendingCrashSummary makes it
    // exactly-once per crash.)
    Component.onCompleted: {
        // Set the saved Outputs-page media-fit default before any take can
        // fall back to the service's startup default.
        LiveOutputService.setDefaultMediaFit(
            String(SettingsService.value("outputs.defaultMediaFit") ?? "contain"))
        // THE STYLE ROSTER HAND-OVER, first thing: QML constructs its own
        // StyleListModel (the engine does not call that class's static create()
        // factory — see OutputListModel::adoptStyleRoster), so the object the
        // Styles screen edits and the one every C++ reader resolves styles
        // through are TWO rosters unless they are joined. Joined here, the
        // tiles, the output windows and the style spec pushed into the engine
        // all follow the live edit instead of the copy that only re-hydrates
        // from the store at boot.
        try {
            OutputListModel.adoptStyleRoster(StyleListModel)
            console.log("[SELFTEST-STYLEBG] style roster hand-over done")
        } catch (e) {
            console.log("[SELFTEST-STYLEBG] style roster hand-over FAILED: " + e)
        }
        // The app scale's one source (see AppScale.qml): a modal card cannot read
        // the scale off a scaled ancestor (its root sits above one), so it reads
        // it from the window registered here.
        AppScale.register(window)
        // One-shot layout truth for the log: which display the app sized itself
        // against, what the engine reported for it, and the scale that came out.
        // (A wrong/absent engine display shows up here instead of only as a
        // vaguely-too-small window.)
        EngineBridge.log("info", "Layout",
            "desktop " + Math.round(window.desktopWidth) + "x" + Math.round(window.desktopHeight)
            + " (engine " + (window.realDisplayWidth > 0
                             ? Math.round(window.realDisplayWidth) + "x" + Math.round(window.realDisplayHeight)
                             : "n/a") + " px-logical, dpr " + window.pixelRatio + ")"
            + ", window " + Math.round(window.width) + "x" + Math.round(window.height)
            + ", uiScale " + window.uiScale.toFixed(3))
        // Previous-run crash report — exactly-once (rotated aside at read).
        if (typeof pendingCrashSummary !== "undefined" && pendingCrashSummary !== "") {
            window.crashRecoveryReported = true
            EventBus.notify(
                qsTr("Recovered from an unexpected shutdown last run (%1). Details were written to the crash log.")
                    .arg(pendingCrashSummary),
                "error", qsTr("Stability"), "ui.crash.recovered")
            // ...and, if the machine cannot capture a crash, say so with it (see
            // noteCrashTooling): "details were written to the crash log" is not
            // true where nothing is.
            window.noteCrashTooling()
        }
        // Boot report with REAL platform facts. The device sweep is
        // ASYNCHRONOUS — the PAL's WASAPI / Media Foundation enumeration must not
        // block the first frame — so at this instant the rosters are still empty
        // and the summary reads "0 audio device(s):  · 0 display(s): ". That is
        // exactly what the startup toast kept printing on every launch. The
        // notice now goes out on the first enumeration instead (reportBoot),
        // with the fallback timer below covering a machine that genuinely has no
        // devices at all.
        if (EngineBridge.bootError.length > 0)
            EventBus.notify(EngineBridge.bootError, "error", qsTr("Engine failed to boot"), "engine.boot")
        else
            window.reportBoot(false)
        bootReportFallback.restart()

        // The engine's boot device sweep is limited to what this setup USES
        // (see requestUsedDevices): every audio input row's endpoint, every
        // video source's camera (and its window/monitor pick), and the display
        // each output is bound to. All of those are restored from the store by
        // now, so this is the moment the app can say which hardware it needs —
        // and the engine stops probing every endpoint, camera and window on the
        // machine to find it. A device picker opening later calls
        // refreshDevices()/refreshAudioDevices(), which enumerate the machine in
        // full again (that is where a NEW pick comes from), and a device that
        // was used last session but is gone now is announced by the engine.
        requestBootDevices()

        // ...and AGAIN whenever one of those rosters is restored or edited. The
        // models hydrate at their own pace (the AV board's video roster lands
        // after this window completes), so the call above legitimately knows
        // nothing yet on a cold start — the engine is told again as soon as
        // there is something to tell it, and a targeted sweep costs nothing to
        // re-run (a device nobody named is never touched).

        // Search: the Bible files are read into the engine (background thread) so verses
        // can be found.
        SearchService.loadBibles()
    }

    // Screens first (opaque, fill the window), then the shared header strip
    // and the menu layer on top — AppMenuBar must sit above AppHeader because
    // its logo/menu labels are drawn inside the title row of the same header.
    VGRPresenterMainScreen {
        id: showScreen
        objectName: "selfTestMainScreen"
        // The design canvas + the app's uniform scale (see uiScale above): sized
        // so the scaled screen still reaches the window's own edges.
        width: window.canvasWidth
        height: window.canvasHeight
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: window.headerExtra
        transformOrigin: Item.Top
        scale: window.uiScale
        visible: window.currentView === "show"
        // Esc on this screen: the “…” menu first (put it away), then the centre
        // page's Close — see the window's cancel binding above.
        onNewShowRequested: newShowDialog.open()
        // Row click in the shows library: open that .vgr (unsaved-changes
        // guarded) and go to the Edit screen.
        onOpenShowRequested: (path) => {
            editScreen.openShowPath(path)
            window.currentView = "edit"
        }
        // The app-wide search (the splash's "Quick search" button).
        onSearchRequested: quickSearch.openSearch()
        onImportRequested: importDialog.open = true
        // A card's Edit action in the Overlays / Templates tab: the Edit screen edits that overlay / template.
        // Unknown id (a deleted design, a stale pick): say so — the silent no-op here once made a broken
        // edit button look exactly like an unresponsive one.
        onDesignEditRequested: (kind, id) => {
            if (editScreen.openDesign(kind, id))
                window.currentView = "edit"
            else
                EventBus.notify(qsTr("That %1 no longer exists.").arg(kind), "warning", qsTr("Edit"))
        }
        // The Scripture tab's "Convert to show": a new show with the slides the engine built for the passage, opened on the Edit screen.
        // A Functions-pane action row's Run mark.
        onActionRunRequested: (actionId) => window.runActionById(actionId)
        onScriptureShowRequested: (name, slides) => {
            editScreen.session.closeDesign()
            editScreen.session.guardUnsaved(function () {
                ShowService.newShowDocument(name)
                slides.forEach((s) => ShowService.addSlide({ title: s.title, background: s.background, blocks: s.blocks }))
                editScreen.session.applyShow(ShowService.currentShow)
                window.currentView = "edit"
            })
        }
    }

    EditScreen {
        id: editScreen
        // Same design canvas + uniform scale as the Show screen above.
        width: window.canvasWidth
        height: window.canvasHeight
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: window.headerExtra
        transformOrigin: Item.Top
        scale: window.uiScale
        visible: window.currentView === "edit"
        // Done in the overlay / template editor: back to the library it came from.
        onDesignClosed: window.currentView = "show"
    }

    // What the Settings > General switches do to the running app: start-up (minimized, last show), autosave, backups,
    // crash recovery, close-to-tray. The engine keeps the settings; this carries them out.
    AppBehaviors {
        id: behaviors
        window: window
        editScreen: editScreen
    }

    // A design is only edited while the Edit screen is showing: leaving it (the Show tab) closes it, and the show that was
    // open comes back on the canvas.
    onCurrentViewChanged: {
        EngineBridge.log("info", "Navigation", "Screen: " + window.currentView)
        if (window.currentView !== "edit")
            editScreen.closeDesign()
    }
    // Switching the page while Settings is open is also "where you were" (see
    // rememberSettingsPage): the newest section is the one a resume comes back to.
    onSettingsSectionChanged: window.rememberSettingsPage()

    // Which page inside Settings the session is on, remembered for "Resume last
    // session" — and "" the moment the dialog closes, so a session that ended with
    // Settings shut can never come back into a dialog the user had already
    // dismissed. Written only after the app has STARTED (behaviors.started): the
    // store loads after this window is built, and a write at load time would
    // overwrite the remembered page with the empty value of a dialog nobody had
    // opened yet.
    function rememberSettingsPage() {
        if (!behaviors.started)
            return
        const at = settingsScrim.visible ? window.settingsSection : ""
        if (at !== String(SettingsService.values["session.lastSettingsSection"] ?? "")) {
            SettingsService.setValue("session.lastSettingsSection", at)
            // Logged like every other navigation step ("Screen: edit"): the page
            // a resume will come back to is exactly the kind of state that has to
            // stay answerable afterwards — "why did it open Settings · Outputs?"
            EngineBridge.log("info", "Navigation",
                             at === "" ? qsTr("Settings closed") : qsTr("Settings page: ") + at)
        }
    }

    // Opens the Settings dialog — the one entry point both the header's
    // gear button and the menu bar's Settings/Preferences items funnel
    // into, so a future keyboard shortcut (Ctrl+,) only ever calls this.
    function openSettings(section) {
        if (section !== undefined)
            window.settingsSection = section
        settingsScrim.visible = true
    }

    AppHeader {
        id: appHeader
        // The design canvas + the app scale (see uiScale): the header shrinks with
        // the screens instead of reflowing into them, and spans the window.
        width: window.canvasWidth
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        transformOrigin: Item.Top
        scale: window.uiScale
        activeTab: window.currentView
        // Edit opens a SHOW — one picked in the centre preview, or the one
        // already open in the editor. With neither, the tab is greyed and does
        // nothing: pressing it used to take the operator into an editor holding
        // an untitled show nobody asked for (and the recovery store then tried
        // to write it, which is where "cannot open for writing: …unsaved-show.vgr.tmp"
        // came from). A new show is made with New show, deliberately.
        // Truthy, not `!== null`: centerItem starts as undefined, and `undefined
        // !== null` is true — which then read `.type` off it and put a TypeError
        // in the log on the first evaluate.
        editAvailable: ShowService.hasShow
            || !!(showScreen.centerItem && showScreen.centerItem.type === "show")
        onTabSelected: (tab) => {
            // Edit means "edit what I'm looking at": a show clicked open in
            // the Show screen's centre preview (ShowCenter) is what Edit
            // should load — switching the tab alone used to leave the Edit
            // screen on whatever it already had (often nothing), ignoring
            // the show right there on screen. Only reload when it's a
            // DIFFERENT show than what's already open, so flipping back and
            // forth while actively editing doesn't reset undo history.
            if (tab === "edit") {
                // Nothing to edit: stay put (the tab is greyed too — this is the
                // guard for a click that got through anyway, a keyboard path, or
                // the last show being closed while the editor held it).
                const item = showScreen.centerItem
                if (!ShowService.hasShow && !(item && item.type === "show"))
                    return
                if (item && item.type === "show" && item.ref !== "" && item.ref !== ShowService.showPath)
                    editScreen.openShowPath(item.ref)
            }
            if (tab === "show" || tab === "edit") window.currentView = tab
        }
        onSettingsClicked: window.openSettings("general")
        onSearchClicked: quickSearch.openSearch()
    }

    // Quick search over the show library — header button, Ctrl+K, and the menu item.
    QuickSearchDialog {
        id: quickSearch
        objectName: "selfTestQuickSearch"
        // The app scale goes on the dialog's CARD, never on this root: the root IS
        // the dialog's dim backdrop, and scaling it shrinks the backdrop away from
        // the window's own edges (the dialog loses its black box). ModalCard does
        // that through the AppScale singleton.
        onResultChosen: (result) => window.openSearchResult(result)
    }

    // What picking a search result does. The dialog only finds things (SearchService, backed
    // by the engine); this is the one place that knows how to go to them.
    function openSearchResult(r) {
        const openT0 = Date.now()
        switch (r.kind) {
        case "show":
            // Same landing as clicking the row in the library: the centre
            // preview, not straight into Edit (this used to jump the
            // search result directly into editing, unlike every other way
            // of reaching a show).
            showScreen.centerItem = { type: "show", ref: r.path, name: r.title, layout: "" }
            window.currentView = "show"
            break
        case "slide":
            editScreen.closeDesign()
            window.currentView = "edit"
            editScreen.session.refresh(r.id)
            break
        case "setting":
            window.openSettings(r.key)
            break
        case "media":
            // Show it in the Media tab, found by its name.
            window.currentView = "show"
            showScreen.showInLibrary("media", r.title)
            break
        case "table": {
            // Straight to the sermon AT its paragraph — the hit carries bookId/chapter/verse.
            // (Pasting the title into the pane's citation search matched nothing: the
            // title is a scripture-shaped label, the pane searches citation lines.)
            window.currentView = "show"
            showScreen.openSermonAt(r.bookId, r.chapter, r.verse, quickSearch.query)
            EventBus.notify(qsTr("Opened %1 in The Table.").arg(r.title), "success", qsTr("The Table"), "search.table.opened")
            break
        }
        case "bible": {
            // Open the SCRIPTURE tab at the verse (user call: picking a Bible result
            // used to build a slide instead — nothing took you to the passage).
            window.currentView = "show"
            showScreen.openVerseAt(r)
            EventBus.notify(qsTr("Opened %1 in Scripture.").arg(r.title), "success", qsTr("Bible"), "search.bible.opened")
            break
        }
        default:
            // Templates, overlays, categories and songs are found (the engine has them) but
            // nothing in the UI shows them yet — say so instead of doing nothing.
            EventBus.notify(qsTr("“%1” — %2. There is no screen for this yet.").arg(r.title).arg(r.subtitle),
                            "info", qsTr("Search"), "search.nodest")
        }
        // The click-to-open cost, so a sluggish pick is diagnosable from the log
        // alone (the whole synchronous path: view switch + tab switch + goTo).
        EngineBridge.log("info", "OpenTrace",
                         "openSearchResult kind=" + r.kind + " took " + (Date.now() - openT0) + "ms")
    }
    // ---- Esc = cancel, Enter = accept -------------------------------------
    // The app's two keys, owned HERE and nowhere else: which surface answers
    // is the Hotkeys scope stack (qml/settings/Hotkeys.qml) — a dialog pushes
    // what cancel/accept mean while it is up, so the one the operator is
    // looking at is the one that answers, and an open confirm beats the dialog
    // it sits on. With nothing open, Esc and Enter reach whatever has focus
    // exactly as they always did (a canvas item, a rename field, a search box).
    //
    // ESC ALSO CLOSES, the way the “…” menu's Close row does. Below every
    // dialog, Esc backs out one level and the last level is the home splash:
    //   * Preview with a show (or project item) open → that page closes
    //     (ShowCenter's own Close: `centerItem = null`, the splash returns);
    //   * the Edit screen, with nothing selected and nothing being typed →
    //     back to Preview, home splash, without closing the show it holds (the
    //     same landing as the tabs, so Edit returns to the document).
    // Still ahead of it, deliberately: an open dialog/picker/panel (a Hotkeys
    // scope — the two never both fire), a text field or a canvas label being
    // typed into (Esc ends the typing, it does not leave the screen), and the
    // canvas' own selection (mCanvas clears that first — see escapeAvailable,
    // which is off while something is selected, so the key reaches the canvas).
    readonly property bool escapeAvailable: !showScreen.typingInField
        && !editScreen.anyTextEditing
        && (window.currentView === "edit"
                ? editScreen.selectedCanvasObjects.length === 0
                : showScreen.centerItem !== null)
    // The keyboard's version of the centre page's Close row.
    function closeToHome() {
        EngineBridge.log("info", "Navigation", "Esc → home (closed the open show)")
        showScreen.centerItem = null
        window.currentView = "show"
    }
    Shortcut {
        action: "cancel"
        enabled: Hotkeys.canCancel || window.escapeAvailable
        onActivated: {
            if (Hotkeys.canCancel)
                Hotkeys.cancel()
            else if (window.currentView === "edit")
                window.closeToHome()
            else
                showScreen.escapeBack()
        }
    }
    Shortcut {
        action: "accept"
        enabled: Hotkeys.canAccept
        onActivated: Hotkeys.accept()
    }

    Shortcut {
        action: "search"
        onActivated: quickSearch.openSearch()
    }

    AppMenuBar {
        id: appMenuBar
        // Same scaled space as the header it draws in. Sized so that, once scaled,
        // it still covers the whole window: this layer is also what catches a
        // click that closes an open menu, so an uncovered strip at a small window
        // would leave menus stuck open.
        readonly property real layerScale: window.uiScale > 0 ? window.uiScale : 1
        width: window.canvasWidth
        height: window.canvasHeight
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        transformOrigin: Item.Top
        scale: layerScale
        onSettingsRequested: (section) => window.openSettings(section)
        // "New show" from either menu = the same deck reset as the Show
        // screen's buttons.
        onNewShowRequested: newShowDialog.open()
        // Show files: the engine reads/writes the .vgr (see ShowSession.qml).
        onOpenShowRequested: {
            window.currentView = "edit"
            editScreen.openShow()
        }
        onQuickSearchRequested: quickSearch.openSearch()
        onSaveShowRequested: editScreen.saveShow()
        onSaveShowAsRequested: editScreen.saveShowAs()
        onImportRequested: importDialog.open = true
        // Record…: start/stop the real engine session with the settings
        // screen's persisted config (the service reads its own blob).
        onRecordToggled: {
            if (RecordingService.recording)
                RecordingService.stopRecording()
            else
                RecordingService.startRecording(RecordingService.config)
        }
    }

    // Ctrl+I: File > Import.
    Shortcut {
        action: "importShow"
        onActivated: importDialog.open = true
    }

    // Ctrl+R: File > Record… — the same toggle.
    Shortcut {
        action: "record"
        onActivated: {
            if (RecordingService.recording)
                RecordingService.stopRecording()
            else
                RecordingService.startRecording(RecordingService.config)
        }
    }

    // Ctrl+N / O / S / Shift+S — the same actions as the File menu.
    Shortcut {
        action: "newShow"
        onActivated: {
            editScreen.newShow()
            window.currentView = "edit"
        }
    }
    Shortcut {
        action: "openShow"
        onActivated: {
            window.currentView = "edit"
            editScreen.openShow()
        }
    }
    Shortcut {
        action: "saveShow"
        onActivated: editScreen.saveShow()
    }
    Shortcut {
        action: "saveShowAs"
        onActivated: editScreen.saveShowAs()
    }

    // The live GROUP shortcuts (Settings · Look & Feel's "Manage groups"):
    // one dynamic binding per group that has a letter assigned — press it to
    // jump to the NEXT slide carrying that group (cycling), the same
    // FreeShow behavior "press V to walk verse 1, 2, 3...". Dynamic (not
    // named in Hotkeys.bindings): these chords are DATA, picked per group by
    // whoever is running the show, not a fixed action this app ships with.
    //
    // Gated to live AND to no text field owning focus — a bare letter chord
    // competing with normal typing (a rename field, the search box, a canvas
    // text edit) is exactly the kind of collision this app's own `addText`
    // shortcut avoids by requiring the canvas's own activeFocus; group
    // shortcuts have no one owning Item to ask instead, so they ask the
    // window's focus item directly.
    readonly property bool textFieldFocused: window.activeFocusItem !== null
        && (window.activeFocusItem instanceof TextInput || window.activeFocusItem instanceof TextEdit)
    Repeater {
        model: {
            void GroupListModel.rosterChanged
            const map = GroupListModel.shortcutMap()
            return Object.keys(map).map((key) => ({ key: key, name: map[key] }))
        }
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.key
            context: Qt.ApplicationShortcut
            enabled: LiveOutputService.live && !window.textFieldFocused
            onActivated: LiveOutputService.jumpToGroup(modelData.name)
        }
    }

    // Settings · Appearance & Actions' own registry — one real
    // LiveOutputService capability per action (ActionListModel.catalogEntries),
    // bound the same way a group's letter is. ONE dispatcher (runEffect)
    // instead of a switch at every call site, so a catalog addition is a
    // single line in ActionListModel.cpp and nothing here changes.
    function runEffect(effectId, effectValue) {
        switch (effectId) {
        case "go_live": LiveOutputService.goLive(); return
        case "stop": LiveOutputService.stop(); return
        case "blackout": LiveOutputService.blackout(); return
        case "end_blackout": LiveOutputService.endBlackout(); return
        case "clear_on_air_slide": LiveOutputService.clearOnAirSlide(); return
        case "clear_slide": LiveOutputService.clearOnAirSlide(); return
        case "clear_all": LiveOutputService.clearAll(); return
        case "clear_media": LiveOutputService.clearMedia(); return
        case "clear_overlays": LiveOutputService.clearAllOverlays(); return
        case "restore_cleared": LiveOutputService.restoreCleared(); return
        case "restore_output": LiveOutputService.restoreCleared(); return
        case "next_slide": LiveOutputService.next(); return
        case "previous_slide": LiveOutputService.previous(); return
        case "jump_to_group": LiveOutputService.jumpToGroup(effectValue); return
        case "media_toggle_play": LiveOutputService.mediaTogglePlay(); return
        case "toggle_playing_media": LiveOutputService.mediaTogglePlay(); return
        case "media_toggle_muted": LiveOutputService.mediaToggleMuted(); return
        }
    }
    // One action, one runner: the shortcut below and the Functions pane's Run
    // mark both land here, so a click and a keypress can never diverge.
    function runActionById(actionId) {
        const row = ActionListModel.rowForId(actionId)
        if (row < 0) return
        const action = ActionListModel.getAction(row)
        const steps = action.steps && action.steps.length
            ? action.steps : [{ effectId: action.effectId, effectValue: action.effectValue }]
        for (let i = 0; i < steps.length; ++i)
            window.runEffect(steps[i].effectId, steps[i].effectValue)
    }
    Repeater {
        model: {
            void ActionListModel.rosterChanged
            const map = ActionListModel.shortcutMap()   // shortcut -> action id
            return Object.keys(map).map((key) => ({ key: key, actionId: map[key] }))
        }
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.key
            context: Qt.ApplicationShortcut
            // "go_live"/"stop" are legal off-air too (that IS the action);
            // every other effect needs a live session to act on, same as
            // the group jump above.
            readonly property var action: {
                void ActionListModel.rosterChanged
                const row = ActionListModel.rowForId(modelData.actionId)
                return row >= 0 ? ActionListModel.getAction(row) : null
            }
            readonly property var steps: action && action.steps && action.steps.length
                ? action.steps : (action ? [{ effectId: action.effectId, effectValue: action.effectValue }] : [])
            readonly property bool canRunOffAir: steps.some((step) => step.effectId === "go_live" || step.effectId === "stop")
            enabled: action !== null && !window.textFieldFocused
                     && (LiveOutputService.live || canRunOffAir)
            onActivated: window.runActionById(modelData.actionId)
        }
    }

    // "Save changes?" over any screen; the show session asks through it.
    UnsavedChangesDialog {
        id: unsavedDialog
        // function(then): the save is a QUEUED document job (ShowService), so the
        // dialog is answered when the verdict lands rather than when the call
        // returns — `then(saved)` is what tells it to proceed or stay put.
        saveAction: function (then) { editScreen.saveShow(then) }
        // The show session asks through this dialog (a function-valued property
        // cannot be bound with Binding, so it is handed over once).
        Component.onCompleted: editScreen.session.askUnsaved = unsavedDialog.ask
    }

    // Closing the window with unsaved changes asks first (once confirmed, it closes).
    property bool closeConfirmed: false
    onClosing: (close) => {
        // Close to tray (Settings > General > Preferences): the window hides and the app keeps running; the tray icon quits it.
        if (!window.closeConfirmed && behaviors.hidesToTray()) {
            close.accepted = false
            window.hide()
            return
        }
        if (window.closeConfirmed || !ShowService.hasShow || !ShowService.showDirty) {
            behaviors.cleanExit()
            return
        }
        close.accepted = false
        unsavedDialog.ask(function () {
            window.closeConfirmed = true
            window.close()
        })
    }

    // Resize borders for the frameless window (the OS handles the resize itself).
    // Deliberately NOT scaled (see uiScale): these ARE the window's frame, so
    // scaling them would pull the resize borders inward, away from the edges the
    // user drags.
    WindowResizeHandles {
        anchors.fill: parent
        // The caption buttons are the one rect the frame leaves alone — see the
        // class comment: these grips don't scale with the app while the buttons
        // inside the scaled header do, so an 8px strip that merely grazed them at
        // full size covers nearly half of an 18px-tall close button once the
        // window scales down (resize cursor over the buttons, clicks resizing
        // instead of closing). Scaled here because the header's own size is in
        // design units and the reserve has to be in window pixels.
        captionReserveWidth: appHeader.captionClusterWidth * window.uiScale
        captionReserveHeight: appHeader.captionClusterHeight * window.uiScale
    }

    // Topmost cursor layer — renders the AppCursor override stack's shape
    // while any gesture (canvas move/resize, AV line-drag) has one pushed;
    // inert the rest of the time. Declared LAST so it z-orders above every
    // screen, the menu bar, and all modals. Also the app's single pointer-
    // position truth source: every position-driven hover (PositionHoverArea)
    // reads AppCursor's point, so this must never be hidden or disabled.
    AppCursorCatcher { }





    // ---- Settings overlay ----
    // Shared ModalScrim: traps outside clicks and consumes wheel events so a
    // modal stays open and scrolling never leaks into the page behind.
    ModalScrim {
        id: settingsScrim
        z: 30000
        visible: false
        // The page layer, from ModalStack's one shared definition (see its
        // own comment) — Settings is the base of the whole dialog stack, so
        // it always blurs the real page, never something nested above it.
        blurSource: ModalStack.pageLayer ? ModalStack.pageLayer() : null
        Component.onCompleted: ModalStack.pageLayer = function() {
            return window.currentView === "edit" ? editScreen : showScreen
        }
        // NOT scaled (see uiScale): this rectangle IS the modal's black box — the
        // full-window dim that makes the dialog read as modal. Scaling the overlay
        // shrank the backdrop away from the window's own edges, so the card floated
        // over an undimmed app. The CARD below carries the app scale; anything
        // nested inside it inherits that and must not scale again.
        // ModalScrim's #99000000 is exactly the 0.6 black this overlay
        // always used — no visual change, just the shared behavior.
        onDismissed: settingsScrim.visible = false
        // Esc closes Settings — the same meaning the card's X has, pushed as a
        // Hotkeys scope for as long as the card is up. There is no accept: the
        // sections apply as they are edited and this shell ships no footer.
        onVisibleChanged: {
            // Opening and closing Settings is part of "where you were" (see
            // rememberSettingsPage): open records the page on screen, closed clears it.
            window.rememberSettingsPage()
            if (settingsScrim.visible) {
                window.settingsHotkeyScope = Hotkeys.open({
                    name: "Settings",
                    cancel: function () { settingsScrim.visible = false }
                })
            } else {
                Hotkeys.close(window.settingsHotkeyScope)
                window.settingsHotkeyScope = 0
            }
        }
        Component.onDestruction: Hotkeys.close(window.settingsHotkeyScope)

        ModalShell {
            id: settingsShell
            // The hook this card's fit scenario grabs (ModalShell has no
            // objectName of its own — the shell is the app's only one).
            objectName: "selfTestSettingsShell"
            // Published once so a dialog opened FROM WITHIN Settings
            // (Manage groups/actions) can blur THIS instead of Show/Edit —
            // see ModalStack.settingsShell's own comment.
            Component.onCompleted: ModalStack.settingsShell = settingsShell
            // The app scale (see uiScale), on the CARD rather than on the scrim
            // above it. Centred, so scaling about the centre leaves it where it
            // was, in proportion with the screens behind it.
            scale: window.uiScale
            transformOrigin: Item.Center
            anchors.centerIn: parent
            currentKey: window.settingsSection
            onSectionSelected: (key) => window.settingsSection = key
            onCloseRequested: settingsScrim.visible = false
            // No Cancel / Save Changes footer: the settings sections apply as you go, and
            // the popup closes with the X or Escape.
            showFooter: false

            Loader {
                    anchors.fill: parent
                    sourceComponent: {
                        switch (window.settingsSection) {
                        case "general": return generalScreenComponent
                        case "smart": return smartConfigScreenComponent
                        case "recording": return recordingScreenComponent
                        case "plugins": return pluginsScreenComponent
                        case "outputs": return outputsScreenComponent
                        case "styles": return stylesScreenComponent
                        case "lookfeel": return lookFeelScreenComponent
                        case "av": return audioVideoScreenComponent
                        default: return placeholderComponent
                        }
                    }
                }
        }
    }

    Component {
        id: generalScreenComponent
        GeneralScreen {
            onSectionRequested: (key) => window.settingsSection = key
        }
    }

    Component {
        id: smartConfigScreenComponent
        SmartConfigScreen {}
    }

    Component {
        id: recordingScreenComponent
        RecordingScreen {}
    }

    Component {
        id: pluginsScreenComponent
        PluginsScreen {}
    }

    Component {
        id: outputsScreenComponent
        OutputsScreen { objectName: "outputsScreen" }
    }

    Component {
        id: stylesScreenComponent
        StylesScreen {}
    }

    Component {
        id: lookFeelScreenComponent
        AppearanceActionsScreen {}
    }

    Component {
        id: audioVideoScreenComponent
        AudioVideoScreen { objectName: "audioVideoScreen" }
    }

    Component {
        id: placeholderComponent
        PlaceholderScreen {
            title: {
                switch (window.settingsSection) {
                case "outputs": return "Outputs"
                default: return window.settingsSection
                }
            }
        }
    }

    // Carries a drag from one pane to another, above everything. In the SAME
    // scaled design space as the screens, so the chip it carries is drawn at the
    // scale of the pane the row came from (DragLayer converts the pointer's window
    // position into that space itself — see its move()).
    DragLayer {
        id: dragLayerItem
        width: window.canvasWidth
        height: window.canvasHeight
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: window.headerExtra
        transformOrigin: Item.Top
        scale: window.uiScale
    }

    // The window-level hover tooltip (TipService publishes the text, this draws
    // it). Deliberately in WINDOW space, NOT the screens' scaled design canvas
    // the drag layer above uses: a tip is text, and the window edge it has to
    // stay inside is a window-space fact (the layer measures an owner inside a
    // scale transform through mapToItem itself).
    ToolTipLayer {
        id: toolTipLayer
        anchors.fill: parent
    }

    // GO LIVE's real destination(s) — a borderless window per Output bound
    // to a physical/HDMI screen in Settings > Outputs. Top-level Windows,
    // not part of this window's visual tree (Instantiator, not a visible
    // child item) — see OutputWindowManager.qml.
    OutputWindowManager {}

    // File > Import: every format the engine reads (FreeShow's Import screen). Above the screens and the Settings overlay, below the toasts.
    ImportDialog {
        id: importDialog
        objectName: "selfTestImportDialog"
        // The app scale lives on the dialog's CARD (ImportDialog.qml, through the
        // AppScale singleton); scaling this root would shrink its dim backdrop.
        onClosed: importDialog.open = false
    }

    // "New show" — FreeShow's own New-show popup (Name, Category, Quick
    // lyrics/Web search/Empty show). "New show" from the Show screen's CTAs
    // and the File menu both open this instead of resetting straight to a
    // blank canvas.
    NewShowDialog {
        id: newShowDialog
        objectName: "selfTestNewShowDialog"
        // The app scale lives on the dialog's CARD (NewShowDialog.qml, through the
        // AppScale singleton); scaling this root would shrink its dim backdrop.
        onShowCreated: (path) => {
            editScreen.openShowPath(path)
            window.currentView = "edit"
        }
        onEmptyShowRequested: (name, category) => {
            editScreen.newShow()
            window.currentView = "edit"
        }
    }

    // Declared LAST (after the Settings overlay) so a crash/error toast
    // always z-orders above an open modal, not just the screens underneath.
    // Scaled by the app scale (see uiScale) about its own BOTTOM-RIGHT corner: a
    // toast shrinks with the rest of the UI while the corner it is pinned to (the
    // window's, plus the usual margin) stays exactly where it was.
    NotificationOverlay {
        // Hook for the modal-fit scenario's toast step (see the sweep below).
        objectName: "selfTestToast"
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: Theme.space5
        scale: window.uiScale
        transformOrigin: Item.BottomRight
        // Keep transient notices in the lower-right corner, away from the
        // monitor wall that occupies the upper-right workspace.
        //
        // ABOVE EVERY MODAL TIER, which is the whole point of a toast over a
        // dialog: the Settings scrim and the modal cards sit at 30000, nested
        // confirm dialogs a step above them (30010), the pickers at 31000 — so
        // this overlay has to be above all of them or a refusal/notice raised
        // from INSIDE a dialog (the routing board's refused drop, an import
        // error) is drawn behind the very dialog that caused it. At 10000 it
        // was under every one of them (NotificationOverlay's own comment has
        // always claimed otherwise).
        z: 40000
    }

    // ---- Settings-inset self-test (VGR_SELFTEST=1 only) ----------------------
    // Renders Settings · General and grabs each section card to a PNG, so
    // tools/selftest/measure_insets.py can assert every card shares one set of
    // insets (the 20px padding the Appearance / Startup cards define). This is
    // the automated guard against the layout drift that let the Settings
    // sections slip to 14px - screenshot truth, not a property trace, because
    // a padding mistake is invisible to every geometry-independent check.
    //
    // Armed only when main.cpp exported SelfTestInsets true (VGR_ENABLE_SELFTEST
    // build + BOTH VGR_SELFTEST=1 and VGR_INSETS_TEST=1): a plain VGR_SELFTEST=1
    // run (the NDI probes) never opens Settings or quits early, and a normal
    // launch never runs a single stage.
    Timer {
        id: selfTestInsetsOpen
        running: typeof SelfTestInsets !== "undefined" && SelfTestInsets
        interval: 2500          // let the first QML frame land before the inset capture
        onTriggered: {
            window.settingsSection = "general"
            settingsScrim.visible = true
            selfTestInsetsGrab.restart()
        }
    }
    Timer {
        id: selfTestInsetsGrab
        interval: 1000          // let the General screen lay out and paint
        onTriggered: {
            const cards = ["selfTestCardAppearance", "selfTestCardStartup",
                           "selfTestCardPreferences", "selfTestCardBackups",
                           "selfTestCardNotifications", "selfTestCardLibraries",
                           "selfTestCardReset"]
            // (selfTestCardStability — Settings · General's "Crash reports" card —
            // is deliberately NOT in this set. The checker compares each card's
            // RIGHT inset, and it does not measure like for like across control
            // shapes: a right-anchored TOGGLE comes back at 25 while a
            // right-anchored CHIP — an ActionRow button, or a pill — comes back
            // at 40-47. That is why Libraries and Reset already fail here, and
            // adding this card would only add a third line for the same reason.
            // The row's own text and wording are covered instead by
            // tools/selftest/run_forensics_test.sh, which reads them off the
            // live item tree.)
            console.log("[SELFTEST-INSETS] grabbing", cards.length, "section cards")
            for (let i = 0; i < cards.length; ++i)
                SelfTest.grab(cards[i], "insets_" + cards[i] + ".png")
            selfTestInsetsDone.restart()
        }
    }
    Timer {
        id: selfTestInsetsDone
        interval: 1500          // let every async grab's saveToFile() land
        onTriggered: {
            console.log("[SELFTEST-INSETS] done - cards grabbed to insets_*.png")
            SelfTest.quit()
        }
    }

    // ---- Popup-modal fit self-test (VGR_MODALFIT_TEST=1 only) ----------------
    // EVERY popup in this app is a card laid over the app, and the window it
    // opens in is frameless and bounded to the active screen (Main.qml lets it
    // be dragged down to 820x560). A card that does not shrink hangs off both
    // edges — its nav rail and its right-hand controls outside the window —
    // which is what "the Settings modal is not responsive" looked like.
    //
    // This sweep opens each popup at the SMALLEST window size, measures its
    // CARD (never the modal root: that always fills the window, so its bounds
    // prove nothing) against the window, grabs it to a PNG for the human, closes
    // it again, and prints one line per popup. The script's grep of `fits=` is
    // the whole checker. Nothing here presses Save/Add, so no roster, show or
    // category is created by a run.
    //
    // Armed only with BOTH VGR_SELFTEST=1 and VGR_MODALFIT_TEST=1: a plain
    // VGR_SELFTEST=1 run (how the NDI probes launch the app) never runs this.
    function modalFitCard(rootName) {
        const named = SelfTest.findItem(rootName + "_card")   // ModalCard names its own card
        if (named)
            return named
        const root = SelfTest.findItem(rootName)
        if (!root || root.width === undefined)
            return null
        // A modal ROOT fills the window: its card is the largest child that does
        // NOT fill it (the scrim does), which covers the dialogs built on their
        // own card instead of ModalCard.
        let best = null
        const kids = root.children
        for (let i = 0; i < kids.length; ++i) {
            const c = kids[i]
            if (!c || c.width === undefined || !c.visible || c.opacity === 0)
                continue
            if (c.width >= root.width * 0.95 && c.height >= root.height * 0.95)
                continue
            if (!best || c.width * c.height > best.width * best.height)
                best = c
        }
        if (best)
            return best
        // No child smaller than the root: the root IS the card (ModalShell's
        // shape) only when its own mapped rect is smaller than the window in at
        // least one direction. A root that FILLS the window and hides its card is
        // reported as unmeasurable rather than passing on the window's own
        // bounds — a trivially-true "fits" is worse than no measurement at all.
        const r = modalFitRect(root)
        const smaller = r.w < window.width * 0.98 || r.h < window.height * 0.98
        return smaller ? root : null
    }
    // An item's rect in WINDOW coordinates. Measured from its mapped corners, not
    // from x/y/width/height: a card inside a scaled parent (the Settings card is
    // scaled to fit small windows) is smaller on screen than its own width, and
    // comparing local sizes against window bounds would read every scaled card as
    // clipped.
    function modalFitRect(item) {
        const p0 = item.mapToItem(null, 0, 0)
        const p1 = item.mapToItem(null, item.width, item.height)
        return { x: p0.x, y: p0.y, w: p1.x - p0.x, h: p1.y - p0.y }
    }
    // The AV routing board is three columns laid out by hand (audio inputs,
    // buses, video), inside a Flickable that never scrolls horizontally — so a
    // board wider than its room pushes the right-hand column out of sight with
    // no way to reach it. Printed next to the popup measurements because that is
    // the same failure shape: "covered the video bus".
    // Proves the ENFORCED half of the app scale: every window-level layer must
    // carry the same scale (window.uiScale), and nothing nested may apply it a
    // second time. A layer that forgets it, or a component that scales itself
    // twice, shows up here as a different number — the script fails on that.
    function reportUiScales() {
        const rows = []
        const add = (name, item) => rows.push(name + "=" + (item && item.scale !== undefined ? item.scale.toFixed(3) : "?"))
        // The layers that carry the design canvas itself.
        add("screens", showScreen)
        add("edit", editScreen)
        add("header", appHeader)
        add("menubar", appMenuBar)
        add("drag", dragLayerItem)
        // ...and the modal CARDS. Only the card is scaled: the scrim behind it has
        // to stay window-sized or the dialog loses its black box (checked next, in
        // reportScrims).
        add("settings", settingsShell)
        add("quicksearch", SelfTest.findItem("selfTestQuickSearch_card"))
        add("import", SelfTest.findItem("selfTestImportDialog_card"))
        add("newshow", SelfTest.findItem("selfTestNewShowDialog_card"))
        console.log("[SELFTEST-UISCALE] uiScale=" + window.uiScale.toFixed(3) + " " + rows.join(" "))
    }
    // The other half of the modal rule: EVERY window-level scrim must still be the
    // whole window, unscaled. This is the measurement behind "the popup has no
    // black box around it" — a scrim that inherited a layer's scale reports a rect
    // smaller than the window, and the script fails on that line.
    // One scrim measurement. For a ModalCard the root itself IS the scrim; the
    // hand-rolled dialogs fill their root with one — either way the root's rect is
    // the backdrop, and it must be the WHOLE window: a scrim that covered only the
    // page a dialog was declared in left the Settings card's nav rail (or the
    // header) lit, which is what "the black box only covers one side" looked like.
    function reportScrim(label, item) {
        if (!item) {
            console.log("[SELFTEST-SCRIMS] " + label + " MISSING covers=false")
            return
        }
        const r = modalFitRect(item)
        const covers = Math.abs(r.x) <= 1 && Math.abs(r.y) <= 1
                    && Math.abs(r.w - window.width) <= 1 && Math.abs(r.h - window.height) <= 1
        console.log("[SELFTEST-SCRIMS] " + label
                    + " rect=" + Math.round(r.w) + "x" + Math.round(r.h)
                    + " at " + Math.round(r.x) + "," + Math.round(r.y)
                    + " scale=" + (item.scale !== undefined ? item.scale.toFixed(2) : "?")
                    + " covers=" + covers)
    }
    // The window-level modals, reportable at any point in the sweep (they all exist
    // from load). The dialogs declared inside a Settings PAGE are checked by the
    // step that opens them instead (a step's `scrim:` name): the page — and so the
    // dialog — does not exist yet when this runs.
    function reportScrims() {
        reportScrim("settings", settingsScrim)
        reportScrim("quicksearch", quickSearch)
        reportScrim("import", importDialog)
        reportScrim("newshow", newShowDialog)
    }

    // The screens are drawn at the design canvas and scaled as a whole, so "does
    // it fit" is only half the story: the scaled screen must also REACH the
    // window's own edges. A design-sized canvas left the bottom dock ~26px above
    // the window at 820x560 — nothing hung outside, but the project pane and the
    // library dock visibly detached from the window.
    //
    // The TOP is the one edge a screen does not touch by design: it is pushed down
    // by headerExtra so its own first 48px sit under the header strip. Left,
    // right and bottom are the edges whose attachment the user sees.
    function reportScreenCoverage() {
        const r = modalFitRect(showScreen)
        const fits = r.x >= -0.5 && r.y >= -0.5
                     && r.x + r.w <= window.width + 0.5 && r.y + r.h <= window.height + 0.5
        const covers = Math.abs(r.x) <= 1
                    && Math.abs(r.x + r.w - window.width) <= 1
                    && Math.abs(r.y + r.h - window.height) <= 1.5
                    && Math.abs(r.y - window.headerExtra) <= 1.5
        console.log("[SELFTEST-MODALFIT] main-screen"
                    + " window=" + window.width + "x" + window.height
                    + " card=" + Math.round(r.w) + "x" + Math.round(r.h)
                    + " at " + Math.round(r.x) + "," + Math.round(r.y)
                    + " fits=" + fits + " covers=" + covers)
        SelfTest.grab("selfTestMainScreen", "modal_fit_main-screen.png")
    }

    // The desktop the app sized itself to at startup, and whether it did: the
    // window must fill the work area (minus the small margin Main.qml opens it
    // with), so the app is scaled to the screen it started on rather than to its
    // 1440x900 design size. `engine` is the panel the ENGINE reported (its real
    // pixels divided by Qt's devicePixelRatio — the same unit as `desktop`), and
    // 0x0 means the engine had no display yet and Qt's Screen was the fallback.
    // Measured at the LAUNCH size, before the sweep shrinks the window.
    function reportDesktop() {
        const fills = window.width >= window.desktopWidth - 32 - 0.5
                    && window.height >= window.desktopHeight - 64 - 0.5
        console.log("[SELFTEST-DESKTOP] desktop=" + Math.round(window.desktopWidth) + "x" + Math.round(window.desktopHeight)
                    + " engine=" + Math.round(window.realDisplayWidth) + "x" + Math.round(window.realDisplayHeight)
                    + " dpr=" + window.pixelRatio
                    + " window=" + Math.round(window.width) + "x" + Math.round(window.height)
                    + " uiScale=" + window.uiScale.toFixed(3)
                    + " fills=" + fills)
    }

    // Card anatomy, printed for the dialog the sweep just measured. It exists
    // because the card's height is DERIVED (header + a capped Flickable + the
    // footer), so a dialog can measure as "fits the window" while its content
    // viewport has collapsed to nothing — the "Add Source is cleared" face of a
    // broken binding. `blank=` is that verdict: a card with real content but no
    // viewport height. The height chain is printed next to it so the failing
    // link (flick, headerItem, footer) is visible in the log, not guessed at.
    function reportCardTree(name) {
        const card = SelfTest.findItem(name + "_card")
        if (!card) {
            console.log("[SELFTEST-CARD] " + name + " no card blank=false")
            return
        }
        const out = []
        const kids = card.children
        for (let i = 0; i < kids.length; ++i) {
            const c = kids[i]
            if (!c || c.width === undefined)
                continue
            out.push(i + ":y" + Math.round(c.y) + ",h" + Math.round(c.height) + ",w" + Math.round(c.width))
        }
        const flick = card.children[1]
        const contentH = Math.round(flick.contentHeight)
        const flickH = Math.round(flick.height)
        console.log("[SELFTEST-CARD] " + name + " card=" + Math.round(card.width) + "x" + Math.round(card.height)
                    + " roomW=" + Math.round(card.roomWidth) + " roomH=" + Math.round(card.roomHeight)
                    + " scale=" + card.scale.toFixed(3) + " rootW=" + Math.round(card.parent.width)
                    + " rootH=" + Math.round(card.parent.height)
                    + " flickH=" + flickH + " contentH=" + contentH
                    + " contentColH=" + (flick.children[0] ? Math.round(flick.children[0].height) : -1)
                    // "?" means the property did not come back readable through the
                    // driver's item handle — the footer's real box is in kids= above.
                    + " footerH=" + (isFinite(card.footerHeight) ? Math.round(card.footerHeight) : "?")
                    // Content that exists with no viewport to show it in: the card
                    // renders as an empty shell. Nothing else in the sweep catches
                    // it, because an empty card still "fits the window".
                    + " blank=" + (contentH > 1 && flickH < 1)
                    + " kids=[" + out.join(" ") + "]")
    }
    // Where an open dropdown is STACKED, printed for the Source menu the sweep just
    // took down. A device list that pops into the window's contentItem instead of
    // the Controls overlay shares no stacking context with a modal (every modal card
    // parents itself to Overlay.overlay), so it draws BEHIND the open dialog — the
    // list "is nowhere in sight" while every geometry check still passes. `layer=`
    // is that verdict: the panel and the modal in the same layer, panel above it.
    function reportDropdown(where, avName) {
        const menu = SelfTest.findItem("selfTestAddSourceSourceField_menu")
        const av = SelfTest.findItem(avName)
        const modal = SelfTest.findItem("selfTestAddSourceDialog")
        const sameLayer = menu && modal && menu.parent === modal.parent
        console.log("[SELFTEST-DROP] " + where + " menu=" + (menu ? (Math.round(menu.width) + "x" + Math.round(menu.height)
                                                 + " at " + Math.round(menu.x) + "," + Math.round(menu.y)
                                                 + " z=" + menu.z + " scale=" + menu.scale.toFixed(3)
                                                 + " visible=" + menu.visible) : "MISSING")
                    + " modalZ=" + (modal ? modal.z : "?")
                    + " sameLayer=" + sameLayer
                    + " options=" + (av ? av.videoSourceOptions("camera").length : "?")
                    + " engineCams=" + (av ? av.engineVideoDevices.length : "?")
                    + " layer=" + (sameLayer && menu.z > modal.z))
    }

    // A notice. It takes the app scale — scaled about its own bottom-right corner,
    // so it shrinks with the rest of the UI AND stays pinned to the window's corner
    // — and its text is plain UTF-8 end to end (the engine's own strings used to
    // carry an em dash that had been UTF-8 encoded twice, which the toast rendered
    // as "â"-style garbage). Measured from the card itself, in window coordinates:
    // a 320px toast at uiScale 0.569 must come back ~182px wide, inside the window.
    function reportToast() {
        const card = SelfTest.findItem("selfTestToastCard")
        if (!card) {
            console.log("[SELFTEST-TOAST] toast MISSING fits=false")
            return
        }
        const r = modalFitRect(card)
        const wantW = 320 * window.uiScale
        const inside = r.x >= -0.5 && r.y >= -0.5
                       && r.x + r.w <= window.width + 0.5 && r.y + r.h <= window.height + 0.5
        const scaled = Math.abs(r.w - wantW) <= 1.5
        console.log("[SELFTEST-TOAST] window=" + window.width + "x" + window.height
                    + " card=" + Math.round(r.w) + "x" + Math.round(r.h)
                    + " at " + Math.round(r.x) + "," + Math.round(r.y)
                    + " want=" + Math.round(wantW)
                    + " rightGap=" + Math.round(window.width - (r.x + r.w))
                    + " bottomGap=" + Math.round(window.height - (r.y + r.h))
                    + " fits=" + (inside && scaled))
    }

    // The notices must COALESCE, not stack: the toast step above pushes the same
    // text six times (the shape a settings write raises when the engine is not
    // running — one notice per attempt), and six copies used to become six cards.
    // One card standing for all six is the pass.
    function reportToastDedup() {
        const items = NotificationCenter.items
        const cards = items.length
        const count = cards === 1 ? (items[0].groupCount || 1) : 0
        console.log("[SELFTEST-TOASTDEDUP] pushed=6 cards=" + cards
                    + " groupCount=" + count
                    + " dedupe=" + (cards === 1 && count === 6))
    }
    // The notice's CRUX has to land ON SCREEN, not just sit in a property: the
    // names it is about (a device, a source) ride the payload as `crux` and the
    // card must hand the renderer a <b> run around each one, with the rest of
    // the sentence plain and unchanged. `emphasis=` is that verdict — a card
    // drawn in plain text is exactly the complaint this step answers. The card
    // is grabbed too, so a passing run still leaves something to look at.
    // The notice's one click: the card draws a button from the notice's action
    // (its id and label) and pressing it has to run what that id was registered
    // for — the whole point of the crash-recovery offer, whose only way to open
    // the recovered show is this button. Driven as a REAL click at the button's
    // centre, with the registry called directly as the documented fallback when
    // the synthetic click misses, and `by=` says which of the two answered.
    property bool toastActionRan: false
    function reportToastAction() {
        const button = SelfTest.findItem("selfTestToastAction")
        const present = button !== null && button.visible && button.width > 0
        window.toastActionRan = false
        if (present)
            SelfTest.clickItem("selfTestToastAction")
        let by = present ? "click" : "none"
        if (!window.toastActionRan) {
            NotificationCenter.runAction("ui.selftest.toastAction")
            by = present ? "click+runAction" : "runAction"
        }
        console.log("[SELFTEST-TOASTACTION] button=" + present
                    + " ran=" + window.toastActionRan + " by=" + by)
    }
    function reportToastCrux() {
        const card = SelfTest.findItem("selfTestToastCard")
        const label = SelfTest.findItem("selfTestToastMessage")
        if (!card || !label) {
            console.log("[SELFTEST-TOASTCRUX] card=" + !!card + " label=" + !!label
                        + " emphasis=false")
            return
        }
        const spans = card.cruxSpans()
        const html = label.text
        let bolded = 0
        for (let i = 0; i < spans.length; ++i)
            if (html.indexOf("<b>" + card.escapeHtml(spans[i]) + "</b>") !== -1)
                ++bolded
        // Take the tags back off the rendered markup — never escape it twice —
        // and what is left has to BE the escaped message the card was handed,
        // i.e. bolding never changed a character of the text.
        const stripped = html.replace(/<\/?b>/g, "")
        const rich = label.textFormat === Text.RichText
        const kept = card.escapeHtml(card.message) === stripped
        console.log("[SELFTEST-TOASTCRUX] spans=" + spans.length
                    + " bolded=" + bolded
                    + " rich=" + rich
                    + " textKept=" + kept
                    + " emphasis=" + (spans.length > 0 && bolded === spans.length
                                      && rich && kept))
        SelfTest.grab("selfTestToastCard", "toast_crux.png")
    }
    function reportAvBoard() {
        const b = SelfTest.findItem("selfTestAvBoard")
        if (!b) {
            console.log("[SELFTEST-AVBOARD] board MISSING")
            return
        }
        const right = b.videoX + b.vidColW
        console.log("[SELFTEST-AVBOARD] board=" + Math.round(b.width)
                    + " audio=0.." + Math.round(b.colW)
                    + " bus=" + Math.round(b.busX) + ".." + Math.round(b.busX + b.busColW)
                    + " video=" + Math.round(b.videoX) + ".." + Math.round(right)
                    + " overflow=" + Math.round(Math.max(0, right - b.width)) + "px"
                    + " fits=" + (right <= b.width + 1))
    }
    // The reported bug this step exists for: a source card's Delete left its
    // curved line drawn ("delete the card first and the line refuses to be
    // deleted"), while removing the line by its own click first made the delete
    // clean. The line IS a connector pair — the board draws one per route its
    // buses report — so the check routes a row on the real board, deletes it
    // through the SCREEN's own removal path (the function the card's context
    // menu and the Edit dialog's Delete button both call), and reads the board
    // back: the pair must be gone, the row gone, and the bus must have lost
    // exactly that one route (the count it had before the route was added).
    property int edgeBus: -1            // the bus this step created itself (if any)
    property int edgeRoutesBefore: -1
    function reportEdgeRemoval() {
        const board = SelfTest.findItem("selfTestAvBoard")
        const screen = SelfTest.findItem("audioVideoScreen")
        if (!board || !screen || typeof screen.removeAudioFixingRoutes !== "function") {
            console.log("[SELFTEST-EDGE] board=" + !!board + " screen=" + !!screen
                        + " connected=false clean=false")
            return
        }
        // A bus of this step's own only when the board has none at all; it is
        // removed again below, and only if it is still the row this step added.
        let bus = 0
        if (BusListModel.rowCount() === 0) {
            BusListModel.addBus("SelfTest Edge Bus", "audio")
            bus = BusListModel.rowCount() - 1
            window.edgeBus = bus
        }
        const row = AudioInputListModel.rowCount()
        AudioInputListModel.addInput()
        BusListModel.connectAudioRoute(bus, row)
        const drawn = function () {
            return board.connectorPairs().some(function (p) {
                return p.kind === "audio" && p.busIndex === bus && p.srcIndex === row })
        }
        window.edgeRoutesBefore = BusListModel.getBus(bus).routedAudioInputs.length - 1
        const lineBefore = drawn()
        screen.removeAudioFixingRoutes(row)          // the card's own Delete path
        const lineAfter = drawn()
        const rowGone = AudioInputListModel.rowCount() === row
        const routesNow = BusListModel.getBus(bus).routedAudioInputs.length
        const routesRestored = window.edgeRoutesBefore >= 0
                                && routesNow === window.edgeRoutesBefore
        if (window.edgeBus >= 0 && window.edgeBus < BusListModel.rowCount()
                && BusListModel.getBus(window.edgeBus).name === "SelfTest Edge Bus")
            BusListModel.removeBus(window.edgeBus)
        window.edgeBus = -1
        console.log("[SELFTEST-EDGE] connected=true lineBefore=" + lineBefore
                    + " lineAfter=" + lineAfter + " rowGone=" + rowGone
                    + " routesBefore=" + window.edgeRoutesBefore
                    + " routesNow=" + routesNow
                    + " clean=" + (lineBefore && !lineAfter && rowGone && routesRestored))
    }
    function reportModalFit(label, rootName) {
        reportFit(label, modalFitCard(rootName), rootName)
    }
    // Shared by the popup sweep and the screen check: measure one item against the
    // window and print the verdict line the script greps.
    function reportFit(label, card, grabName) {
        if (!card) {
            console.log("[SELFTEST-MODALFIT] " + label + " card=" + grabName + " MISSING fits=false")
            return
        }
        // Window coordinates, however deep the card sits (the AV dialogs are
        // nested inside the Settings card, which may itself be scaled down).
        const r = modalFitRect(card)
        const fits = r.x >= -0.5 && r.y >= -0.5
                     && r.x + r.w <= window.width + 0.5 && r.y + r.h <= window.height + 0.5
        console.log("[SELFTEST-MODALFIT] " + label
                    + " window=" + window.width + "x" + window.height
                    + " card=" + Math.round(r.w) + "x" + Math.round(r.h)
                    + " at " + Math.round(r.x) + "," + Math.round(r.y)
                    + " fits=" + fits)
        SelfTest.grab(grabName, "modal_fit_" + label + ".png")
    }

    // The sweep. Each entry: `open()` it, then next tick measure `card` (an
    // objectName; "" skips the measurement for a pure setup/teardown step),
    // grab it, `close()` it. The two section-switch steps exist because a page's
    // controls only exist once the Loader has built them.
    readonly property var modalFitSweep: [
        {
            // The Show screen itself, at the smallest size: the screens are drawn
            // at the design canvas and scaled as a whole, so the checks are that
            // the scaled screen lands inside the window (nothing inside it can
            // overflow the way a reflowed layout could) AND that it covers it —
            // see reportScreenCoverage for why "covers" is the attachment check.
            label: "main-screen", card: "",
            open: function () { },
            close: function () { },
            report: function () { reportScreenCoverage() }
        },
        {
            // The enforced scale: every layer must carry window.uiScale. Measured
            // at the smallest size, where a missing scale is visible as a mismatch.
            label: "ui-scale", card: "",
            open: function () { },
            close: function () { },
            report: function () { reportUiScales(); reportScrims() }
        },
        {
            label: "settings-shell", card: "selfTestSettingsShell",
            open: function () { window.settingsSection = "general"; settingsScrim.visible = true },
            close: function () { }
        },
        {
            label: "settings-av-page", card: "selfTestSettingsShell",
            open: function () { window.settingsSection = "av" },
            // Measured in the tick AFTER the page has been built.
            close: function () { reportAvBoard() }
        },
        {
            // Deleting a source CARD has to take its curved LINE with it: the
            // report was "delete the card first and the line refuses to be
            // deleted" (its own click resolves a roster ROW to a graph node, and
            // a row that is already gone has none — so the line's own removal
            // had nothing left to act on). Proved on the real board: route a
            // row, delete it through the screen's own removal path (exactly what
            // the card's menu and the Edit dialog's Delete button call) and read
            // the board back — the line is a connector pair, so "no pair names
            // it" is "no line is drawn for it".
            label: "av-edge-delete", card: "",
            open: function () { },
            close: function () { },
            report: function () { reportEdgeRemoval() }
        },
        {
            label: "av-add-audio", card: "selfTestAddSourceDialog", scrim: "selfTestAddSourceDialog",
            open: function () { SelfTest.clickItem("selfTestAvAddAudio") },
            close: function () { const s = SelfTest.findItem("audioVideoScreen"); if (s) s.addSourceShown = false }
        },
        {
            label: "av-add-video", card: "selfTestAddSourceDialog",
            open: function () { SelfTest.clickItem("selfTestAvAddVideo") },
            close: function () {
                reportCardTree("selfTestAddSourceDialog")
                const s = SelfTest.findItem("audioVideoScreen")
                if (s)
                    s.addSourceShown = false
            }
        },
        {
            // The dialog's SOURCE field, opened for real: the popup it opens is a
            // window-level layer (SelectField reparents it to the window), so the
            // check is that it appears at all, lands inside the window and is drawn
            // at the app scale. This is the "clicked Source and nothing appeared"
            // case — the popup is measured, not just the dialog around it.
            label: "av-source-drop-open", card: "",
            open: function () { SelfTest.clickItem("selfTestAvAddVideo") },
            close: function () { }
        },
        {
            label: "av-source-drop", card: "selfTestAddSourceSourceField_menu",
            scrim: "selfTestAddSourceDialog",
            open: function () { SelfTest.clickItem("selfTestAddSourceSourceField_box") },
            // The camera roster is enumerated OFF the GUI thread, and since the
            // boot sweep asks only for the devices the setup uses (see
            // EngineBridge::requestUsedDevices), opening this dialog starts a
            // real full sweep that finishes a moment later. A synthetic click
            // lands in the same frame and would measure an EMPTY picker — a
            // false "clicked Source and nothing appeared". Wait, bounded, for
            // the roster this step is about to inspect.
            waitFor: function () {
                const av = SelfTest.findItem("audioVideoScreen")
                return !av || av.engineVideoDevices.length > 0
            },
            report: function () {
                reportModalFit("av-source-drop", "selfTestAddSourceSourceField_menu")
                reportDropdown("av-source-drop", "audioVideoScreen")
            },
            close: function () {
                const m = SelfTest.findItem("selfTestAddSourceSourceField_menu")
                if (m)
                    m.visible = false
                const s = SelfTest.findItem("audioVideoScreen")
                if (s)
                    s.addSourceShown = false
            }
        },
        {
            label: "av-add-bus", card: "selfTestBusDialog", scrim: "selfTestBusDialog",
            open: function () { SelfTest.clickItem("selfTestAvAddBus") },
            close: function () { const s = SelfTest.findItem("audioVideoScreen"); if (s) s.busDialogOpen = false }
        },
        {
            label: "settings-outputs-page", card: "selfTestSettingsShell",
            open: function () { window.settingsSection = "outputs" },
            close: function () { }
        },
        {
            label: "outputs-add", card: "selfTestAddOutputDialog", scrim: "selfTestAddOutputDialog",
            open: function () { SelfTest.clickItem("selfTestAddOutput") },
            close: function () { const s = SelfTest.findItem("outputsScreen"); if (s) s.addShown = false }
        },
        {
            label: "settings-close", card: "",
            open: function () { settingsScrim.visible = false },
            close: function () { }
        },
        {
            label: "new-show", card: "selfTestNewShowDialog",
            open: function () { newShowDialog.open() },
            close: function () { newShowDialog.close() }
        },
        {
            label: "import", card: "selfTestImportDialog",
            open: function () { importDialog.open = true },
            close: function () { importDialog.open = false }
        },
        {
            label: "quick-search", card: "selfTestQuickSearch",
            open: function () { quickSearch.openSearch() },
            close: function () { quickSearch.close() }
        },
        {
            label: "category-name", card: "selfTestCategoryNameDialog", scrim: "selfTestCategoryNameDialog",
            open: function () { SelfTest.clickItem("selfTestShowsNewCategory") },
            close: function () { const d = SelfTest.findItem("selfTestCategoryNameDialog"); if (d) d.close() }
        },
        {
            // The notices: they take the app scale now (scaled about their own
            // bottom-right corner, so they shrink with the UI but stay pinned to
            // the window's corner). Pushed with the punctuation the engine's own
            // and UI strings actually use — the toast text used to render a
            // double-encoded em dash as "â"-style garbage.
            // Pushed SIX times with one identical text on purpose: that is the
            // reported shape — a settings write that cannot reach the engine
            // raises the same notice once per attempt, and six attempts stacked
            // six toasts the operator could not read. One card must stand for
            // all six (groupCount), and reportToastDedup() is the check.
            // Its text is the missing-device notice the app really raises, crux
            // and all, so this single step covers the geometry, the coalescing
            // AND the bolding the operator complained was missing
            // (reportToastCrux).
            label: "toast", card: "",
            open: function () {
                const text = "Not connected this time: WindowsClipboard.cpp - WindowsUI - Visual Studio Code — the inputs that use them read no signal until “Camera 1” is back (Settings · Audio & Video)."
                // The names the notice is ABOUT, the same shape the boot and
                // missing-device notices use as their crux: the toast has to
                // draw the device name bold inside the sentence, with the quoted
                // source name bold too (see reportToastCrux).
                const crux = ["WindowsClipboard.cpp - WindowsUI - Visual Studio Code"]
                // ...and with a ONE-CLICK ACTION, the shape the crash-recovery
                // offer uses: the id is registered here, the notice carries only
                // its name and label, and the card's button has to run it (see
                // reportToastAction).
                window.toastActionRan = false
                NotificationCenter.registerAction("ui.selftest.toastAction",
                                                   function() { window.toastActionRan = true })
                const action = { id: "ui.selftest.toastAction", label: qsTr("Open recovered show") }
                // Pushed through the SAME door the app's real notices use —
                // EventBus.notify, crux and action as its last arguments — so this
                // step covers the production path end to end (payload "crux" ->
                // NotificationCenter -> the card's <b> runs, and payload "action"
                // -> the card's one button), not just the card's own rendering of
                // a hand-passed pair.
                for (let i = 0; i < 6; ++i)
                    EventBus.notify(text, "warning", qsTr("Devices"), "ui.selftest.toast", crux, action)
            },
            close: function () { NotificationCenter.clear() },
            report: function () { reportToast(); reportToastDedup(); reportToastCrux(); reportToastAction() }
        },
        {
            // Last, back at the size the app opened at with the AV page up: the
            // board's columns must be EXACTLY where they were before the fit work
            // (they are, since a card with room keeps its design size and its scale
            // of 1), so a small-window repair cannot quietly re-space the normal
            // layout. This is also the control reading for the AVBOARD line: the
            // one above it is the cramped case.
            label: "settings-av-design-size", card: "selfTestSettingsShell",
            open: function () {
                window.width = modalFitLaunchW
                window.height = modalFitLaunchH
                window.settingsSection = "av"
                settingsScrim.visible = true
            },
            close: function () { reportAvBoard() }
        }
    ]
    property int modalFitStep: -1
    property bool modalFitOpened: false
    // How many ticks the CURRENT step has spent waiting on its own `waitFor`
    // predicate (see modalFitAdvance) — bounded so a roster that never arrives
    // fails the step instead of hanging the scenario.
    property int modalFitWaitTries: 0
    readonly property int modalFitWaitMax: 12   // ~16 s at modalFitDelay's tick
    property real modalFitLaunchW: 1440
    property real modalFitLaunchH: 900

    function modalFitAdvance() {
        const steps = modalFitSweep
        if (modalFitStep < 0) {
            modalFitStep = 0
            modalFitOpened = false
        }
        if (modalFitStep >= steps.length) {
            settingsScrim.visible = false
            SelfTest.quit()
            return
        }
        const s = steps[modalFitStep]
        if (!modalFitOpened) {
            s.open()
            modalFitOpened = true
            modalFitWaitTries = 0
        } else {
            // A step whose subject is ASYNCHRONOUS declares `waitFor`: the step
            // repeats (open already happened) until it answers true, or the
            // bounded budget runs out and the step measures what it has — which
            // fails loudly in the report rather than passing on an empty roster.
            if (s.waitFor !== undefined && !s.waitFor()) {
                modalFitWaitTries++
                if (modalFitWaitTries <= modalFitWaitMax) {
                    modalFitDelay.restart()
                    return
                }
                console.log("[SELFTEST-MODALFIT] " + s.label
                            + " waitFor TIMED OUT after " + modalFitWaitTries + " ticks")
            }
            if (s.report)
                s.report()
            else if (s.card !== "")
                reportModalFit(s.label, s.card)
            // A step that names a `scrim` also proves the dialog's dim covers the
            // whole window, measured while it is open (the settings-page dialogs
            // only exist once their page has been built).
            if (s.scrim !== undefined)
                reportScrim(s.label + "-scrim", SelfTest.findItem(s.scrim))
            s.close()
            modalFitStep++
            modalFitOpened = false
        }
        modalFitDelay.restart()
    }

    Timer {
        id: selfTestModalFitOpen
        running: typeof SelfTestModalFit !== "undefined" && SelfTestModalFit
        interval: 2500          // let the first QML frame land, as the inset test does
        onTriggered: {
            window.settingsSection = "general"
            settingsScrim.visible = true
            selfTestModalFitLaunch.restart()
        }
    }
    Timer {
        id: selfTestModalFitLaunch
        interval: 1200          // let the card lay out at the launch size
        onTriggered: {
            // The one measurement the user never sees a problem with, kept as
            // the control: the card at its full 1080x640 design size.
            reportModalFit("settings-launch", "selfTestSettingsShell")
            reportDesktop()
            modalFitLaunchW = window.width          // remembered for the design-size step
            modalFitLaunchH = window.height
            window.width = window.minimumWidth      // the smallest the user can drag to
            window.height = window.minimumHeight
            modalFitStep = -1
            modalFitOpened = false
            modalFitDelay.restart()
        }
    }
    Timer {
        id: modalFitDelay
        interval: 1300          // one popup's open, or its measure+grabs+close, per tick
        onTriggered: modalFitAdvance()
    }

    // ---- sidebar split drag (VGR_SPLIT_TEST=1) -------------------------------
    // Proves a dragged sidebar width is actually REMEMBERED, end to end: press the
    // Projects divider, drag it, release, then read the value back out of the store
    // after the pane's own 500ms save timer has had time to run. It is the guard for
    // the whole class of bug around this save path — a timer that fires without a
    // change (which wrote the untouched columns back on every launch) and a timer
    // that never fires at all both fail a check like this, while the store's own
    // value is what the UI binds to, so "after" is the honest reading.
    property real splitBefore: -1
    Timer {
        id: selfTestSplitDrag
        running: typeof SelfTestSplit !== "undefined" && SelfTestSplit
        interval: 2500          // let the main screen finish laying out
        onTriggered: {
            const strip = SelfTest.findItem("selfTestProjectSplit")
            if (!strip) {
                console.log("[SELFTEST-SPLIT] split MISSING saved=false")
                SelfTest.quit()
                return
            }
            window.splitBefore = Number(SettingsService.values["session.projectsColumnWidth"] ?? -1)
            const h = SelfTest.itemCenter("selfTestProjectSplit")
            // Park the pointer well away first: QCursor::setPos generates NO WM_MOUSEMOVE
            // when the cursor is already where it is asked to go, so a drag that starts
            // near where the last one ended can lose its very first step.
            SelfTest.move(8, 8)
            SelfTest.press(h.x, h.y)
            SelfTest.move(h.x + 20, h.y)
            SelfTest.move(h.x + 40, h.y)
            SelfTest.move(h.x + 60, h.y)
            SelfTest.move(h.x + 80, h.y)
            SelfTest.release(h.x + 80, h.y)
            console.log("[SELFTEST-SPLIT] dragged override=" + Math.round(showScreen.projectsColumnOverride))
            selfTestSplitCheck.restart()
        }
    }
    Timer {
        id: selfTestSplitCheck
        interval: 2000          // the 500ms save timer, then the store write
        onTriggered: {
            const after = Number(SettingsService.values["session.projectsColumnWidth"] ?? -1)
            console.log("[SELFTEST-SPLIT] split before=" + window.splitBefore + " after=" + after
                        + " saved=" + (after > 0 && after !== window.splitBefore))
            SelfTest.quit()
        }
    }

    // ---- Media pane L/R pills (VGR_PILL_TEST=1) ------------------------------
    // The report: "if i click on the L or R it closes all". The Media pane's Inputs
    // section is the BUSES (the audio-input and video-source rosters moved out of
    // the pane and back to the Settings - Audio & Video board that owns them), so
    // the scenario drives the bus card: it opens the pane's Inputs roster, clicks
    // the L pill (a real synthetic click at the pill's centre — the pill's own
    // toggled() is the documented fallback when that click misses), then reads BOTH
    // sides back out of the model. The clicked side must be closed and the OTHER
    // side must still be open. Trims and any row it added are put back, so the
    // roster reads as it did before. (The video cards' identical pair left with
    // them: the ChannelPill control now has exactly one roster to serve here.)
    property var pillCases: [{ name: "bus" }]
    property int pillCase: 0
    property int pillStep: 0        // 0 set up, 1 click, 2 read, 3 read the fallback
    property int pillRow: -1
    property int pillAddedBus: -1
    property int pillAddedVideo: -1
    property string pillBy: ""
    property string pillSetupBy: ""
    property int pillSetupTries: 0
    property int pillClickTries: 0
    property bool pillCleaning: false
    property var pillBefore: [1, 1]
    property var pillResults: ({})
    // The pane must be SHOWING the case's roster before a pill's centre means
    // anything: while the tab is hidden its cards are unpositioned (their items
    // report (0,0)), so a click at that point lands on whatever else is there.
    // Returns true once the pane is up on the buses — the caller only clicks a
    // pill after that.
    function pillOpenPane() {
        SelfTest.activate()
        const pane = SelfTest.findItem("selfTestMediaPane")
        if (!pane)
            return false
        if (!pane.visible) {
            window.pillSetupTries++
            if (window.pillSetupTries <= 2) {
                SelfTest.clickItem("selfTestTab_media")       // the real path
            } else {
                // The click never took (the dock tab is off the laid-out row on
                // this geometry): set the tab bar's own state — the same value
                // the click writes — and say which path opened the pane.
                const bar = SelfTest.findItem("selfTestLibraryTabBar")
                const tabs = bar && bar.tabs ? bar.tabs : []
                for (let i = 0; i < tabs.length; ++i)
                    if (tabs[i].pane === "media") { bar.currentTab = i; break }
                window.pillSetupBy = "currentTab"
            }
            return false
        }
        pane.selection = "inputs"
        return true
    }
    function pillModelName() { return window.pillCases[window.pillCase].name }
    function pillGain(side) { return BusListModel.channelGain(window.pillRow, side) }
    function pillSet(side, gain) { BusListModel.setChannelGain(window.pillRow, side, gain) }
    function pillItemName(side) {
        return "selfTestBusPill_" + window.pillRow + "_" + side
    }
    // One line per case, then the trims go back where they were.
    function pillReadCase() {
        const own = window.pillGain(0)
        const other = window.pillGain(1)
        const closed = Db.isChannelOff(own)
        const otherOpen = !Db.isChannelOff(other)
        console.log("[SELFTEST-PILL] " + window.pillModelName()
                    + " side0=" + own.toFixed(2) + " side1=" + other.toFixed(2)
                    + " closedOwnSide=" + closed + " otherSideOpen=" + otherOpen
                    + " by=" + window.pillBy)
        window.pillResults[window.pillModelName()] = closed && otherOpen
        window.pillSet(0, window.pillBefore[0])
        window.pillSet(1, window.pillBefore[1])
    }
    // Waits out boot AND the rosters' own restore (which retries on the event loop
    // and would otherwise land between the add and the cleanup, leaving the row
    // indices pointing at the user's own rows).
    Timer {
        id: selfTestPillBoot
        running: typeof SelfTestPill !== "undefined" && SelfTestPill
        interval: 15000
        onTriggered: selfTestPill.start()
    }
    Timer {
        id: selfTestPill
        running: false
        interval: 2500          // a click, then its deferred model write, per tick
        repeat: true
        onTriggered: {
            if (window.pillCase >= window.pillCases.length) {
                // Leave the rosters as the scenario found them, and let the model's
                // debounced roster write (400 ms) land BEFORE quitting: quitting
                // first left a removed row in settings.json, which is how a
                // "SelfTest Pill Media" row survived in the user's own roster. A
                // row is removed only if it is still the row this scenario ADDED —
                // the index is checked against its own name first, so a shifted
                // roster can never cost the user a bus or a source.
                if (!window.pillCleaning) {
                    if (window.pillAddedVideo >= 0
                            && VideoSourceListModel.getSource(window.pillAddedVideo).name
                               === "SelfTest Pill Media")
                        VideoSourceListModel.removeSource(window.pillAddedVideo)
                    if (window.pillAddedBus >= 0
                            && BusListModel.getBus(window.pillAddedBus).name === "SelfTest Pill Bus")
                        BusListModel.removeBus(window.pillAddedBus)
                    window.pillCleaning = true
                    return
                }
                selfTestPill.stop()
                let verdict = ""
                for (let i = 0; i < window.pillCases.length; ++i) {
                    const n = window.pillCases[i].name
                    verdict += (verdict === "" ? "" : " ") + n
                               + "=" + (window.pillResults[n] === true ? "PASS" : "FAIL")
                }
                console.log("[SELFTEST-PILL] result " + verdict)
                SelfTest.quit()
                return
            }
            const c = window.pillCases[window.pillCase]
            if (window.pillStep === 0) {
                if (!SelfTest.findItem("selfTestMediaPane")) {
                    console.log("[SELFTEST-PILL] pane=missing")
                    window.pillResults[c.name] = false
                    window.pillCase = window.pillCases.length
                    return
                }
                if (!window.pillOpenPane())
                    return              // next tick: the pane is coming up
                // The pane's Inputs section is the BUSES (the video source and audio
                // input rosters — and with them those cards' pill pairs — live on the
                // Settings - Audio & Video board), so the bus card is the scenario.
                if (BusListModel.rowCount() === 0) {
                    BusListModel.addBus("SelfTest Pill Bus", "audio")
                    window.pillAddedBus = BusListModel.rowCount() - 1
                }
                window.pillRow = BusListModel.rowCount() - 1
                // THE CARD'S OWN GESTURE, driven too: the monitor click is what
                // starts the meter taps (the pair's glow) and the video preview,
                // and it is the path that silently threw "meterAudio is not
                // defined" while the card just looked inert. Opened and released
                // again here (both synchronous) so the taps do not outlive the
                // probe; the report names which bus it took, how many taps, and
                // whether the release closed it.
                {
                    const pane = SelfTest.findItem("selfTestMediaPane")
                    if (pane) {
                        try {
                            pane.toggleBusMonitorPreview(window.pillRow)
                            const opened = pane.busMonitorAudioBus === window.pillRow
                            const taps = pane.meteredLabels.length
                            pane.toggleBusMonitorPreview(window.pillRow)
                            console.log("[SELFTEST-PILL] busmon bus=" + window.pillRow
                                        + " opened=" + opened + " audioTaps=" + taps
                                        + " released=" + (pane.busMonitorAudioBus === -1))
                        } catch (e) {
                            console.log("[SELFTEST-PILL] busmon bus=" + window.pillRow
                                        + " threw: " + e)
                        }
                    }
                }
                // The pill must be LAID OUT before its centre is a real point on
                // it: an unpositioned pill reports (0,0) and the click would land
                // on the first card's corner instead.
                if (!SelfTest.itemVisible(window.pillItemName(0)))
                    return
                window.pillBefore = [window.pillGain(0), window.pillGain(1)]
                // Both sides open to begin with: "clicking L closes L" needs L open.
                window.pillSet(0, 1.0)
                window.pillSet(1, 1.0)
                window.pillBy = ""
                window.pillClickTries = 0
                window.pillSetupTries = 0
                window.pillStep = 1
                return
            }
            if (window.pillStep === 1) {
                if (!SelfTest.itemVisible(window.pillItemName(0))) {
                    // The card scrolled away or the roster changed under us: go
                    // back to setup rather than clicking at a stale point.
                    window.pillSetupTries = 0
                    window.pillStep = 0
                    return
                }
                // The driver raises the window first, so this is a REAL click
                // delivered to this control (see SelfTestDriver::activate).
                SelfTest.activate()
                SelfTest.clickItem(window.pillItemName(0))
                window.pillBy = "click"
                window.pillClickTries++
                window.pillStep = 2
                return
            }
            if (window.pillStep === 2) {
                if (Db.isChannelOff(window.pillGain(0))) {   // the click landed
                    window.pillReadCase()
                    window.pillStep = 0
                    window.pillCase++
                    return
                }
                if (window.pillClickTries < 3) {
                    // A click that did not move the model gets another go before
                    // the fallback: the first one can still be swallowed while
                    // the window is coming to the foreground.
                    window.pillStep = 1
                    return
                }
                if (window.pillBy === "click") {
                    // Every click missed even though the window was foreground and
                    // the pill was on screen (three tries) — emit the pill's OWN
                    // toggled(), the very handler its MouseArea calls, and say so
                    // in the report, so a silent miss can never read as a pass.
                    const pill = SelfTest.findItem(window.pillItemName(0))
                    window.pillBy = "toggled()"
                    if (pill) {
                        try { pill.toggled() }
                        catch (e) { window.pillBy = "toggled() failed: " + e }
                    } else {
                        window.pillBy = "pillMissing"
                    }
                    window.pillStep = 3
                    return
                }
                window.pillReadCase()       // nothing moved at all: it fails
                window.pillStep = 0
                window.pillCase++
                return
            }
            window.pillReadCase()           // the fallback's reading
            window.pillStep = 0
            window.pillCase++
        }
    }

    // ---- "New bus" needs its source (VGR_NEWBUS_TEST=1) ----------------------
    // The rule the dialog exists for: a bus is created WITH the source the
    // operator picked, so Create can never leave behind a bus that feeds nothing
    // (a card that can never light, and a signal path nothing should open a
    // device for). This drives the real button and the real cards: open
    // Media · Inputs, click the New bus FAB, click Create with NOTHING picked (the
    // roster must not grow), click the first source card, click Create (a bus must
    // appear with its route already made and NO tap opened), grab the dialog for
    // the design review, and remove the row again.
    property int newBusStep: 0
    property int newBusTries: 0
    property int newBusBefore: 0
    property int newBusRow: -1
    property string newBusKind: ""
    property int newBusPickRow: -1
    property bool newBusBlocked: false
    property bool newBusCreated: false
    property bool newBusRouted: false
    property bool newBusTapsClean: true
    // The lights and the cancel path (the two things a passing Create flow does
    // NOT prove — see the notes where each is read).
    property int newBusLightChecked: 0
    property int newBusLightWrong: 0
    property bool newBusCancelClosed: false

    function newBusFinish() {
        selfTestNewBusTick.stop()
        // Only the row THIS run added, and only while it still carries the marker
        // name — a shifted roster can never cost the user a bus.
        if (window.newBusRow >= 0
                && BusListModel.getBus(window.newBusRow).name === "SelfTest NewBus Bus")
            BusListModel.removeBus(window.newBusRow)
        const ok = window.newBusBlocked && window.newBusCreated
                   && window.newBusRouted && window.newBusTapsClean
                   && window.newBusLightChecked > 0 && window.newBusLightWrong === 0
                   && window.newBusCancelClosed
                   && window.newBusEditPicker && window.newBusEditCancelClosed
        console.log("[SELFTEST-NEWBUS] blockedEmpty=" + window.newBusBlocked
                    + " created=" + window.newBusCreated
                    + " routed=" + window.newBusRouted
                    + " noTapOpened=" + window.newBusTapsClean
                    + " lights=" + (window.newBusLightChecked - window.newBusLightWrong)
                    + "/" + window.newBusLightChecked
                    + " cancelClosed=" + window.newBusCancelClosed
                    + " editRing=" + window.newBusEditPicker + "/" + window.newBusEditCancelClosed
                    + " pick=" + window.newBusKind + "#" + window.newBusPickRow)
        console.log("[SELFTEST-NEWBUS] result " + (ok ? "PASS" : "FAIL"))
        SelfTest.quit()
    }

    // ---- what each SOURCE CARD's light claims ------------------------------
    // Read off the card itself (`lightState`, the property its colour is drawn
    // from) and checked against the very probe that answers it
    // (LiveOutputService.inputAvailable/inputDelivery — the same call the bus
    // cards in the pane use). A click-through flow passes with every card
    // painted one flat colour; this is the half that says the colour MEANS
    // something. "The probe light is not showing correct state" is exactly a
    // card disagreeing with its own probe.
    function newBusCheckLights() {
        let state = ""
        window.newBusLightChecked = 0
        window.newBusLightWrong = 0
        const read = (kind, row, label, sourceKind, name) => {
            const card = SelfTest.findItem("newBusPicker_row_" + kind + "_" + row)
            const light = card ? String(card.lightState) : "missing"
            const alive = LiveOutputService.inputAvailable(label, sourceKind)
            const starved = LiveOutputService.inputDelivery(label, sourceKind) === "starved"
            const wanted = !alive ? "offline" : (starved ? "starved" : "connected")
            window.newBusLightChecked++
            if (light !== wanted)
                window.newBusLightWrong++
            state += (state === "" ? "" : " ") + name + "=" + light
                     + (light === wanted ? "" : "(wanted " + wanted + ")")
        }
        for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
            const v = VideoSourceListModel.getSource(i)
            const kind = v.kind !== undefined ? String(v.kind) : ""
            if (["camera", "screen", "ndi"].indexOf(kind) < 0)
                continue
            read("video", i, v.sublabel !== undefined ? String(v.sublabel).trim() : "",
                 kind, String(v.name !== undefined ? v.name : kind).replace(/ /g, "_"))
        }
        for (let i = 0; i < AudioInputListModel.rowCount(); ++i) {
            const a = AudioInputListModel.getInput(i)
            read("audio", i, a.sublabel !== undefined ? String(a.sublabel).trim() : "",
                 "audio", String(a.name !== undefined ? a.name : "audio").replace(/ /g, "_"))
        }
        console.log("[SELFTEST-NEWBUS] lights " + state
                    + " mismatched=" + window.newBusLightWrong
                    + "/" + window.newBusLightChecked)
    }

    // The pane must be SHOWING its Inputs section before the FAB's centre means
    // anything (the same reason the pill scenario opens it first).
    function newBusOpenPane() {
        SelfTest.activate()
        const pane = SelfTest.findItem("selfTestMediaPane")
        if (!pane)
            return false
        if (!pane.visible) {
            window.newBusTries++
            if (window.newBusTries <= 2) {
                SelfTest.clickItem("selfTestTab_media")
            } else {
                const bar = SelfTest.findItem("selfTestLibraryTabBar")
                const tabs = bar && bar.tabs ? bar.tabs : []
                for (let i = 0; i < tabs.length; ++i)
                    if (tabs[i].pane === "media") { bar.currentTab = i; break }
            }
            return false
        }
        window.newBusTries = 0
        pane.selection = "inputs"
        return true
    }

    // The first source card the dialog can offer, as { kind, row } — the rosters
    // the dialog itself reads (a source with no device named is skipped: it has
    // nothing for the tap to resolve).
    function newBusFirstPick() {
        for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
            const v = VideoSourceListModel.getSource(i)
            const kind = v.kind !== undefined ? String(v.kind) : ""
            if (["camera", "screen", "ndi"].indexOf(kind) >= 0
                    && v.sublabel !== undefined && String(v.sublabel).trim() !== "")
                return { kind: "video", row: i }
        }
        for (let i = 0; i < AudioInputListModel.rowCount(); ++i) {
            const a = AudioInputListModel.getInput(i)
            if (a.sublabel !== undefined && String(a.sublabel).trim() !== "")
                return { kind: "audio", row: i }
        }
        return null
    }

    function newBusAdvance() {
        if (window.newBusStep === 0) {
            if (!SelfTest.findItem("selfTestMediaPane")) {
                console.log("[SELFTEST-NEWBUS] pane=missing")
                window.newBusStep = 99
                window.newBusFinish()
                return
            }
            if (!window.newBusOpenPane())
                return
            window.newBusBefore = BusListModel.rowCount()
            window.newBusStep = 1
            return
        }
        if (window.newBusStep === 1) {
            if (!SelfTest.itemVisible("selfTestBusNew"))
                return
            SelfTest.clickItem("selfTestBusNew")     // the real button
            window.newBusStep = 2
            return
        }
        if (window.newBusStep === 2) {
            // The dialog's card is what the click is measured on; the fade is
            // 130 ms, so a tick is plenty.
            if (!SelfTest.itemVisible("newBusPicker_card")) {
                window.newBusTries++
                if (window.newBusTries < 3)
                    return
                console.log("[SELFTEST-NEWBUS] dialog=missing")
                window.newBusFinish()
                return
            }
            window.newBusTries = 0
            SelfTest.grab("newBusPicker_card", "newbus_dialog.png")
            console.log("[SELFTEST-NEWBUS] dialog=open grabbed=newbus_dialog.png")
            window.newBusCheckLights()
            window.newBusStep = 3
            return
        }
        if (window.newBusStep === 3) {
            // NOTHING picked: Create must make nothing, and say why.
            SelfTest.clickItem("newBusPicker_create")
            window.newBusStep = 4
            return
        }
        if (window.newBusStep === 4) {
            window.newBusBlocked = BusListModel.rowCount() === window.newBusBefore
            const problem = SelfTest.findItem("newBusPicker_problem")
            console.log("[SELFTEST-NEWBUS] createWithNoPick rows=" + BusListModel.rowCount()
                        + " (before " + window.newBusBefore + ")"
                        + " saidWhy=" + (problem !== null && problem.visible))
            const pick = window.newBusFirstPick()
            if (!pick) {
                console.log("[SELFTEST-NEWBUS] no source to pick")
                window.newBusFinish()
                return
            }
            window.newBusKind = pick.kind
            window.newBusPickRow = pick.row
            const name = "newBusPicker_row_" + pick.kind + "_" + pick.row
            if (!SelfTest.itemVisible(name)) {
                // Diagnose BEFORE failing: the name is carried by BOTH instances
                // of this picker (the Media pane's and the Edit screen's), so a
                // resolution that lands on the other instance's card reports the
                // wrong thing — and "not laid out" would be the wrong sentence
                // for it. No duplicate names, no ambiguity: see the objectName
                // in BusSourceModal (the name follows the MODE, and only one
                // caller ever opens the create mode at a time... except that the
                // Edit instance is built at boot in create mode too).
                const c = SelfTest.findItem(name)
                console.log("[SELFTEST-NEWBUS] pick card probe " + name
                            + " found=" + (c !== null)
                            + " visible=" + (c !== null ? c.visible : "-")
                            + " w=" + (c !== null ? c.width : "-")
                            + " h=" + (c !== null ? c.height : "-")
                            + " itemVisible=" + SelfTest.itemVisible(name))
                window.newBusFinish()
                return
            }
            SelfTest.clickItem(name)
            window.newBusStep = 5
            return
        }
        if (window.newBusStep === 5) {
            const create = SelfTest.findItem("newBusPicker_create")
            console.log("[SELFTEST-NEWBUS] createLive=" + (create !== null && create.opacity === 1)
                        + " saidWhy=" + (SelfTest.findItem("newBusPicker_problem") !== null
                                          && SelfTest.findItem("newBusPicker_problem").visible))
            SelfTest.grab("newBusPicker_card", "newbus_dialog_picked.png")
            SelfTest.clickItem("newBusPicker_create")
            window.newBusStep = 6
            return
        }
        // ---- the created bus, read back out of the model ----------------
        if (BusListModel.rowCount() !== window.newBusBefore + 1) {
            console.log("[SELFTEST-NEWBUS] create=" + (BusListModel.rowCount() - window.newBusBefore)
                        + " row(s) (expected 1)")
            window.newBusFinish()
            return
        }
        window.newBusRow = BusListModel.rowCount() - 1
        window.newBusCreated = true
        const bus = BusListModel.getBus(window.newBusRow)
        window.newBusRouted = window.newBusKind === "audio"
            ? (bus.routedAudioInputs && bus.routedAudioInputs.length > 0)
            : (bus.routedVideoSources && bus.routedVideoSources.length > 0)
        // The drain this dialog exists to avoid: creating a bus must not open a
        // device. No card preview, no output take, no meter — the route is a
        // graph edge, nothing more.
        const pane = SelfTest.findItem("selfTestMediaPane")
        const meters = pane && pane.meteredLabels ? pane.meteredLabels.length : 0
        window.newBusTapsClean = meters === 0 && LiveOutputService.inputLabel === ""
        console.log("[SELFTEST-NEWBUS] bus=" + window.newBusRow + " type=" + bus.type
                    + " name=\"" + bus.name + "\""
                    + " routed=" + window.newBusRouted
                    + " taps=" + meters + " outputTake=\"" + LiveOutputService.inputLabel + "\"")
        BusListModel.renameBus(window.newBusRow, "SelfTest NewBus Bus")
        window.newBusStep = 7
        return
    }

    // ---- the picker, cancelled -------------------------------------------
    // Steps 8-10: open it again and click Cancel. This is the half a successful
    // Create does not cover: the modal reports `cancelled()` and does NOT close
    // itself (its other caller binds `open` to its own flag), so a caller that
    // forgot the handler left the picker on screen with Cancel, Esc and the scrim
    // all doing nothing — "the buttons are not working". Esc goes through the
    // same `cancelled()` (Hotkeys), so this covers both key and button.
    function newBusCancelPhase() {
        if (window.newBusStep === 8) {
            if (window.newBusRow >= 0)
                return                      // wait for the cleanup tick to finish
            if (!SelfTest.itemVisible("selfTestBusNew"))
                return
            SelfTest.clickItem("selfTestBusNew")
            window.newBusStep = 9
            return
        }
        if (window.newBusStep === 9) {
            if (!SelfTest.itemVisible("newBusPicker_card"))
                return
            SelfTest.clickItem("newBusPicker_cancel")
            window.newBusStep = 10
            return
        }
        // Open again must be CLOSED again, and the roster untouched.
        window.newBusCancelClosed = !SelfTest.itemVisible("newBusPicker_card")
                           && BusListModel.rowCount() === window.newBusBefore
        console.log("[SELFTEST-NEWBUS] cancel closed=" + window.newBusCancelClosed
                    + " buses=" + BusListModel.rowCount()
                    + " (before " + window.newBusBefore + ")")
        window.newBusStep = 11           // then the EDIT ring's own door
    }

    // ---- the Edit + ring's Bus chip uses the SAME picker -------------------
    // The ring's Bus chip used to open a list of EXISTING buses, so a slide could
    // carry a bus that feeds nothing (a window painting a hole). It opens the very
    // picker Media › Inputs' "New bus" uses now, in its create mode: pick the
    // SOURCE, the bus is made with that route already in it. This phase proves the
    // WIRING (the chip reaches that picker, in that mode, and Cancel closes it)
    // without creating anything, because creating here would add a canvas item to
    // whatever show the operator has open.
    property bool newBusEditPicker: false
    property bool newBusEditCancelClosed: false

    function newBusEditPhase() {
        if (window.newBusStep === 11) {
            window.currentView = "edit"
            window.newBusStep = 12
            return
        }
        if (window.newBusStep === 12) {
            if (!SelfTest.itemVisible("selfTestAddChip"))
                return                    // the Edit screen is still coming up
            SelfTest.clickItem("selfTestAddChip")       // the + ring
            window.newBusStep = 13
            return
        }
        if (window.newBusStep === 13) {
            if (!SelfTest.itemVisible("selfTestAddKindChip_bus"))
                return                    // the menu opens on its own fade
            SelfTest.clickItem("selfTestAddKindChip_bus")
            window.newBusStep = 14
            return
        }
        if (window.newBusStep === 14) {
            // The picker that must appear is the CREATE one: its card, and at
            // least one SOURCE card with a light. A bus-mode picker would show
            // bus tiles instead (they carry no such name).
            const pick = window.newBusFirstPick()
            // The EDIT instance's own hooks (see the modal's hookPrefix note): the
            // Media pane's picker is closed by now but still in the tree, and both
            // are create-mode instances of the same component.
            const sourceCard = pick
                ? "editBusPicker_row_" + pick.kind + "_" + pick.row : ""
            window.newBusEditPicker = SelfTest.itemVisible("editBusPicker_card")
                                      && sourceCard !== ""
                                      && SelfTest.itemVisible(sourceCard)
            console.log("[SELFTEST-NEWBUS] editRing reachPicker=" + window.newBusEditPicker
                        + " sourceCard=\"" + sourceCard + "\"")
            SelfTest.clickItem("editBusPicker_cancel")
            window.newBusStep = 15
            return
        }
        window.newBusEditCancelClosed = !SelfTest.itemVisible("editBusPicker_card")
                                        && BusListModel.rowCount() === window.newBusBefore
        console.log("[SELFTEST-NEWBUS] editRing cancelClosed=" + window.newBusEditCancelClosed
                    + " buses=" + BusListModel.rowCount())
        window.newBusFinish()
    }

    Timer {
        id: selfTestNewBus
        running: typeof SelfTestNewBus !== "undefined" && SelfTestNewBus
        interval: 8000          // boot, then the rosters' own restore
        onTriggered: selfTestNewBusTick.start()
    }
    Timer {
        id: selfTestNewBusTick
        interval: 900           // one click, then read it back, per tick
        repeat: true
        onTriggered: {
            if (window.newBusStep === 7) {
                // The rename landed: take the row back out, then go and prove the
                // picker CLOSES (see newBusCancelPhase).
                if (window.newBusRow >= 0
                        && BusListModel.getBus(window.newBusRow).name === "SelfTest NewBus Bus")
                    BusListModel.removeBus(window.newBusRow)
                window.newBusRow = -1
                window.newBusStep = 8
                return
            }
            if (window.newBusStep >= 11) {
                window.newBusEditPhase()
                return
            }
            if (window.newBusStep >= 8) {
                window.newBusCancelPhase()
                return
            }
            window.newBusAdvance()
        }
    }

    // ---- Bus status light (VGR_BUSLIGHT_TEST=1) ------------------------------
    // The report, verbatim: "i thought i said we should give the status light
    // access to check if [the] device in the source is bringing data … so i
    // expect to see all green". Two things can pin that light amber while every
    // device is present: a roster that never carried the source's device (the
    // used-device sweep is TARGETED — a source whose label never reached it is
    // invisible to every reader), and a binding with nothing to re-evaluate it
    // when the sweep that does carry it lands (Q_INVOKABLE row lookups are not
    // tracked by QML, so the light kept whatever answer it got at construction).
    // This drives the real pane on the real roster: it opens Media · Inputs,
    // waits for the light's own ping + re-enumeration to settle, then reads each
    // bus card's own `busStatus` (the property its colour is drawn from) and
    // prints the routing behind it. Verdict = the user's sentence: every bus
    // with a routed source reads "connected".
    property int busLightStep: 0
    property int busLightTries: 0
    property var busLightLines: []
    property int busLightGreen: 0
    property int busLightRouted: 0
    // The source this run HOLDS a real tap on (see the delivery phase): one of the
    // buses' own video sources, taken with the card's own monitor gesture and
    // released before the run ends. Candidates are collected while the cards are
    // read (cameras first) — a hold that does not take moves to the next one.
    property var busLightHold: ({ bus: -1, label: "", kind: "" })
    property var busLightCandidates: []
    property string busLightDelivery: ""
    // ...and the AUDIO half of the same reading (see the audio hold): an
    // audio-only bus's monitor gesture takes its input meters, which is a tap
    // that really does deliver levels — the positive case this run needs, since a
    // camera pointed at a wall may honestly deliver nothing but a meter always
    // hands over data (silence included).
    property var busLightAudioHold: ({ bus: -1, label: "" })
    property string busLightAudioDelivery: ""
    property string busLightAudioStatus: ""
    // The card's own status AT THE MOMENT the delivery was read, so the two can
    // be compared as one fact (see busLightFinish).
    property string busLightHoldStatus: ""
    function busLightFinish(err) {
        selfTestBusLight.stop()
        for (let i = 0; i < window.busLightLines.length; ++i)
            console.log("[SELFTEST-BUSLIGHT] " + window.busLightLines[i])
        const videoDelivery = window.busLightHold.bus < 0 ? "n/a"
            : (window.busLightDelivery === "" ? "<none>" : window.busLightDelivery)
        const audioDelivery = window.busLightAudioHold.bus < 0 ? "n/a"
            : (window.busLightAudioDelivery === "" ? "<none>" : window.busLightAudioDelivery)
        // THE LIGHT'S OWN ANSWER, checked against what the taps say. The bridge
        // answering correctly is one fact; the CARD actually consuming that answer
        // is a different one, and it is the one the user sees. A delivery of
        // "starved" has to paint the card amber; "ok" — and "" (nobody is
        // watching that tap, so there is no live evidence) — leave the presence
        // answer standing, which is what keeps an unwatched bus green.
        const videoWanted = window.busLightDelivery === "starved" ? "starved" : "connected"
        const videoConsistent = window.busLightHold.bus < 0
            || window.busLightHoldStatus === videoWanted
        // The POSITIVE case: a meter tap that is handing data over must read ok,
        // and its card must read connected. Without this the run could pass with
        // every light amber off taps that never delivered anything.
        const audioOk = window.busLightAudioHold.bus >= 0
            && window.busLightAudioDelivery === "ok"
            && window.busLightAudioStatus === "connected"
        const ok = err === "" && window.busLightRouted > 0
                   && window.busLightGreen === window.busLightRouted
                   && videoConsistent && audioOk
        console.log("[SELFTEST-BUSLIGHT] result routed=" + window.busLightRouted
                    + " green=" + window.busLightGreen
                    + " delivery=" + videoDelivery
                    + " heldBus=" + window.busLightHold.bus
                    + " heldStatus=" + (window.busLightHoldStatus === "" ? "<none>" : window.busLightHoldStatus)
                    + " audio=" + audioDelivery
                    + " audioBus=" + window.busLightAudioHold.bus
                    + " audioStatus=" + (window.busLightAudioStatus === "" ? "<none>" : window.busLightAudioStatus)
                    + " busStatus=" + (ok ? "PASS" : "FAIL")
                    + " buses=" + BusListModel.rowCount() + " err=" + err)
        SelfTest.quit()
    }
    Timer {
        id: selfTestBusLightBoot
        running: typeof SelfTestBusLight !== "undefined" && SelfTestBusLight
        interval: 15000         // boot, then the rosters' own restore
        onTriggered: selfTestBusLight.start()
    }
    Timer {
        id: selfTestBusLight
        running: false
        interval: 2500
        repeat: true
        onTriggered: {
            if (window.busLightStep === 0) {
                SelfTest.activate()
                const pane = SelfTest.findItem("selfTestMediaPane")
                if (!pane) {
                    if (++window.busLightTries > 8) {
                        window.busLightFinish("pane-missing")
                        return
                    }
                    return
                }
                if (!pane.visible) {
                    // The real path first (the dock tab's click), then the tab
                    // bar's own state if the click never took — same two-step the
                    // pill scenario uses, so the pane comes up either way.
                    SelfTest.clickItem("selfTestTab_media")
                    const bar = SelfTest.findItem("selfTestLibraryTabBar")
                    const tabs = bar && bar.tabs ? bar.tabs : []
                    for (let i = 0; i < tabs.length; ++i)
                        if (tabs[i].pane === "media") { bar.currentTab = i; break }
                    return
                }
                pane.selection = "inputs"      // the BUSES
                window.busLightStep = 1
                window.busLightTries = 0
                return
            }
            // Two phases share the settle: the presence reading (step 1) and the
            // delivery reading (step 2, after a real tap has had longer than the
            // engine's warm-up grace to produce frames). Three ticks is ~7 s each.
            if (++window.busLightTries < 3)
                return
            window.busLightTries = 0
            if (window.busLightStep === 2) {
                window.busLightReadDelivery()
                return
            }
            // Camera candidates before screen/NDI ones: a camera delivers frames
            // regardless of what is on top of it, and this run needs a tap that
            // really feeds (see the hold below).
            window.busLightCandidates = []
            const n = BusListModel.rowCount()
            for (let i = 0; i < n; ++i) {
                const card = SelfTest.findItem("selfTestBusCard_" + i)
                if (!card) {
                    window.busLightLines.push("bus=" + i + " card=missing")
                    continue
                }
                const b = BusListModel.getBus(i)
                const name = (b && b.name !== undefined) ? String(b.name) : ("#" + i)
                const type = String(card.busType || "")
                const videos = (b && b.routedVideoSources !== undefined) ? b.routedVideoSources : []
                const audios = (b && b.routedAudioInputs !== undefined) ? b.routedAudioInputs : []
                const routedVideo = type === "audio" ? 0 : videos.length
                const routedAudio = type === "video" ? 0 : audios.length
                const routed = routedVideo + routedAudio
                const video = String(card.busVideoLabel || "")
                const vkind = card.busVideoSource && card.busVideoSource.kind !== undefined
                    ? String(card.busVideoSource.kind) : ""
                const audioLabel = routedAudio > 0
                    ? String((AudioInputListModel.getInput(audios[0]) || {}).sublabel || "") : ""
                const audioKind = routedAudio > 0
                    ? String((AudioInputListModel.getInput(audios[0]) || {}).kind || "") : ""
                const status = String(card.busStatus || "")
                if (routed > 0) window.busLightRouted++
                if (routed > 0 && status === "connected") window.busLightGreen++
                // The first bus that carries a video source is the one this run
                // HOLDS (see the delivery phase) — picked here, taken after the
                // whole roster has been read, so nothing changes under the reading.
                if (routedVideo > 0 && video !== "")
                    window.busLightCandidates.push({ bus: i, label: video, kind: vkind })
                window.busLightLines.push("bus=" + i + " name=\"" + name + "\" type=" + type
                    + " routed=" + routed + " status=" + status
                    + " video=\"" + video + "\"(" + vkind + ")"
                    + " videoAlive=" + (video !== ""
                        ? LiveOutputService.inputHealthy(video, vkind) : "n/a")
                    + " audio=\"" + audioLabel + "\"(" + audioKind + ")"
                    + " audioAlive=" + (routedAudio > 0
                        ? LiveOutputService.inputHealthy(audioLabel, "audio") : "n/a"))
            }
            // THE PING ITSELF — the half of this that the user asked for by name
            // ("give them access to ping"). Every bus above answered from the
            // rosters, which is the good case but proves nothing about a source
            // whose device the rosters do NOT carry. A label no device and no
            // window has must take the ping path: one diagnostic line, one queued
            // targeted sweep that names it, and a false answer. The runner reads
            // that line out of the engine log, so a light that can only ever
            // answer from a cache it never refreshes is caught here.
            const ghostKind = "screen"
            const ghost = "selftest-buslight-ghost-window"
            const ghostAlive = LiveOutputService.inputAvailable(ghost, ghostKind)
            // ...and the same label asked with the WRONG kind must still answer
            // from the roster that does know it (the kind-agnostic fallback): a
            // kind string that failed to travel must not pin a real source's
            // light amber for good.
            const bus1Video = String((SelfTest.findItem("selfTestBusCard_1") || {}).busVideoLabel || "")
            const wrongKindAlive = bus1Video !== ""
                ? LiveOutputService.inputHealthy(bus1Video, "window") : "n/a"
            window.busLightLines.push("ping label=\"" + ghost + "\" kind=" + ghostKind
                + " alive=" + ghostAlive)
            window.busLightLines.push("wrongkind label=\"" + bus1Video
                + "\" kind=window alive=" + wrongKindAlive)
            // ...and a PNG of the pane as it stands, because the report this
            // guards is a picture: the lights' colour and the middle of the card
            // being empty are both things a property dump cannot state. The grab
            // is ASYNCHRONOUS (QQuickItemGrabResult::ready -> saveToFile), so the
            // run waits one more tick before quitting — quitting in the same tick
            // dropped the file while still reporting the request as accepted,
            // which is exactly how a "no screenshot" can read as a pass.
            // ---- THE LIVE HALF, MEASURED ON A REAL TAP ----------------------
            // The delivery verdict is only ever about a tap the app holds, so the
            // run takes one: the chosen bus's video source, through the card's own
            // monitor gesture (the same call the operator's click makes). Phase 2
            // then waits PAST the engine's warm-up grace before reading it, so "ok"
            // can only be true because frames actually arrived — a tap that opened
            // and produced nothing reads "starved", and the light must not call it
            // connected. Nothing is left held: phase 2 releases it.
            const pane = SelfTest.findItem("selfTestMediaPane")
            if (pane) {
                // ENSURE, don't assume, and take a source that really feeds: the
                // pane holds ONE video monitor, a toggle that lands on the bus it is
                // already monitoring RELEASES it, and a window capture can refuse to
                // open at all (an occluded window) — so the candidates are tried in
                // order (cameras first: a camera delivers frames whatever is on top
                // of it) until the pane reports it is monitoring one. Whichever took
                // is the tap phase 2 reads.
                const wants = window.busLightCandidates.slice()
                wants.sort((a, b) => (a.kind === "camera" ? 0 : 1) - (b.kind === "camera" ? 0 : 1))
                for (let c = 0; c < wants.length; ++c) {
                    const cand = wants[c]
                    try {
                        pane.toggleBusMonitorPreview(cand.bus)
                        if (Number(pane.busMonitorVideoBus) !== cand.bus)
                            pane.toggleBusMonitorPreview(cand.bus)   // the pane's own undo
                        if (Number(pane.busMonitorVideoBus) === cand.bus) {
                            window.busLightHold = cand
                            window.busLightLines.push("held bus=" + cand.bus + " label=\""
                                + cand.label + "\" kind=" + cand.kind + " via=cardMonitor"
                                + " rev=" + LiveOutputService.inputRev)
                            break
                        }
                    } catch (e) {
                        window.busLightLines.push("holdattempt bus=" + cand.bus + " threw: " + e)
                    }
                }
                if (window.busLightHold.bus < 0)
                    window.busLightDelivery = "nothold"
                // THE AUDIO HALF, on the same footing: an AUDIO-ONLY bus's monitor
                // gesture takes that bus's input meters (the pane's own call), and
                // a meter hands over LEVELS — real data, silence included — so this
                // is the tap that can be relied on to answer "ok". A bus that also
                // carries video is skipped: the pane holds ONE video monitor, and
                // taking the audio bus's monitor would have to move it.
                for (let i = 0; i < n; ++i) {
                    const card = SelfTest.findItem("selfTestBusCard_" + i)
                    if (!card || String(card.busType || "") !== "audio")
                        continue
                    const b = BusListModel.getBus(i)
                    const rows = (b && b.routedAudioInputs !== undefined) ? b.routedAudioInputs : []
                    if (rows.length === 0)
                        continue
                    const data = AudioInputListModel.getInput(rows[0])
                    const label = data && data.sublabel !== undefined ? String(data.sublabel).trim() : ""
                    if (label === "")
                        continue
                    try {
                        pane.toggleBusMonitorPreview(i)
                        window.busLightAudioHold = { bus: i, label: label }
                        window.busLightLines.push("held audio bus=" + i + " label=\"" + label
                            + "\" via=busMonitor")
                    } catch (e) {
                        window.busLightLines.push("audiohold bus=" + i + " threw: " + e)
                    }
                    break
                }
            }
            // ONE reading, then the delivery phase: without this the whole
            // reading ran again on every tick — and its hold is a pane TOGGLE,
            // so a repeat took, released, took the same tap forever (the live
            // half could never be read and the run never ended).
            window.busLightStep = 2
        }
    }
    // Phase 2: read what the HELD tap is actually delivering, then put the tap back.
    function busLightReadDelivery() {
        const held = window.busLightHold
        if (held.bus < 0) {
            window.busLightFinishDelivery()
            return
        }
        if (window.busLightDelivery !== "") {
            // The hold never settled (see the ensure above): report that as the
            // verdict rather than reading a tap nobody holds.
            window.busLightLines.push("delivery label=\"" + held.label
                + "\" verdict=<none> hold=" + window.busLightDelivery)
            window.busLightFinishDelivery()
            return
        }
        const delivery = String(LiveOutputService.inputDelivery(held.label, held.kind))
        const card = SelfTest.findItem("selfTestBusCard_" + held.bus)
        const status = card ? String(card.busStatus || "") : ""
        window.busLightDelivery = delivery
        window.busLightHoldStatus = status
        // The tap's own bookkeeping is reported beside the verdict, so a failed
        // run says WHICH half was missing: no `devId` means the engine never
        // resolved the label to a device (nothing to deliver from), no
        // `cardPreviews` entry means the service let the hold go, and a held tap
        // with no verdict means the frames never stamped.
        const heldCards = LiveOutputService.cardPreviews.indexOf(held.label) >= 0
        const devId = String(EngineBridge.resolvedPreviewDeviceId(held.label) || "")
        // THE TWO OTHER HALVES OF THE SAME FACT, so a "starved" says WHICH one
        // broke: `producing` is the pane's own evidence that frames arrived (the
        // service's first-frame detection on the very pulls this light reads),
        // and `failure` is the platform's own reason for a tap that cannot
        // produce (a busy device, a type the tap cannot consume). A verdict that
        // disagrees with `producing` is the wiring bug this half exists to catch.
        const producing = LiveOutputService.inputProducing(held.label)
        const failure = String(LiveOutputService.inputFailure(held.label, held.kind) || "")
        window.busLightLines.push("delivery label=\"" + held.label + "\" kind=" + held.kind
            + " verdict=" + (delivery === "" ? "<none>" : delivery)
            + " producing=" + producing
            + " failure=\"" + failure + "\""
            + " cardPreviews=" + heldCards + " devId=\"" + devId + "\""
            // WHICH HALF OF THE TAP the verdict came from: a card whose state
            // window is not asking for frames (stateWin=false) is not being
            // watched, and its silence proves nothing — that is the difference
            // this line exists to make readable.
            + " stateWin=" + (card && card.stateWinImage
                              ? (card.stateWinImage.visible + "/status" + card.stateWinImage.status) : "n/a")
            + " producing=" + producing
            + " pulled=" + EngineBridge.previewPullAgeMs(held.label) + "ms"
            + " lastFrame=" + EngineBridge.previewDeliveryAgeMs(held.label) + "ms"
            + " heldBusStatus=" + (status === "" ? "<none>" : status))
        const pane = SelfTest.findItem("selfTestMediaPane")
        if (pane) {
            try { pane.toggleBusMonitorPreview(held.bus) }
            catch (e) { window.busLightLines.push("release threw: " + e) }
        }
        window.busLightLines.push("released bus=" + held.bus)
        // ...and the audio half, read and released the same way (see the audio
        // hold): its verdict is "ok" when the meter tap really is live, and the
        // card must be showing that as connected.
        const ahold = window.busLightAudioHold
        if (ahold.bus >= 0) {
            const agot = String(LiveOutputService.inputDelivery(ahold.label, "audio"))
            window.busLightAudioDelivery = agot
            const acard = SelfTest.findItem("selfTestBusCard_" + ahold.bus)
            window.busLightAudioStatus = acard ? String(acard.busStatus || "") : ""
            window.busLightLines.push("delivery audio label=\"" + ahold.label
                + "\" verdict=" + (agot === "" ? "<none>" : agot)
                + " busStatus=" + (window.busLightAudioStatus === ""
                                   ? "<none>" : window.busLightAudioStatus))
            const apane = SelfTest.findItem("selfTestMediaPane")
            if (apane) {
                try { apane.toggleBusMonitorPreview(ahold.bus) }
                catch (e) { window.busLightLines.push("audio release threw: " + e) }
            }
            window.busLightLines.push("released audio bus=" + ahold.bus)
        } else {
            window.busLightAudioDelivery = "n/a"
            window.busLightLines.push("audio half skipped — no audio-only bus with a routed input")
        }
        window.busLightFinishDelivery()
    }
    function busLightFinishDelivery() {
        // The PNG LAST, with the tap released: what the operator's pane looks like
        // once nothing is being pulled (and with the middles empty).
        window.busLightLines.push("grab bus_light_pane.png requested="
            + SelfTest.grab("selfTestMediaPane", "bus_light_pane.png"))
        window.busLightStep = 3
        selfTestBusLight.stop()   // one reading per run, from here on
    }
    Timer {
        id: selfTestBusLightGrabTail
        running: typeof SelfTestBusLight !== "undefined" && SelfTestBusLight
                    && window.busLightStep === 3
        interval: 2500         // the grab's ready signal, then its save
        onTriggered: window.busLightFinish("")
    }

    // ---- Media rows' own audio meters (VGR_MEDIAMETER_TEST=1) ----------------
    // The report: a media row's level bar on Settings · Audio & Video "is still
    // stuck", and it "shows the opposite in edit" — the bar on the card drew the
    // row's STORED volume (a setting: a fixed fill that tracked the Volume
    // slider and never moved), while the Edit dialog for the SAME row draws the
    // bridge's stream tap (the actual audio — a flat -inf dB strip when the file
    // is gone, which is the roster this was reported from). One row, two
    // readings that could never agree.
    //
    // This measures both halves through the app's own UI, with the user's own
    // case in it: it adds a media row pointed at a REAL audio file and one
    // pointed at a file that is not there, opens Settings · Audio & Video, and
    // reads each row's LevelTrack `value` — the number its fill is drawn from —
    // across several meter-pump ticks.
    //   * the LIVE row's bar must MOVE (nonzero, off the stream tap)
    //   * the SILENT row's bar must read 0 even though its stored level is 4
    //     (≈53% of the bar's own scale — the fill the stored-volume binding
    //     drew, frozen, which is the reported screenshot)
    //   * and each bar must match what the DIALOG side reports for that row,
    //     read off the screen object the Edit dialog's strips bind, so
    //     "the card agrees with the dialog" is measured, not assumed.
    // Both rows are removed again, and the removal gets its own tick to land in
    // settings.json before quitting (the roster write is debounced) — the lesson
    // the pill scenario learned the hard way, when a leftover row survived in
    // the user's own roster.
    property int mediaMeterPhase: 0
    property int mediaMeterLiveRow: -1
    property int mediaMeterSilentRow: -1
    property real mediaMeterLiveMax: 0
    property real mediaMeterSilentMax: 0
    property real mediaMeterLiveDialogMax: 0
    property real mediaMeterSilentDialogMax: 0
    property bool mediaMeterDialogSeen: false
    property int mediaMeterSamples: 0
    property int mediaMeterFilePick: 0
    property string mediaMeterFile: ""
    property string mediaMeterEndFile: ""
    // A file that is NOT there, with the stored level the reported row carries.
    readonly property string mediaMeterMissingFile: "selftest-meter-missing.mp4"
    readonly property real mediaMeterMissingLevel: 4
    // Real files with real audio, longest first: Windows' own sounds, which
    // every machine has, and Media Foundation decodes a WAV with no codec pack.
    readonly property var mediaMeterFiles: [
        "C:/Windows/Media/Ring05.wav",
        "C:/Windows/Media/Ring08.wav",
        "C:/Windows/Media/Ring09.wav",
        "C:/Windows/Media/Alarm05.wav",
        "C:/Windows/Media/Alarm06.wav",
        "C:/Windows/Media/Alarm01.wav"
    ]
    // The runner's own pick first (it checks the file exists), then the list —
    // so the script may name any file and the scenario still runs on its own.
    function mediaMeterFileList() {
        const named = typeof SelfTestMediaMeterFile !== "undefined"
                      ? String(SelfTestMediaMeterFile || "").trim() : ""
        const out = []
        if (named !== "")
            out.push(named)
        for (let i = 0; i < window.mediaMeterFiles.length; ++i)
            if (out.indexOf(window.mediaMeterFiles[i]) < 0)
                out.push(window.mediaMeterFiles[i])
        return out
    }
    // The AudioVideoScreen that owns this board: the bar and the dialog's strips
    // must report the same row the same way, and the honest way to check that is
    // to read the DIALOG's own helper off the screen object rather than to
    // re-derive it here.
    function mediaMeterScreen() {
        let it = SelfTest.findItem("selfTestAvBoard")
        for (let hop = 0; it && hop < 6; ++hop) {
            if (typeof it.videoChannelLevels === "function")
                return it
            it = it.parent
        }
        return null
    }
    // The bar's own number (-1 while the item is not built yet), and the
    // dialog-side helper's version of it (null when the screen is unreachable).
    function mediaMeterBar(row) {
        const item = SelfTest.findItem("selfTestVideoRowMeter_" + row)
        return item ? Number(item.value) : -1
    }
    function mediaMeterDialog(row, sublabel, level) {
        const screen = window.mediaMeterScreen()
        if (!screen)
            return null
        const channels = screen.videoChannelLevels(row, sublabel, "media", false, level)
        return Math.max(channels[0], channels[1]) * 100
    }
    Timer {
        id: selfTestMediaMeterStart
        running: typeof SelfTestMediaMeter !== "undefined" && SelfTestMediaMeter
        // Boot AND the rosters' own restore, which retries on the event loop —
        // the same wait the pill scenario uses before it touches a roster.
        interval: 15000
        onTriggered: selfTestMediaMeter.start()
    }
    Timer {
        id: selfTestMediaMeter
        running: false
        interval: 700            // several samples inside one system sound's worth of audio
        repeat: true
        onTriggered: {
            const files = window.mediaMeterFileList()
            if (window.mediaMeterPhase === 0) {
                // Sweep rows an EARLIER run left behind before adding this
                // run's — this scenario adds and removes rows in the user's own
                // roster, so it must be idempotent: a run that hung (or was
                // killed mid-flight) never reached its cleanup, and three of
                // those otherwise leave six test rows on the board forever.
                // Matched by NAME, walking from the end down, so a removal can
                // never shift an index not yet looked at.
                for (let r = VideoSourceListModel.rowCount() - 1; r >= 0; --r) {
                    const n = VideoSourceListModel.getSource(r).name
                    if (n === "SelfTest Meter Live" || n === "SelfTest Meter Silent")
                        VideoSourceListModel.removeSource(r)
                }
                window.mediaMeterFile = files[0]
                window.mediaMeterEndFile = window.mediaMeterFile
                const before = VideoSourceListModel.rowCount()
                VideoSourceListModel.addSourceWith("SelfTest Meter Live", "media",
                                                   window.mediaMeterFile, 100, false)
                window.mediaMeterLiveRow = before
                VideoSourceListModel.addSourceWith("SelfTest Meter Silent", "media",
                                                   window.mediaMeterMissingFile,
                                                   window.mediaMeterMissingLevel, false)
                window.mediaMeterSilentRow = before + 1
                window.openSettings("av")
                window.mediaMeterPhase = 1
                return
            }
            if (window.mediaMeterPhase === 1) {
                // The bar exists only once the screen is up AND the board has the
                // new rows (modelsRev → the row roster rebuilds). An item that is
                // not there has no value to read yet.
                if (!SelfTest.findItem("selfTestAvBoard"))
                    return
                if (window.mediaMeterBar(window.mediaMeterLiveRow) < 0
                        || window.mediaMeterBar(window.mediaMeterSilentRow) < 0)
                    return
                console.log("[SELFTEST-MEDIAMETER] measuring file=" + window.mediaMeterFile
                            + " liveRow=" + window.mediaMeterLiveRow
                            + " silentRow=" + window.mediaMeterSilentRow)
                window.mediaMeterPhase = 2
                return
            }
            if (window.mediaMeterPhase === 2) {
                const live = window.mediaMeterBar(window.mediaMeterLiveRow)
                const silent = window.mediaMeterBar(window.mediaMeterSilentRow)
                if (live > window.mediaMeterLiveMax)
                    window.mediaMeterLiveMax = live
                if (silent > window.mediaMeterSilentMax)
                    window.mediaMeterSilentMax = silent
                const dLive = window.mediaMeterDialog(window.mediaMeterLiveRow,
                                                      window.mediaMeterFile, 100)
                const dSilent = window.mediaMeterDialog(window.mediaMeterSilentRow,
                                                        window.mediaMeterMissingFile,
                                                        window.mediaMeterMissingLevel)
                if (dLive !== null && dSilent !== null) {
                    window.mediaMeterDialogSeen = true
                    if (dLive > window.mediaMeterLiveDialogMax)
                        window.mediaMeterLiveDialogMax = dLive
                    if (dSilent > window.mediaMeterSilentDialogMax)
                        window.mediaMeterSilentDialogMax = dSilent
                }
                window.mediaMeterSamples++
                // A system sound ENDS and parks its tap (the PAL publishes a
                // zeroed window so meters read silence, not a freeze). Keep a
                // fresh decode running by moving the row to the next real file —
                // the label IS the tap key, so the screen re-points the stream
                // tap with it — so "the bar never got a chance to move" can never
                // be mistaken for a broken binding.
                if (window.mediaMeterLiveMax === 0 && window.mediaMeterSamples % 3 === 0
                        && window.mediaMeterSamples < 15) {
                    window.mediaMeterFilePick = (window.mediaMeterFilePick + 1) % files.length
                    window.mediaMeterFile = files[window.mediaMeterFilePick]
                    window.mediaMeterEndFile = window.mediaMeterFile
                    VideoSourceListModel.setSublabel(window.mediaMeterLiveRow,
                                                     window.mediaMeterFile)
                    console.log("[SELFTEST-MEDIAMETER] no level yet - moving the live row to "
                                + window.mediaMeterFile)
                }
                if (window.mediaMeterSamples < 18)
                    return
                window.mediaMeterPhase = 3
                return
            }
            if (window.mediaMeterPhase === 3) {
                const live = window.mediaMeterLiveMax > 0 ? "PASS" : "FAIL"
                // The reported case: a nonzero stored level beside silence. The
                // fill the OLD binding drew from it is printed too — the number
                // that made this read as "a stuck animation" rather than a meter.
                const silent = window.mediaMeterSilentMax === 0
                               && window.mediaMeterMissingLevel > 0 ? "PASS" : "FAIL"
                let agree = "SKIP"
                if (window.mediaMeterDialogSeen)
                    agree = (Math.abs(window.mediaMeterLiveMax - window.mediaMeterLiveDialogMax)
                                <= 0.5
                             && Math.abs(window.mediaMeterSilentMax
                                         - window.mediaMeterSilentDialogMax) <= 0.5)
                            ? "PASS" : "FAIL"
                console.log("[SELFTEST-MEDIAMETER] file=" + window.mediaMeterEndFile
                            + " samples=" + window.mediaMeterSamples
                            + " barLive=" + window.mediaMeterLiveMax.toFixed(2)
                            + " barSilent=" + window.mediaMeterSilentMax.toFixed(2)
                            + " dialogLive=" + window.mediaMeterLiveDialogMax.toFixed(2)
                            + " dialogSilent=" + window.mediaMeterSilentDialogMax.toFixed(2)
                            + " storedSilent=" + window.mediaMeterMissingLevel
                            + " oldFill="
                            + Math.round(Db.pct(window.mediaMeterMissingLevel) * 100) + "%")
                console.log("[SELFTEST-MEDIAMETER] result live=" + live
                            + " silent=" + silent + " agree=" + agree)
                window.mediaMeterPhase = 4
                return
            }
            if (window.mediaMeterPhase === 4) {
                // Removed only if the row still wears the scenario's own name, so
                // a shifted roster can never cost the user a source. The higher
                // index goes first: removing it must not shift the other one.
                if (window.mediaMeterSilentRow >= 0
                        && VideoSourceListModel.getSource(window.mediaMeterSilentRow).name
                           === "SelfTest Meter Silent")
                    VideoSourceListModel.removeSource(window.mediaMeterSilentRow)
                if (window.mediaMeterLiveRow >= 0
                        && VideoSourceListModel.getSource(window.mediaMeterLiveRow).name
                           === "SelfTest Meter Live")
                    VideoSourceListModel.removeSource(window.mediaMeterLiveRow)
                window.mediaMeterPhase = 5
                return
            }
            selfTestMediaMeter.stop()
            SelfTest.quit()
        }
    }

    // ---- click-to-open timing (VGR_OPEN_TEST=1) ------------------------------
    // The user report: picking a sermon in the app-wide search "takes time to
    // take me to open The Table". This scenario drives that exact path — open
    // the dialog, type a citation, pick the first result — and logs the cost of
    // each step, so a slow pick is measured instead of guessed at.
    property real openTestT0: 0
    Timer {
        id: selfTestOpenDialog
        running: typeof SelfTestOpen !== "undefined" && SelfTestOpen
        // Waits out boot (QML load is ~14s here) AND the background sermon
        // indexing (~35s after the event loop starts): before it finishes, the
        // sermon search can answer empty and the library lock is contended, so
        // an earlier query measures an indexing race instead of the real click.
        interval: 45000
        onTriggered: {
            const t0 = Date.now()
            quickSearch.openSearch()
            console.log("[SELFTEST-OPEN] dialog open=" + (Date.now() - t0) + "ms")
            selfTestOpenQuery.restart()
        }
    }
    // Re-asks until the sermon search answers: the background sermon indexing takes
    // ~40s after boot, and a query issued while it runs can come back empty — the
    // measurement must be the steady-state click, not an indexing race.
    property int openTestTries: 0
    Timer {
        id: selfTestOpenQuery
        interval: 2000
        repeat: true
        onTriggered: {
            const t0 = Date.now()
            quickSearch.query = "64-0617 54"
            quickSearch.refresh()
            window.openTestTries++
            console.log("[SELFTEST-OPEN] query try=" + window.openTestTries
                        + " refresh=" + (Date.now() - t0) + "ms")
            if (quickSearch.results.length > 0) {
                const r = quickSearch.results[0]
                selfTestOpenQuery.stop()
                console.log("[SELFTEST-OPEN] picking kind=" + r.kind + " verse=" + r.verse + " title=" + r.title)
                window.openTestT0 = Date.now()
                quickSearch.choose(r)
                selfTestOpenCheck.restart()
            } else if (window.openTestTries >= 20) {
                selfTestOpenQuery.stop()
                console.log("[SELFTEST-OPEN] no results after " + window.openTestTries + " tries")
                SelfTest.quit()
            }
        }
    }
    Timer {
        id: selfTestOpenCheck
        interval: 2500
        onTriggered: {
            const pane = SelfTest.findItem("selfTestTablePane")
            console.log("[SELFTEST-OPEN] state pane=" + (pane && pane.book ? pane.book.id : "-")
                        + ":" + (pane ? pane.chapterNumber : "-") + " verses=" + (pane ? pane.chapterVerses.length : -1)
                        + " selected=" + (pane ? JSON.stringify(pane.selected) : "[]")
                        + " total=" + (Date.now() - window.openTestT0) + "ms")
            SelfTest.quit()
        }
    }

    // ---- exact-word lock (VGR_EXACT_TEST=1) ---------------------------------
    // The padlock at the dialog field's far end promises "exact word search".
    // This scenario proves it in the RUNNING app: type a word, read the rows,
    // CLICK the lock, read them again — and check the invariant that defines the
    // feature: every sermon row the LOCKED search returns must carry the typed
    // word as a WHOLE word. Loose mode legitimately returns the word-extension
    // matches too ("friends" for "friend"), so the pair of counts is the proof
    // that the lock changed the answer rather than merely staying quiet.
    // Two probes, because one word can only prove half the feature:
    //   "frien"  — NOT a word: loose search finds the paragraphs that say "friend"/
    //              "friends" (substring/completion), the LOCK must answer fewer;
    //   "friend" — a real word: the LOCK must still find it, and every row it
    //              returns must carry it WHOLE.
    readonly property string exactProbeWord: "frien"
    readonly property string exactRealWord: "friend"
    property int exactFilteredLocked: -1
    property string exactToggleBy: ""
    function exactSermonRows() {
        return quickSearch.results.filter((r) => r.kind === "table")
    }
    // The sentence the lock promises: the query's words IN ORDER, next to each
    // other (only punctuation/whitespace between them), each appearing whole.
    // One word is just a whole word.
    function exactPhraseRe() {
        const words = String(quickSearch.query).trim().toLowerCase().split(/\s+/).filter((w) => w !== "")
        const esc = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        return new RegExp("\\b" + esc.join("[\\s,;:.!?“”'\\-—]+\\b") + "\\b", "i")
    }
    // The paragraph text a row stands for (the fetched paragraphs, not the capped
    // snippet), and how many of `rows` do NOT carry the query's sentence. Locked
    // mode must report 0 — that is the whole promise of the padlock.
    function exactRowText(r) {
        if (typeof TheTableService !== "undefined" && r.bookId !== undefined && Number(r.verse) > 0)
            return TheTableService.paragraphPair(r.bookId, r.chapter, r.verse, r.spanned === true ? 2 : 1)
        return r.text || ""
    }
    function exactViolations(rows) {
        const re = window.exactPhraseRe()
        let bad = 0
        for (const r of rows)
            if (!re.test(window.exactRowText(r)))
                bad++
        return bad
    }
    // The diagnostic tail of every log line: what the check actually looked for
    // and the first row's own text (so a surprising count is never a mystery).
    function exactDiag(rows) {
        const r0 = rows.length > 0 ? rows[0] : null
        return " q=" + JSON.stringify(quickSearch.query)
               + " re=" + window.exactPhraseRe().source
               + " row0=" + (r0 ? (r0.bookId + ":" + r0.chapter + ":" + r0.verse
                                  + "=" + String(window.exactRowText(r0)).substring(0, 60)) : "-")
    }
    property string exactSentenceWord: "my wife"
    property string exactTypoWord: "compeling"   // the user's own typo for "compelling"
    property string exactOrderWord: "compelling that you recieve the holy ghost now"
    property string exactHint: ""
    property var exactSentence: ({ rows: -1, bad: -1 })
    property bool exactDone: false
    property bool exactDone2: false
    property bool exactDone3: false
    property int exactPhase: 0    // 0 = loose "frien", 1 = locked "frien",
                                  // 2 = locked "friend", 3 = locked "my wife"
    property int exactTries: 0
    Timer {
        id: selfTestExactOpen
        running: typeof SelfTestExact !== "undefined" && SelfTestExact
        // Waits out boot AND the background sermon indexing (see the open test).
        interval: 45000
        onTriggered: {
            quickSearch.openSearch()
            quickSearch.query = window.exactProbeWord
            quickSearch.refresh()
            window.exactPhase = 0
            window.exactTries = 0
            window.exactDone = false
            window.exactDone2 = false
            window.exactDone3 = false
            selfTestExactPoll.restart()
        }
    }
    // One poll in two phases, so the comparison is steady-state against
    // steady-state: an EMPTY loose answer (the sermon indexing holds the library
    // lock for the first ~40s) proves nothing beside a locked answer — the loose
    // read is retried until the library actually answers, then the lock is
    // clicked and the locked read is given its own moment to land.
    Timer {
        id: selfTestExactPoll
        interval: 2000
        repeat: true
        onTriggered: {
            window.exactTries++
            const rows = window.exactSermonRows()
            if (window.exactPhase === 0) {
                if (rows.length === 0 && window.exactTries < 25) {
                    quickSearch.refresh()   // still indexing: ask again
                    return
                }
                console.log("[SELFTEST-EXACT] unlocked word=" + window.exactProbeWord
                            + " rows=" + rows.length
                            + " violations=" + window.exactViolations(rows)
                            + " all=" + quickSearch.results.length
                            + window.exactDiag(rows))
                // Click the padlock for real, through the OS input path. Whether
                // that click LANDS is logged: this harness can only synthesize
                // clicks and the driver's own note says they "need window-foreground
                // luck", so a miss is reported and the scenario then drives the
                // toggle through the very function the control's click calls —
                // everything after that (dialog -> SearchService -> the library's
                // exact filter -> the rows) is the real production path either way.
                SelfTest.clickItem("selfTestSearchExactLock")
                window.exactPhase = 1
                window.exactTries = 0
                return
            }
            // Each click's own search runs async: wait for its rows (the stale
            // previous list is still standing for the first ticks). The toggle
            // rescue below is phase 1's alone: later phases also idle two ticks
            // here, and a stray toggleExact() among them would silently flip the
            // mode the phase under test had just set up.
            if (window.exactTries < 2) {
                if (window.exactTries === 1 && window.exactPhase === 1) {
                    if (quickSearch.exact)
                        window.exactToggleBy = "click"
                    else {
                        window.exactToggleBy = "toggleExact()"
                        quickSearch.toggleExact()   // the control's own handler
                    }
                    console.log("[SELFTEST-EXACT] toggle by=" + window.exactToggleBy
                                + " shown=" + quickSearch.shown)
                }
                return
            }
            if (window.exactPhase === 1) {
                window.exactFilteredLocked = rows.length
                console.log("[SELFTEST-EXACT] filtered word=" + window.exactProbeWord
                            + " locked=" + (quickSearch.exact === true)
                            + " rows=" + rows.length)
                window.exactPhase = 2
                window.exactTries = 0
                quickSearch.query = window.exactRealWord
                quickSearch.refresh()   // still locked: the real word, exact
                return
            }
            if (window.exactPhase === 2) {
                if (rows.length === 0 && window.exactTries < 8)
                    return
                const bad = window.exactViolations(rows)
                console.log("[SELFTEST-EXACT] result word=" + window.exactRealWord
                            + " locked=" + (quickSearch.exact === true)
                            + " rows=" + rows.length + " violations=" + bad
                            + " all=" + quickSearch.results.length
                            + " lockedProbe=" + window.exactFilteredLocked)
                // The SENTENCE probe (user call: "prioritize the sentence first;
                // ... no individual words, the next available word combination"):
                // a multi-word query with the lock on must come back with
                // paragraphs that carry the words TOGETHER — exactViolations is a
                // phrase check here (exactPhraseRe), so a row that merely
                // sprinkles the words fails.
                window.exactPhase = 3
                window.exactTries = 0
                quickSearch.query = window.exactSentenceWord
                quickSearch.refresh()   // still locked: the sentence, as typed
                return
            }
            // phase 3 — the sentence probe's own read, then the pane phase (the
            // poll must stop re-reading here: a phase with no exit logs forever)
            if (window.exactPhase === 3) {
                if (window.exactDone || (rows.length === 0 && window.exactTries < 8))
                    return
                window.exactDone = true
                const sentBad = window.exactViolations(rows)
                window.exactSentence = { rows: rows.length, bad: sentBad }
                console.log("[SELFTEST-EXACT] sentence word=" + window.exactSentenceWord
                            + " locked=" + (quickSearch.exact === true)
                            + " rows=" + rows.length + " violations=" + sentBad
                            + " all=" + quickSearch.results.length
                            + window.exactDiag(rows))
                // Phase 4 - THE TYPO UNDER THE LOCK (user call: "i was supposing
                // to see the yellow auto correct show under"): a misspelled word
                // with the lock on keeps the engine's gold correction line (it
                // is how a locked search recovers - Tab takes the fix and re-runs
                // locked).
                window.exactPhase = 4
                window.exactTries = 0
                quickSearch.query = window.exactTypoWord
                quickSearch.refresh()
                return
            }
            // phase 4 — THE TYPO UNDER THE LOCK. A SIBLING of phase 3's block, not
            // its tail: these two handlers were indented after phase 3's
            // unconditional `return`, so no tick ever reached them — the hint and
            // group-order phases never logged a line and the run hung waiting for
            // an app that no scenario step would ever quit.
            if (window.exactPhase === 4 && !window.exactDone2 && window.exactTries >= 2) {
                window.exactDone2 = true
                const h = SelfTest.findItem("selfTestSearchHint")
                window.exactHint = h && h.text !== undefined ? String(h.text) : "(no hint item)"
                console.log("[SELFTEST-EXACT] hint locked=" + (quickSearch.exact === true)
                            + " text=" + JSON.stringify(window.exactHint))
                // Phase 5 — WHICH KIND LEADS (user report: a sentence The Table has
                // word-for-word sat under the Bible's looser rows). Unlocked, the
                // user's own sentence: a kind carrying it as a phrase must lead.
                window.exactPhase = 5
                window.exactTries = 0
                if (quickSearch.exact)
                    quickSearch.toggleExact()   // the general (unlocked) case
                quickSearch.query = window.exactOrderWord
                quickSearch.refresh()
            }
            // phase 5 — the group-order read, then the pane phase
            if (window.exactPhase === 5 && !window.exactDone3 && window.exactTries >= 2) {
                if (quickSearch.results.length === 0 && window.exactTries < 8)
                    return
                window.exactDone3 = true
                const first = quickSearch.results.length > 0 ? quickSearch.results[0].kind : "-"
                // The words production ranking sees: unlocked, the highlight source
                // IS the resolved query ("recieve" -> "receive"), and the gaps are
                // the same bounded fillers the dialog's phraseHitOf tolerates (the
                // "and" between "compelling" and "compulsory"). The FULL paragraph
                // (exactRowText), not the capped snippet, is what "The Table has
                // the sentence" means.
                const srcWords = String(quickSearch.highlightSource).trim().toLowerCase()
                    .split(/\s+/).filter((w) => w !== "")
                const esc = srcWords.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
                const re = new RegExp("\\b" + esc.join("\\b\\W+(?:\\w+\\W+){0,2}") + "\\b", "i")
                let tablePhrase = false
                for (const r of quickSearch.results)
                    if (r.kind === "table" && re.test(String(window.exactRowText(r))))
                        tablePhrase = true
                console.log("[SELFTEST-EXACT] order locked=" + (quickSearch.exact === true)
                            + " first=" + first + " tablePhrase=" + tablePhrase
                            + " kinds=" + JSON.stringify(quickSearch.results.slice(0, 5).map((r) => r.kind)))
                // Then the OTHER search box — the pane's pill (The Table's "Search
                // in the sermons"), whose padlock drives ReferencePane -> adapter ->
                // TheTableService -> the library. No synthetic click here: the
                // pill's lock is the same PositionHoverArea control the dialog's
                // click path already exercised, so what is at stake is the pane's
                // own wiring.
                quickSearch.close()
                window.currentView = "show"
                showScreen.showInLibrary("table", "")
                selfTestExactPane.restart()
            }
        }
    }
    property int exactPaneLoose: -1
    Timer {
        id: selfTestExactPane
        interval: 5000   // let the pane build its view + library first
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p) {
                console.log("[SELFTEST-EXACT] pane missing — the pane lock was not exercised")
                SelfTest.quit()
                return
            }
            p.searching = true
            p.exact = false
            p.runSearch(window.exactProbeWord)
            selfTestExactPaneLoose.restart()
        }
    }
    Timer {
        id: selfTestExactPaneLoose
        interval: 5000
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            window.exactPaneLoose = p.searchResults.length
            p.toggleExact()   // the pill padlock's own handler
            p.runSearch(window.exactProbeWord)
            selfTestExactPaneLocked.restart()
        }
    }
    Timer {
        id: selfTestExactPaneLocked
        interval: 5000
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            console.log("[SELFTEST-EXACT] pane locked=" + (p.exact === true)
                        + " loose=" + window.exactPaneLoose
                        + " exactRows=" + p.searchResults.length)
            SelfTest.quit()
        }
    }

    // ---- chapter lock (VGR_SCOPELOCK_TEST=1) --------------------------------
    // The book pill beside the padlock promises: with a passage OPEN, the tab's
    // search covers that passage — user call: "add a chapter lock search ... if i
    // turn the lock for the book all my search goes against that book alone and
    // not all the books". Proved in the RUNNING app on both tabs that offer it:
    // a word is searched across the library first (it lands in many sermons),
    // then the pill is CLICKED, and every locked row must sit inside the open
    // passage — fewer rows, one place, none from outside, where "outside" is
    // counted against the pane's own book id + chapter number (so the check does
    // not trust the lock, it re-derives the scope from what the tab has open).
    // The pill is clicked for real; `by=` says whether the OS click landed or the
    // control's own handler did the toggle (a synthetic click can miss).
    readonly property string scopeWord: "faith"
    // Scripture's probe word: one that IS in the passage the scenario opens
    // (Genesis 1:1 "In the beginning ...") so a zero-row locked answer can only
    // mean the scope is wrong, and one that appears all over the Bible so the
    // loose pass really does span places.
    readonly property string scopeScriptureWord: "beginning"
    property var scopeLoose: ({ rows: -1, outside: -1, places: -1 })
    property var scopeLocked: ({ rows: -1, outside: -1, places: -1 })
    property string scopeBy: ""
    property string scopeOpen: ""
    property var scopeScriptureLoose: ({ rows: -1, outside: -1, places: -1 })
    property var scopeScriptureLocked: ({ rows: -1, outside: -1, places: -1 })
    property string scopeScriptureBy: ""
    property string scopeScriptureOpen: ""
    // Rows / how many of them fall OUTSIDE the passage the pane has open / how
    // many distinct places the rows came from. `places` > 1 on the loose pass is
    // what makes the locked pass (places === 1) mean something.
    function scopeMeasure(rows, bookId, chapter) {
        const places = ({})
        let outside = 0
        for (const r of rows) {
            places[String(r.bookId) + ":" + String(r.chapter)] = true
            if (String(r.bookId) !== String(bookId) || Number(r.chapter) !== Number(chapter))
                outside++
        }
        return { rows: rows.length, outside: outside, places: Object.keys(places).length }
    }
    function scopeReport(tag, m, by, scopeText) {
        console.log("[SELFTEST-SCOPE] " + tag + " by=" + by + " scope=" + scopeText
                    + " rows=" + m.rows + " outside=" + m.outside + " places=" + m.places)
    }
    Timer {
        id: selfTestScopeOpen
        running: typeof SelfTestScopeLock !== "undefined" && SelfTestScopeLock
        // Waits out boot AND the background sermon indexing.
        interval: 45000
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p) {
                console.log("[SELFTEST-SCOPE] table pane missing")
                SelfTest.quit()
                return
            }
            p.searching = true
            p.exact = false
            p.scopeLock = false
            p.openBook(0, 1)   // the first year, its first sermon
            selfTestScopeGo.restart()
        }
    }
    Timer {
        id: selfTestScopeGo
        interval: 4000   // the chapter has to load before its scope is real
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            window.scopeOpen = String(p.book.id) + ":" + p.chapterNumber
            p.runSearch(window.scopeWord)
            selfTestScopeLoose.restart()
        }
    }
    Timer {
        id: selfTestScopeLoose
        interval: 8000   // the worker answers with the whole-library rows
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            window.scopeLoose = window.scopeMeasure(p.searchResults, p.book.id, p.chapterNumber)
            window.scopeReport("table loose", window.scopeLoose, "-", window.scopeOpen)
            SelfTest.activate()
            SelfTest.clickItem("selfTestChapterLock-table")
            window.scopeBy = p.scopeLock === true ? "click" : ""
            if (p.scopeLock !== true) {
                p.toggleScopeLock()   // the pill's own handler
                window.scopeBy = p.scopeLock === true ? "handler" : "missed"
            }
            p.runSearch(window.scopeWord)
            selfTestScopeLocked.restart()
        }
    }
    Timer {
        id: selfTestScopeLocked
        interval: 8000
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            window.scopeLocked = window.scopeMeasure(p.searchResults, p.book.id, p.chapterNumber)
            window.scopeReport("table locked", window.scopeLocked, window.scopeBy, window.scopeOpen)
            selfTestScopeScripture.restart()
        }
    }
    Timer {
        id: selfTestScopeScripture
        interval: 2000
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            if (!s || !s.books || s.books.length === 0) {
                console.log("[SELFTEST-SCOPE] scripture pane/books missing — table pass only")
                console.log("[SELFTEST-SCOPE] result tableLoose=" + window.scopeLoose.places
                            + " tableOutside=" + window.scopeLocked.outside
                            + " tableRows=" + window.scopeLocked.rows
                            + " tableBy=" + window.scopeBy + " scripture=skipped")
                SelfTest.quit()
                return
            }
            s.searching = true
            s.exact = false
            s.scopeLock = false
            s.openBook(0, 1)
            selfTestScopeScriptureGo.restart()
        }
    }
    Timer {
        id: selfTestScopeScriptureGo
        interval: 4000
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            window.scopeScriptureOpen = String(s.book.id) + ":" + s.chapterNumber
            s.runSearch(window.scopeScriptureWord)
            selfTestScopeScriptureLoose.restart()
        }
    }
    Timer {
        id: selfTestScopeScriptureLoose
        interval: 8000
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            window.scopeScriptureLoose = window.scopeMeasure(s.searchResults, s.book.id, s.chapterNumber)
            window.scopeReport("scripture loose", window.scopeScriptureLoose, "-", window.scopeScriptureOpen)
            SelfTest.activate()
            SelfTest.clickItem("selfTestChapterLock-scripture")
            window.scopeScriptureBy = s.scopeLock === true ? "click" : ""
            if (s.scopeLock !== true) {
                s.toggleScopeLock()
                window.scopeScriptureBy = s.scopeLock === true ? "handler" : "missed"
            }
            s.runSearch(window.scopeScriptureWord)
            selfTestScopeScriptureLocked.restart()
        }
    }
    Timer {
        id: selfTestScopeScriptureLocked
        interval: 8000
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            window.scopeScriptureLocked = window.scopeMeasure(s.searchResults, s.book.id, s.chapterNumber)
            window.scopeReport("scripture locked", window.scopeScriptureLocked, window.scopeScriptureBy,
                               window.scopeScriptureOpen)
            console.log("[SELFTEST-SCOPE] result tableLoose=" + window.scopeLoose.places
                        + " tableOutside=" + window.scopeLocked.outside
                        + " tableRows=" + window.scopeLocked.rows
                        + " tableBy=" + window.scopeBy
                        + " tableScope=" + window.scopeOpen
                        + " scriptureLoose=" + window.scopeScriptureLoose.places
                        + " scriptureOutside=" + window.scopeScriptureLocked.outside
                        + " scriptureRows=" + window.scopeScriptureLocked.rows
                        + " scriptureBy=" + window.scopeScriptureBy
                        + " scriptureScope=" + window.scopeScriptureOpen)
            SelfTest.quit()
        }
    }

    // ---- a mark on a verse (VGR_MARK_TEST=1) --------------------------------
    // WHAT A NOTE OR HIGHLIGHT COSTS THE OPERATOR, measured in the RUNNING app.
    // setNote/setHighlighted are Q_INVOKABLE on a QML singleton, so their cost is
    // the INTERFACE thread's — and that path used to be a whole-corpus rewrite:
    // read bibles.json, parse it, write it back, all under the engine lock every
    // scripture read takes (a real dev store is 5.8 MB for one Bible). The engine
    // tests pin the cheap half (one small sidecar file, corpus byte-identical)
    // and put a number on it on a synthetic corpus; this drives the REAL app on
    // the operator's OWN store, through the pane's own handlers:
    //   * the star button's toggleHighlight(), timed on and off again;
    //   * the note editor's saveNoteDraft(), timed added and removed again.
    // A verse with NO mark on it is chosen, so undoing the marks restores exactly
    // the state the scenario found — the store afterwards is the store before,
    // which tools/selftest/run_mark_sidecar_test.sh checks byte for byte and
    // counts against the engine log (sidecar writes up, corpus writes zero).
    readonly property string markNoteText: "selftest mark note (removed again)"
    property var markMs: ({ on: -1, off: -1, note: -1, noteOff: -1 })
    property int markVerse: 0
    property string markWhere: ""
    property string markSkip: ""
    property string markDetail: ""
    function markPickVerse() {
        const s = SelfTest.findItem("selfTestScripturePane")
        if (!s || !s.book || s.chapterVerses.length === 0)
            return 0
        for (const v of s.chapterVerses) {
            const m = s.markFor(v.number)
            if (!m.highlight && !m.note)
                return v.number
        }
        return 0
    }
    function markReport(restored) {
        console.log("[SELFTEST-MARK] result where=" + window.markWhere
                    + " skip='" + window.markSkip + "' verse=" + window.markVerse
                    + " highlightOnMs=" + window.markMs.on
                    + " highlightOffMs=" + window.markMs.off
                    + " noteAddMs=" + window.markMs.note
                    + " noteRemoveMs=" + window.markMs.noteOff
                    + " restored=" + restored + " detail='" + window.markDetail + "'")
    }
    Timer {
        id: selfTestMarkOpen
        running: typeof SelfTestMark !== "undefined" && SelfTestMark
        interval: 45000   // boot, the store restore and the background re-index
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            if (!s) {
                window.markSkip = "scripture pane missing"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            s.searching = false
            s.openBook(0, 1)   // the first Bible, its first book, chapter 1
            selfTestMarkGo.restart()
        }
    }
    Timer {
        id: selfTestMarkGo
        interval: 4000   // the chapter has to load before a verse can be marked
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            window.markWhere = String(s.sourceId) + " " + String(s.book.id)
                                + " " + s.chapterNumber
            window.markVerse = window.markPickVerse()
            if (window.markVerse === 0) {
                window.markSkip = "no unmarked verse in the open chapter"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            // ---- the star: the row control's own handler, as the interface
            // thread feels it (the JSON build AND the write are inside it).
            let t0 = Date.now()
            s.toggleHighlight(window.markVerse)
            window.markMs.on = Date.now() - t0
            selfTestMarkOn.restart()
        }
    }
    Timer {
        id: selfTestMarkOn
        interval: 700   // the pane's batched mark map lands on the next revision
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            if (s.markFor(window.markVerse).highlight !== true) {
                window.markDetail = "the highlight never reached the pane"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            const t0 = Date.now()
            s.toggleHighlight(window.markVerse)
            window.markMs.off = Date.now() - t0
            selfTestMarkOff.restart()
        }
    }
    Timer {
        id: selfTestMarkOff
        interval: 700
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            if (s.markFor(window.markVerse).highlight === true) {
                window.markDetail = "the highlight did not clear again"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            // ---- the note: the editor's own open/save pair, timed.
            s.openNoteEditor(window.markVerse)
            const t0 = Date.now()
            s.saveNoteDraft(window.markNoteText)
            window.markMs.note = Date.now() - t0
            selfTestMarkNote.restart()
        }
    }
    Timer {
        id: selfTestMarkNote
        interval: 700
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            if (s.markFor(window.markVerse).note !== window.markNoteText) {
                window.markDetail = "the note never reached the pane"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            s.openNoteEditor(window.markVerse)
            const t0 = Date.now()
            s.saveNoteDraft("")   // empty = the delete gesture
            window.markMs.noteOff = Date.now() - t0
            selfTestMarkDone.restart()
        }
    }
    Timer {
        id: selfTestMarkDone
        interval: 700
        onTriggered: {
            const s = SelfTest.findItem("selfTestScripturePane")
            const m = s.markFor(window.markVerse)
            if (m.highlight || m.note) {
                window.markDetail = "the verse is still marked (highlight="
                                    + !!m.highlight + " note='" + (m.note || "") + "')"
                window.markReport(false)
                SelfTest.quit()
                return
            }
            window.markReport(true)
            SelfTest.quit()
        }
    }

    // ---- the list is divided BEFORE it is clicked (VGR_PRESPLIT_TEST=1) -----
    // User call: "now the app should split not waiting until it click before
    // spliting so it doesnt give the gui thread unnessry job during pressure time
    // when presenting in church". The paragraph list used to be divided only for
    // the passage the operator had ALREADY picked — the rows were built from the
    // pick's own preview — so a long sermon paragraph read as one long line until
    // it was clicked, and the division it needed landed on the interface thread at
    // the exact moment the operator was presenting.
    //
    // Proved in the RUNNING app, with NO row clicked first: a sermon is opened, the
    // pick is then EMPTIED (the state in which no pick can explain any row), and the
    // rows the list actually renders (mainRows — the verse list's own model) must
    // still carry the continuations the chapter was cut into: some paragraph shown as
    // its own rows ("7", "7-1", "7-2"). Then one of those continuation rows is
    // CLICKED for real, and the piece it names must be the piece the pane's preview
    // moves to — the row the operator sees before touching anything and the row they
    // get when they do have to be the same division, or one of the two is lying.
    property var preSplitOpenRow: null
    // Has the row to be clicked been brought on screen yet? The paragraph list is
    // VIRTUALIZED (see ReferencePane's ListView): a row that is off screen is not
    // built, so it has no center to click — the reveal below scrolls it there, the
    // click waits for it to arrive, and only then is a click sent.
    property bool preSplitRowReady: false
    property var preSplitFacts: ({ rows: -1, split: -1, hadPick: false, openMs: -1 })
    property var preSplitRestore: ({})
    property double preSplitOpenAt: 0
    property int preSplitTries: 0
    property int preSplitClickTries: 0
    property int preSplitWaitTicks: 0
    // The row's own name (see ReferencePane's delegate): its label, spelled the same
    // way, so a scenario can address the continuation row the operator is looking at
    // rather than the first row that happens to carry the paragraph.
    function preSplitRowName(row) {
        if (!row)
            return ""
        return "selfTestVerseRow_" + Number(row.number)
                + (row.continuation === true ? "-" + Number(row.partIndex) : "")
    }
    // The settings this run changed, put back exactly as they were found (the tab
    // divides long paragraphs only when it is asked to — the shipped default is off,
    // and the operator's own profile is not this scenario's to rewrite).
    function preSplitRestoreSettings() {
        if (window.preSplitRestore.split === undefined || window.preSplitRestore.words === undefined)
            return
        SettingsService.setValue("table.splitLongVerses", window.preSplitRestore.split)
        SettingsService.setValue("table.wordsPerSlide", window.preSplitRestore.words)
    }
    Timer {
        id: selfTestPreSplitOpen
        running: typeof SelfTestPreSplit !== "undefined" && SelfTestPreSplit
        // Boot AND the background sermon indexing, as the chapter-lock scenario
        // waits them out.
        interval: 45000
        onTriggered: {
            // THE PANE ON SCREEN FIRST. The dock shows one library tab at a time
            // (`visible: media_tab_bar.currentPane === "table"`), and a pane that is not
            // on screen has no VISIBLE row to click — while its model answers every
            // question anyway, which is how this scenario once reported a fully divided
            // list and then waited a minute for a row that was never on screen. The
            // launch state is not the scenario's to rely on (it is whatever tab the
            // operator left the dock on), so it puts the tab up itself, the way the
            // quick-search scenarios do, and lets the pane lay out before opening.
            window.currentView = "show"
            showScreen.showInLibrary("table", "")
            selfTestPreSplitOpened.restart()
        }
    }
    Timer {
        id: selfTestPreSplitOpened
        interval: 1500   // a beat for the tab switch to lay the pane out
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p || p.books.length === 0) {
                console.log("[SELFTEST-PRESPLIT] table pane/books missing — no sermon to cut")
                SelfTest.quit()
                return
            }
            window.preSplitRestore = {
                split: SettingsService.value("table.splitLongVerses"),
                words: SettingsService.value("table.wordsPerSlide")
            }
            // A budget of 20 words guarantees a sermon paragraph divides (its own
            // shipped number is 40, and its paragraphs run to hundreds).
            SettingsService.setValue("table.splitLongVerses", true)
            SettingsService.setValue("table.wordsPerSlide", 20)
            p.searching = false
            window.preSplitOpenAt = Date.now()
            // A COLLECTION THE APP DID NOT RESTORE, deliberately: the dock comes back on
            // whatever passage was last open, and that one is already read and cut (it is
            // held from the boot). Opening IT would measure a lookup, not an open — the
            // cold path is the one an operator takes when they click a different sermon,
            // and it is the path with the head's cut, the remainder behind it and the
            // neighbours to read. The report prints which book it landed on.
            p.openBook(p.bookIndex === 0 ? 1 : 0, 1)
            selfTestPreSplitPoll.restart()
        }
    }
    // Wait for the rows to actually BE there: the chapter read is asynchronous, and the
    // list builds its rows when it lands. The wait doubles as the measurement this whole
    // report is about — how long after the open the rows are on screen, with nothing
    // clicked — so the number in the log is the operator's own, not a timer's.
    Timer {
        id: selfTestPreSplitPoll
        interval: 25
        repeat: true
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p || window.preSplitTries++ > 800)
                return
            // A row ITEM, not just the model: the verse list's own delegate is what the
            // operator sees, so its existence is the list having been built.
            if (p.chapterLoading || p.chapterVerses.length === 0
                    || !SelfTest.findItem("selfTestVerseRow_1"))
                return
            // THE CUT ARRIVES IN TWO PIECES (SlideBuilder's head/remainder block): the head
            // with the open — which is what the first number below measures — and the rest
            // of the sermon a beat later, which is the half that divides a paragraph BELOW
            // the fold (the reported bug, one screen down). So the division is allowed to
            // settle before it is judged, and both readings are reported: what the operator
            // saw the moment the sermon appeared, and what the whole list ended up as.
            const splitNow = p.mainRows.filter((r) => r.continuation === true).length
            if (window.preSplitFacts.openMs < 0) {
                window.preSplitFacts.openMs = Date.now() - window.preSplitOpenAt
                window.preSplitFacts.openSplit = splitNow
            }
            // Settled = the division has not moved for a comfortable beat, and the beat is
            // longer than the second half takes to arrive (the remainder's own cut is tens
            // of ms for a whole sermon, plus its delivery). Judging it the instant it stops
            // changing for one tick would judge the HEAD, which is exactly the half that is
            // not the whole story.
            if (splitNow !== window.preSplitFacts.lastSplit) {
                window.preSplitFacts.lastSplit = splitNow
                window.preSplitFacts.lastChangeAt = Date.now()
                window.preSplitTries = 0   // an arriving half resets the patience, not the poll
                return
            }
            const lastChange = window.preSplitFacts.lastChangeAt || window.preSplitOpenAt
            if (Date.now() - lastChange < 600)
                return
            window.preSplitFacts.finalSplit = splitNow
            window.preSplitFacts.settledMs = Date.now() - window.preSplitOpenAt
            selfTestPreSplitPoll.stop()
            window.preSplitFacts.hadPick = p.selected.length > 0
            // THE PICK IS EMPTIED FIRST, deliberately: it is the one state in which a
            // pick cannot be what put the continuations on screen. (openBook selects
            // the first paragraph, as clicking it would.)
            p.selected = []
            p.previewSlideIndex = 0
            const rows = p.mainRows
            const split = rows.filter((r) => r.continuation === true)
            window.preSplitFacts.rows = rows.length
            window.preSplitFacts.split = split.length
            console.log("[SELFTEST-PRESPLIT] opened book=" + p.book.name + " " + p.chapterNumber
                        + " rows=" + rows.length + " splitRows=" + split.length
                        + " openMs=" + window.preSplitFacts.openMs
                        + " openSplitRows=" + window.preSplitFacts.openSplit
                        + " settledMs=" + window.preSplitFacts.settledMs
                        + " pickedBefore=" + (window.preSplitFacts.hadPick ? 1 : 0)
                        + " pickedNow=" + p.selected.length)
            if (split.length === 0) {
                console.log("[SELFTEST-PRESPLIT] result rows=" + rows.length
                            + " splitRows=0 openMs=" + window.preSplitFacts.openMs
                            + " — the list was NOT divided before the click")
                window.preSplitRestoreSettings()
                SelfTest.quit()
                return
            }
            window.preSplitOpenRow = split[0]
            // Bring the row ON SCREEN first — the scroll an operator does before
            // clicking, and the only way a row that is not built can be clicked at
            // all (see preSplitRowReady). The index it takes is the row's OWN: a
            // divided chapter makes a paragraph's number and its row index different
            // things, so the row is found by the pair the two lists share (number +
            // piece) rather than by number alone.
            window.preSplitRowReady = false
            p.revealRow(p.mainRows.findIndex((r) => Number(r.number) === Number(split[0].number)
                                               && Number(r.partIndex ?? 0) === Number(split[0].partIndex ?? 0)))
            SelfTest.activate()
            selfTestPreSplitClicked.restart()
        }
    }
    // ---- THE LAYOUT PROBE SCENARIO (VGR_LAYOUT_TEST, tools/selftest/run_layout_probe.sh) ----
    // Drives the two interactions the hang captures name — a filter typed into a library tab,
    // and a pane switch — one per second, and reports for each step:
    //
    //   stallMax/stallSum  how long the INTERFACE thread was parked, measured from the thread
    //                      itself: a 16 ms heartbeat timer that asks for the interval it wants
    //                      and reports the overshoot. A DWrite/shape/relayout pile-up cannot
    //                      hide from this the way it hides from a wall-clock stopwatch (the
    //                      stopwatch measures the work, this measures the freeze), and the
    //                      C++ FrameProbe in the same log is the same stall seen from the
    //                      render thread as the gap between painted frames.
    //   built/dropped      the delegates each list built and THREW AWAY during the step. This
    //                      is what separates the two causes: a pane switch that shows
    //                      builds=+N drops=+N is a view releasing and re-creating its items,
    //                      while a filter that shows builds=+N drops=0 is a one-way rebuild
    //                      (nothing was thrown away, the list just grew a new tail).
    //   rows               how many rows each list's model holds after the step, so a rebuild
    //                      of a 6-card library can't be confused with one of a 33-card one.
    //
    // The walk: a cold switch through every library tab; the SAME-WIDTH pane pairs (media /
    // overlays / templates all share the narrowed dock width) against the WIDTH-CROSSING pairs
    // (a reference tab takes the template column's whole width) — the two hypotheses for why a
    // delegate is re-created, a hidden view or a moved cell; a filter typed into a design
    // library and into the shows table; and the same pane walk again with everything warm.
    property var layoutProbeSteps: [
        { pane: "shows",     tag: "switch-shows-cold" },
        { pane: "media",     tag: "switch-media-cold" },
        { pane: "overlays",  tag: "switch-overlays-cold" },
        { pane: "templates", tag: "switch-templates-cold" },
        { pane: "table",     tag: "switch-table-cold" },
        { pane: "scripture", tag: "switch-scripture-cold" },
        { pane: "shows",     tag: "switch-shows-back" },
        { pane: "overlays",  tag: "samewidth-overlays-1" },
        { pane: "templates", tag: "samewidth-templates-1" },
        { pane: "overlays",  tag: "samewidth-overlays-2" },
        { pane: "templates", tag: "samewidth-templates-2" },
        { pane: "table",     tag: "crosswidth-table-1" },
        { pane: "templates", tag: "crosswidth-templates-1" },
        { pane: "table",     tag: "crosswidth-table-2" },
        { pane: "templates", tag: "crosswidth-templates-2" },
        // The LIBRARY lists' filter, through the app's own entry point (the tab bar's
        // search: showInLibrary is what its typing beat ends in — every pane reads the
        // query as searches.<pane>): the shows table (680 rows), then the two design grids.
        { pane: "shows",     query: "",  tag: "shows-filter-open" },
        { pane: "shows",     query: "s", tag: "shows-filter-s" },
        { pane: "shows",     query: "se", tag: "shows-filter-se" },
        { pane: "shows",     query: "",  tag: "shows-filter-clear" },
        { pane: "overlays",  query: "",  tag: "overlay-filter-open" },
        { pane: "overlays",  query: "c", tag: "overlay-filter-c" },
        { pane: "templates", query: "s", tag: "template-filter-s" },
        { pane: "overlays",  query: "",  tag: "overlay-filter-clear" },
        // The PASSAGE list's filter: the tab bar's query narrows the book list, and the
        // pane's OWN box runs the citation search whose hits fill the hit rows.
        { pane: "table",     query: "",      tag: "passage-filter-open" },
        { pane: "table",     query: "faith", tag: "passage-filter-faith" },
        { pane: "table",     query: "",      tag: "passage-filter-clear" },
        { body: "table",     text: "faith",  search: true, tag: "passage-search-faith" },
        { body: "table",     text: "faith",  search: true, tag: "passage-search-settle" },
        { body: "table",     text: "",       search: true, tag: "passage-search-clear" },
        // THE SCROLL PHASE (the scroll-cost probe). The steps above measure a SETTLE — a switch,
        // a filter — one second each. The lag the operator actually reports ("scripture,
        // templates and the table are sluggish when I scroll") is a DIFFERENT measurement: cost
        // paid continuously, per frame, while rows enter and leave the viewport. So each step
        // here scrolls ONE list through its whole content with a linear animation (700 ms, one
        // and a half screens per second — a fast wheel drag) and the step reports:
        //
        //   frames/fps/avgFrameMs/worstFrameMs/jank33/jank50
        //                    the frame times the app actually produced DURING the scroll, from a
        //                    FrameAnimation on the interface thread — the same cadence the render
        //                    thread paints at, so the C++ FrameProbe lines beside these agree with
        //                    them. The first frame is skipped: its frameTime is the idle gap since
        //                    the last painted frame, which is not a scroll cost.
        //   built/dropped    the scrolled list's delegates, as a delta over the scroll window —
        //                    rows the scroll created and rows it threw away.
        //   *Evals           the row binding evaluations the same window cost: the reference
        //                    pane's verse/book/chapter/source rows' own label/text bindings, and
        //                    cardFitSolves — the design cards' fit solvers (11 bisections and
        //                    re-measures each) run while cards are built. A list that scrolls with
        //                    eval deltas of 0 is not paying binding work at all, whatever its
        //                    frames say; one whose evals scale with the scroll distance is.
        //   contentPx/viewPx/scrollPx
        //                    how far the scroll actually went, so a short list (or one that could
        //                    not scroll) cannot be read as a free one.
        //
        // Each scroll step still carries the heartbeat stall numbers: a scroll that parks the
        // interface thread shows up there at the same time as in the frame times.
        // Each scroll step names the SNAPSHOT KEYS of the list it scrolls (rows/built/dropped/
        // evals), so the line reports that list's own numbers as scrollRows/scrollBuilt/
        // scrollDropped/scrollEvals instead of leaving the reader to map a list name onto the
        // per-list fields by hand. The deltas are the scroll window's: the line's "previous"
        // snapshot is taken before the scroll starts (the beat timer runs first).
        { pane: "shows",     scroll: "selfTestShowsList",            rows: "showsRows",    built: "showsBuilt",    dropped: "showsDropped",    evals: "showsEvals",    tag: "scroll-shows-rows" },
        { pane: "table",     scroll: "selfTestVersesList-table",     rows: "refVerseRows", built: "refVerseBuilt", dropped: "refVerseDropped", evals: "refVerseEvals", tag: "scroll-table-verses" },
        { pane: "scripture", scroll: "selfTestVersesList-scripture", rows: "refVerseRows", built: "refVerseBuilt", dropped: "refVerseDropped", evals: "refVerseEvals", tag: "scroll-scripture-verses" },
        { pane: "scripture", scroll: "selfTestBooksList-scripture",  rows: "refBookRows",  built: "refBookBuilt",  dropped: "refBookDropped",  evals: "refBookEvals",  tag: "scroll-scripture-books" },
        { pane: "scripture", scroll: "selfTestChaptersList-scripture", rows: "refChapterRows", built: "refChapterBuilt", dropped: "refChapterDropped", evals: "refChapterEvals", tag: "scroll-scripture-chapters" },
        { pane: "templates", scroll: "selfTestDesignGrid-template",  rows: "templateRows", built: "templateBuilt", dropped: "templateDropped", evals: "cardFitSolves", tag: "scroll-templates-grid" },
        { pane: "overlays",  scroll: "selfTestDesignGrid-overlay",   rows: "overlayRows",  built: "overlayBuilt",  dropped: "overlayDropped",  evals: "cardFitSolves", tag: "scroll-overlays-grid" },
        { pane: "shows",     tag: "switch-shows-warm" },
        { pane: "templates", tag: "switch-templates-warm" },
        { pane: "table",     tag: "switch-table-warm" }
    ]
    property int layoutProbeStep: 0
    property double layoutProbeT0: 0
    property string layoutProbePending: ""
    // The worst overshoot of a 16 ms timer on the interface thread this step, and the sum of
    // them: the freeze itself, not the work that caused it.
    property double layoutProbeStallMax: 0
    property double layoutProbeStallSum: 0
    property double layoutProbeBeat: 0
    // The counters as they stood at the END of the previous step, so each line is a delta.
    property var layoutProbePrev: null
    // The step whose line has not been written yet, so the line can report the fields its own
    // list carries (the scroll steps name them in the step itself — see layoutProbeSteps).
    property var layoutProbePendingStep: null

    function layoutProbePanes() {
        return { media: SelfTest.findItem("selfTestMediaPane"),
                 overlays: SelfTest.findItem("selfTestOverlaysPane"),
                 templates: SelfTest.findItem("selfTestTemplatesPane"),
                 table: SelfTest.findItem("selfTestTablePane"),
                 scripture: SelfTest.findItem("selfTestScripturePane"),
                 shows: SelfTest.findItem("selfTestShowsRows") }
    }
    // The two reference panes (The Table and Scripture) are the SAME component, so their
    // counters are summed: a scroll of the sermon list and a scroll of the bible list land in
    // the same field, and because the scroll phase scrolls ONE named list per step, the delta
    // that moves is that list's — the line names the list it scrolled.
    function layoutProbeRefSum(prop) {
        const panes = window.layoutProbePanes()
        let total = 0
        for (const key of ["table", "scripture"]) {
            const item = panes[key]
            const value = item ? item[prop] : undefined
            if (typeof value === "number" && value >= 0)
                total += value
        }
        return total
    }
    // The reference panes' binding-evaluation totals, which live in a plain JS accumulator
    // (ReferencePane.probeEvals — a QML property written from inside a binding is a reported
    // binding loop, so the counters are JS fields and are read here rather than notified).
    function layoutProbeRefEvals(key) {
        const panes = window.layoutProbePanes()
        let total = 0
        for (const tag of ["table", "scripture"]) {
            const item = panes[tag]
            const value = (item && item.probeEvals) ? item.probeEvals[key] : undefined
            if (typeof value === "number" && value >= 0)
                total += value
        }
        return total
    }
    function layoutProbeEvalsOf(item, key) {
        const value = (item && item.probeEvals) ? item.probeEvals[key] : undefined
        return (typeof value === "number" && value >= 0) ? value : -1
    }
    function layoutProbeRefLen(prop) {
        const panes = window.layoutProbePanes()
        let total = 0
        for (const key of ["table", "scripture"]) {
            const item = panes[key]
            const value = item ? item[prop] : undefined
            if (value !== undefined && value !== null && value.length !== undefined)
                total += value.length
        }
        return total
    }
    function layoutProbeRefChapters() {
        const panes = window.layoutProbePanes()
        let total = 0
        for (const key of ["table", "scripture"]) {
            const item = panes[key]
            const chapters = item && item.book ? item.book.chapters : undefined
            if (chapters && chapters.length !== undefined)
                total += chapters.length
        }
        return total
    }
    function layoutProbeSnapshot() {
        const p = window.layoutProbePanes()
        const num = (o, name) => (o && o[name] !== undefined) ? o[name] : -1
        const len = (o, name) => (o && o[name] !== undefined) ? o[name].length : -1
        return {
            showsRows: len(p.shows, "shownShows"),
            showsBuilt: num(p.shows, "showsRowBuilds"), showsDropped: num(p.shows, "showsRowDrops"),
            showsEvals: window.layoutProbeEvalsOf(p.shows, "rows"),
            mediaRows: len(p.media, "gridItems"),
            mediaBuilt: num(p.media, "gridRowBuilds"), mediaDropped: num(p.media, "gridRowDrops"),
            overlayRows: len(p.overlays, "gridItems"),
            overlayBuilt: num(p.overlays, "gridRowBuilds"), overlayDropped: num(p.overlays, "gridRowDrops"),
            templateRows: len(p.templates, "gridItems"),
            templateBuilt: num(p.templates, "gridRowBuilds"), templateDropped: num(p.templates, "gridRowDrops"),
            verseRows: len(p.table, "mainRows"),
            verseBuilt: num(p.table, "verseRowBuilds"), verseDropped: num(p.table, "verseRowDrops"),
            hitBuilt: num(p.table, "hitRowBuilds"), hitDropped: num(p.table, "hitRowDrops"),
            // The reference panes' OWN rows (both tabs summed) — the lists the scroll phase
            // scrolls, and the ones the operator's report names (scripture, the table).
            refVerseRows: window.layoutProbeRefLen("mainRows"),
            refVerseBuilt: window.layoutProbeRefSum("verseRowBuilds"),
            refVerseDropped: window.layoutProbeRefSum("verseRowDrops"),
            refVerseEvals: window.layoutProbeRefEvals("verse"),
            refBookRows: window.layoutProbeRefLen("visibleBooks"),
            refBookBuilt: window.layoutProbeRefSum("bookRowBuilds"),
            refBookDropped: window.layoutProbeRefSum("bookRowDrops"),
            refBookEvals: window.layoutProbeRefEvals("book"),
            refChapterRows: window.layoutProbeRefChapters(),
            refChapterBuilt: window.layoutProbeRefSum("chapterRowBuilds"),
            refChapterDropped: window.layoutProbeRefSum("chapterRowDrops"),
            refChapterEvals: window.layoutProbeRefEvals("chapter"),
            refSourceRows: window.layoutProbeRefLen("sources"),
            refSourceBuilt: window.layoutProbeRefSum("sourceRowBuilds"),
            refSourceDropped: window.layoutProbeRefSum("sourceRowDrops"),
            refSourceEvals: window.layoutProbeRefEvals("source"),
            // The design cards' own cost (both design grids summed): every text block of every
            // card that gets built runs an 11-step fit bisection over a hidden twin Text, counted
            // on the PANE each card reports to (a counter on the card would reset with the card).
            cardFitSolves: Math.max(0, num(p.overlays, "gridFitSolves"))
                           + Math.max(0, num(p.templates, "gridFitSolves"))
        }
    }
    function layoutProbeLine(tag, ms) {
        const now = window.layoutProbeSnapshot()
        const prev = window.layoutProbePrev
        const d = (key) => (prev === null || prev[key] < 0 || now[key] < 0) ? 0 : now[key] - prev[key]
        window.layoutProbePrev = now
        const list = (name, rowsKey, builtKey, droppedKey) =>
            " " + name + "Rows=" + now[rowsKey]
            + " " + name + "Built=+" + d(builtKey) + " " + name + "Dropped=+" + d(droppedKey)
        // The reference panes' rows, with the binding evaluations each row cost as well as the
        // builds and drops: `refVerseEvals` counts evaluations of the verse rows' own text
        // binding, `refBookEvals`/`refChapterEvals`/`refSourceEvals` the label bindings of the
        // three sidebar lists. They are printed on EVERY step (a pane switch and a filter are
        // built and re-evaluated too), and the scroll phase's line is what ties them to frames.
        const ref = (name, rowsKey, builtKey, droppedKey, evalsKey) =>
            " ref" + name + "Rows=" + now[rowsKey]
            + " ref" + name + "Built=+" + d(builtKey) + " ref" + name + "Dropped=+" + d(droppedKey)
            + " ref" + name + "Evals=+" + d(evalsKey)
        return "phase=" + tag + " ms=" + ms
               + " stallMax=" + Math.round(window.layoutProbeStallMax)
               + " stallSum=" + Math.round(window.layoutProbeStallSum)
               + list("shows", "showsRows", "showsBuilt", "showsDropped")
               + list("media", "mediaRows", "mediaBuilt", "mediaDropped")
               + list("overlay", "overlayRows", "overlayBuilt", "overlayDropped")
               + list("template", "templateRows", "templateBuilt", "templateDropped")
               + list("verse", "verseRows", "verseBuilt", "verseDropped")
               + " hitBuilt=+" + d("hitBuilt") + " hitDropped=+" + d("hitDropped")
               + ref("Verse", "refVerseRows", "refVerseBuilt", "refVerseDropped", "refVerseEvals")
               + ref("Book", "refBookRows", "refBookBuilt", "refBookDropped", "refBookEvals")
               + ref("Chapter", "refChapterRows", "refChapterBuilt", "refChapterDropped", "refChapterEvals")
               + ref("Source", "refSourceRows", "refSourceBuilt", "refSourceDropped", "refSourceEvals")
               + " cardFitSolves=+" + d("cardFitSolves")
               // A scroll step that never got going (a name that did not resolve, or a list with
               // nothing to scroll) is reported here; one that ran writes its own record when the
               // scroll stops (layoutProbeLogScroll).
               + ((window.layoutProbePendingStep && window.layoutProbePendingStep.scroll !== undefined)
                  ? window.layoutProbeScrollFields() : "")
               + window.layoutProbeScrolledFields(prev, now, window.layoutProbePendingStep)
    }

    // The scrolled list's own deltas (the step names the snapshot keys): what that list built,
    // threw away and evaluated over the window being reported. Empty on a step that did not
    // scroll. `prev` is passed in because the two callers measure against different baselines:
    // the step line against the previous step's end, the scroll's own record against its start.
    function layoutProbeScrolledFields(prev, now, step) {
        if (!step || step.scroll === undefined)
            return ""
        const d = (key) => (prev === null || prev[key] === undefined || prev[key] < 0 || now[key] < 0)
                            ? 0 : now[key] - prev[key]
        const rows = (key) => (now[key] !== undefined && now[key] >= 0) ? now[key] : -1
        return " scrollRows=" + rows(step.rows)
               + " scrollBuilt=+" + d(step.built) + " scrollDropped=+" + d(step.dropped)
               + " scrollEvals=+" + d(step.evals)
    }

    // ---- the scroll phase's machinery (see the scroll steps in layoutProbeSteps) ----
    // The scrolled list's name, its frame meter's running totals, and where the scroll started.
    property string scrollListName: ""
    // The STEP that armed this scroll, kept here rather than read off layoutProbePendingStep: a
    // starved interface thread can delay the beat timer past the tick that advances the walk, and
    // the record would then be tagged with the NEXT step's list (measured: a books-list record
    // arriving while the chapters step was already pending).
    property var scrollStep: null
    property bool scrollFound: false
    // Armed = THIS step's scroll is configured and running; logArmed = its stop is the one whose
    // record belongs to this step. Two flags rather than one because the animation object is
    // shared and a starved interface thread overlaps things: the stop of a PREVIOUS run (however
    // it ended) must not write the current step's record, and only a scroll that actually started
    // gets a record written on stop.
    property bool scrollArmed: false
    property bool scrollLogArmed: false
    property int scrollTries: 0
    property bool scrollMeterOn: false
    property int scrollFrames: 0
    property double scrollMs: 0        // the frame times the meter saw, summed (ms)
    property double scrollWorstMs: 0
    property int scrollJank33: 0       // frames longer than two 60 Hz frames — the ones a user sees
    property int scrollJank50: 0
    property double scrollSeconds: 0   // the meter's own window, from FrameAnimation.elapsedTime (seconds)
    property double scrollWallMs: 0    // …and the wall clock the same run took, so a scroll the app
                                       // produced NO frames during (meter window 0) still reads as
                                       // the elapsed time it was, not as an instant one
    property double scrollT0: 0
    property double scrollStartY: 0
    // The counter snapshot taken when the scroll started, so the scroll's own record can report
    // its deltas without depending on when the step timer happened to fire (see LogScroll).
    property var scrollPrevCounts: null

    function scrollMeterReset() {
        window.scrollFrames = 0
        window.scrollMs = 0
        window.scrollWorstMs = 0
        window.scrollJank33 = 0
        window.scrollJank50 = 0
        window.scrollSeconds = 0
        window.scrollWallMs = 0
    }

    // Scroll ONE list through its whole content, linearly, with the frame meter running. This is
    // the measurement the phase exists for: the frame times the app produced while rows were
    // entering and leaving the viewport, not a stopwatch around a positionViewAtIndex jump
    // (which would time a single relayout and miss the cost of the rows themselves).
    // Write the record for a scroll that is STILL IN FLIGHT, before anything can stop it
    // unrecorded. The animation object is shared, so the next step's arming stops it — and that
    // stop arrives with the next step's arming already in progress, i.e. after this step's
    // `scrollLogArmed` was cleared, so the stop is swallowed and the scroll is never reported at
    // all (measured: the scripture passage scroll ran, was armed, started, and wrote nothing).
    // The window reported is the meter's elapsedTime so far: a partial window on purpose, because
    // what it measured is what happened.
    function layoutProbeFlushScroll() {
        if (window.scrollStep === null)
            return
        window.scrollSeconds = selfTestScrollMeter.elapsedTime
        window.scrollWallMs = window.scrollT0 > 0 ? Date.now() - window.scrollT0 : 0
        window.scrollLogArmed = false
        window.scrollArmed = false
        window.layoutProbeLogScroll()
    }

    // A retrying armer: up to six attempts, 150 ms apart, because a list can be missing from the
    // tree for a moment right after its pane is switched in (measured: the bible list was not
    // found at 180 ms and was there a step later). Only after the attempts run out is a missing
    // name reported as the step's finding — a scroll that could not be set up is worth reporting,
    // but a race with a pane switch is not.
    function layoutProbeStartScroll() {
        if (window.scrollArmed || window.scrollStep === null)
            return   // this step's scroll is already running, or already reported
        const item = SelfTest.findItem(window.scrollListName)
        const found = item !== null && item !== undefined
        if (!found && ++window.scrollTries < 6)
            return   // 6 × 150 ms, still inside the step: a pane switch can block the thread for ~hundreds of ms here
        selfTestScrollStart.stop()
        window.scrollMeterReset()
        window.scrollPrevCounts = window.layoutProbeSnapshot()
        window.scrollFound = found
        if (!window.scrollFound) {
            window.layoutProbeLogScroll()
            return
        }
        window.scrollStartY = item.contentY
        const maxY = Math.max(0, (item.contentHeight || 0) - (item.height || 0))
        if (maxY <= 1) {
            window.layoutProbeLogScroll()
            return
        }
        // The shared animation is stopped first: if an earlier step's run is somehow still going,
        // that stop must not be read as this step's end (scrollLogArmed is false until the line
        // below), which is exactly how a stale stop once wrote a record with no scroll in it. An
        // earlier scroll's own record was written when this step was issued (see FlushScroll) —
        // it cannot be written here, because by now scrollStep IS this step.
        selfTestScrollAnim.stop()
        selfTestScrollAnim.target = item
        selfTestScrollAnim.from = window.scrollStartY
        selfTestScrollAnim.to = maxY
        window.scrollArmed = true
        window.scrollLogArmed = true
        selfTestScrollAnim.start()
    }

    // The scroll fields themselves — what the frame meter saw, and how far the list actually
    // moved. `listFound=0` means the step's objectName did not resolve (a broken name is the one
    // failure that would otherwise read exactly like a list too short to scroll, so it gets its
    // own field); `frames=0 listFound=1` with a real contentPx/viewPx means the list was there
    // and had nothing to scroll.
    function layoutProbeScrollFields() {
        const item = window.scrollFound ? SelfTest.findItem(window.scrollListName) : null
        const contentPx = item ? Math.round(item.contentHeight) : -1
        const viewPx = item ? Math.round(item.height) : -1
        const scrolled = item ? Math.round(item.contentY - window.scrollStartY) : 0
        const seconds = window.scrollSeconds
        const fps = (seconds > 0 && window.scrollFrames > 0) ? (window.scrollFrames / seconds) : 0
        const avg = window.scrollFrames > 0 ? (window.scrollMs / window.scrollFrames) : 0
        // The scrolled LIST's own row count where it has one (a view's `count`): the reference
        // panes' scrollRows fields are the sum of BOTH tabs' rows, so this is the unambiguous
        // "how long is the list that just scrolled" — -1 for the Flickable + Repeater sidebars,
        // where contentPx is the only length there is.
        const items = (item && item.count !== undefined) ? item.count : -1
        return " scrollList=" + window.scrollListName
               + " listFound=" + (window.scrollFound ? 1 : 0)
               + " scrollItems=" + items
               + " contentPx=" + contentPx + " viewPx=" + viewPx
               + " scrollPx=" + scrolled + " scrollMs=" + Math.round(seconds * 1000)
               + " scrollWallMs=" + Math.round(window.scrollWallMs)
               + " frames=" + window.scrollFrames
               + " fps=" + (Math.round(fps * 10) / 10)
               + " avgFrameMs=" + (Math.round(avg * 10) / 10)
               + " worstFrameMs=" + (Math.round(window.scrollWorstMs * 10) / 10)
               + " jank33=" + window.scrollJank33 + " jank50=" + window.scrollJank50
    }

    // The scroll's OWN record, written when the scroll stops rather than at the step timer's next
    // tick. A step whose list is slow to draw OVERRUNS the 1 s cadence (measured here: the
    // templates grid's 37 cards took 7.1 s of a 1 s step), and a record taken at the tick would
    // have been written before the scroll even began — which is exactly what the first cut of this
    // reported (listFound=0, frames=0, and then the scroll quietly starting afterwards). Its
    // counter deltas run from the snapshot taken at scroll START, so they are this scroll's
    // whatever the tick timer did in between.
    function layoutProbeLogScroll() {
        const step = window.scrollStep
        const now = window.layoutProbeSnapshot()
        EngineBridge.log("info", "LayoutProbe",
                         "phase=" + (step ? step.tag : "scroll-unknown")
                         + " scrollStart=1"
                         + window.layoutProbeScrollFields()
                         + window.layoutProbeScrolledFields(window.scrollPrevCounts, now, step))
        // The next step's line must measure its own step, not what the scroll left behind.
        window.layoutProbePrev = now
        // Only disarm the walk if this record IS the step the walk is on. A starved interface
        // thread delivers a scroll's stop LATE — after the tick has already issued the next step —
        // and clearing the arming unconditionally cancelled that next step's scroll entirely
        // (measured: two scripture scrolls wrote no record at all, because their predecessor's
        // late stop nulled the step they had just armed).
        if (window.layoutProbePendingStep === step)
            window.scrollStep = null
    }
    // The stall measurement itself: a timer on the interface thread asking for 16 ms. When the
    // thread is parked in DirectWrite, in a relayout, or in a binding storm, this is late by
    // exactly how long it was parked. Anything under 8 ms of overshoot is timer noise.
    Timer {
        id: selfTestLayoutHeartbeat
        interval: 16
        repeat: true
        running: typeof SelfTestLayout !== "undefined" && SelfTestLayout
        onTriggered: {
            const now = Date.now()
            const last = window.layoutProbeBeat
            window.layoutProbeBeat = now
            if (last <= 0)
                return
            const lost = (now - last) - 16
            if (lost < 8)
                return
            window.layoutProbeStallSum += lost
            if (lost > window.layoutProbeStallMax)
                window.layoutProbeStallMax = lost
        }
    }
    // The frame meter of the scroll phase: a FrameAnimation fires once per frame the app
    // renders, on the interface thread, so `frameTime` is the frame the app just produced. It
    // runs ONLY while a scroll step's animation does, so nothing else in the run pays for it.
    //
    // frameTime and elapsedTime are in SECONDS (Qt's own doc: "the time in seconds since the
    // previous frame update") — converted to ms once, here, and the window comes from the
    // meter's own elapsedTime rather than a second clock, so frames and duration are the same
    // measurement.
    //
    // The FIRST frame's frameTime is skipped: it is the gap since whatever was last painted, so
    // on an idle app it reads as seconds of "cost" that no scroll caused. The worst frame
    // reported is therefore the worst frame the scroll itself produced.
    FrameAnimation {
        id: selfTestScrollMeter
        running: window.scrollMeterOn
        onTriggered: {
            if (selfTestScrollMeter.currentFrame <= 1)
                return
            const dt = selfTestScrollMeter.frameTime * 1000
            window.scrollFrames++
            window.scrollMs += dt
            if (dt > window.scrollWorstMs)
                window.scrollWorstMs = dt
            if (dt > 33)
                window.scrollJank33++
            if (dt > 50)
                window.scrollJank50++
        }
    }
    // The scroll itself: contentY, start to end, at a constant speed (linear). A wheel drag with
    // no flick inertia — reproducible run to run, which is what makes two runs comparable.
    NumberAnimation {
        id: selfTestScrollAnim
        property: "contentY"
        duration: 700
        easing.type: Easing.Linear
        onStarted: {
            window.scrollT0 = Date.now()
            window.scrollMeterOn = true
        }
        onStopped: {
            window.scrollMeterOn = false
            window.scrollWallMs = window.scrollT0 > 0 ? Date.now() - window.scrollT0 : 0
            window.scrollSeconds = selfTestScrollMeter.elapsedTime
            if (window.scrollLogArmed) {
                window.scrollLogArmed = false
                window.scrollArmed = false
                window.layoutProbeLogScroll()
            }
        }
    }
    // A short beat between the pane switch and the scroll: the switch's own first frames are the
    // step before this one's cost, and a scroll started inside them would report both.
    Timer {
        id: selfTestScrollStart
        interval: 150
        repeat: true
        onTriggered: window.layoutProbeStartScroll()
    }
    Timer {
        id: selfTestLayoutProbe
        interval: 1000
        repeat: true
        running: typeof SelfTestLayout !== "undefined" && SelfTestLayout
        onTriggered: {
            const pending = window.layoutProbePendingStep
            const wasScroll = pending !== null && pending !== undefined && pending.scroll !== undefined
            // Report the step that was just issued (its list has settled by now). A scroll step is
            // NOT reported here: it writes its own record when the scroll stops (or when it turns
            // out it cannot scroll) — logging it here as well would print it twice, one of them
            // before the scroll had run.
            if (window.layoutProbePending !== "" && !wasScroll)
                EngineBridge.log("info", "LayoutProbe",
                                 window.layoutProbeLine(window.layoutProbePending,
                                                        Date.now() - window.layoutProbeT0))
            else if (wasScroll) {
                // …unless this step never got a record at all: a starved interface thread can hold
                // the beat timer past the whole step (measured: two scripture scrolls wrote nothing).
                // One record per scroll step, always — a scroll that could not even start is a
                // finding, not a gap.
                if (window.scrollStep !== null && !window.scrollArmed) {
                    EngineBridge.log("info", "LayoutProbe",
                                     window.layoutProbeLine(window.layoutProbePending,
                                                            Date.now() - window.layoutProbeT0))
                    selfTestScrollStart.stop()
                    window.scrollStep = null
                }
                // The next step's line still has to measure its own step, not what the scroll left
                // behind, so the baseline moves here even though nothing was printed.
                window.layoutProbePrev = window.layoutProbeSnapshot()
            }
            window.scrollMeterOn = false
            window.layoutProbeStallMax = 0
            window.layoutProbeStallSum = 0
            if (window.layoutProbeStep >= window.layoutProbeSteps.length) {
                const now = window.layoutProbeSnapshot()
                EngineBridge.log("info", "LayoutProbe",
                                 "phase=done showsRows=" + now.showsRows + " showsBuilt=" + now.showsBuilt
                                 + " mediaRows=" + now.mediaRows + " mediaBuilt=" + now.mediaBuilt
                                 + " overlayRows=" + now.overlayRows + " overlayBuilt=" + now.overlayBuilt
                                 + " templateRows=" + now.templateRows + " templateBuilt=" + now.templateBuilt
                                 + " verseRows=" + now.verseRows + " verseBuilt=" + now.verseBuilt
                                 + " hitBuilt=" + now.hitBuilt)
                SelfTest.quit()
                return
            }
            const step = window.layoutProbeSteps[window.layoutProbeStep++]
            window.layoutProbePending = step.tag
            window.layoutProbePendingStep = step
            window.layoutProbeT0 = Date.now()
            // The library dock has to be the view on screen for any of this to lay out
            // (see the presplit scenario's own note on the launch state).
            if (window.layoutProbeStep === 1)
                window.currentView = "show"
            if (step.pane !== undefined)
                showScreen.showInLibrary(step.pane, step.query === undefined ? "" : step.query)
            else if (step.body !== undefined) {
                // The pane's OWN search box (see the pane-highlight scenario): the magnifier's
                // click reveals it, then the query is typed into it.
                const pane = SelfTest.findItem(step.body === "table" ? "selfTestTablePane" : "selfTestScripturePane")
                if (pane && step.search === true)
                    pane.searching = true
                const box = SelfTest.findItem("selfTestSearchBox-" + step.body)
                if (box)
                    box.text = step.text
            }
            if (step.scroll !== undefined) {
                // Armed here, started by the beat timer below: the meter's totals are reset with
                // the step so the record that follows is this scroll's alone. A still-unrecorded
                // scroll from the previous step is written out first (see FlushScroll).
                window.layoutProbeFlushScroll()
                window.scrollListName = step.scroll
                window.scrollStep = step
                window.scrollFound = false
                window.scrollArmed = false
                window.scrollLogArmed = false
                window.scrollTries = 0
                window.scrollMeterReset()
                selfTestScrollStart.restart()
            }
        }
    }

    // The click has to actually LAND before it can be judged (a synthetic click needs the
    // window focused, which takes a moment), so this waits for the pick it should have
    // made instead of assuming it arrived.
    Timer {
        id: selfTestPreSplitClicked
        interval: 250
        repeat: true
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            const row = window.preSplitOpenRow
            const name = window.preSplitRowName(row)
            // Click ONCE THE ROW IS ON SCREEN — the reveal above is a scroll, and a
            // scroll takes a frame or two before the row exists to be clicked. Waiting
            // for itemVisible (rather than clicking blind) is also what rejects a
            // RECYCLED row of the same name: the list keeps the rows it has finished
            // with in a reuse pool, and those are invisible.
            if (!window.preSplitRowReady) {
                if (!SelfTest.itemVisible(name)) {
                    // BOUNDED, and it says what it is waiting on: a row that never came on
                    // screen used to park here for ever, which reads as a hanging run
                    // instead of a failure. 240 ticks is a minute; the wait itself is the
                    // reveal being a scroll (a frame or two), so anything long is a
                    // finding, not patience.
                    const waited = ++window.preSplitWaitTicks
                    if (waited % 40 === 0)
                        console.log("[SELFTEST-PRESPLIT] still waiting for " + name
                                    + " to be on screen: model=" + p.mainRows.length
                                    + " rows=" + p.chapterVerses.length
                                    + " rowAt=" + p.mainRows.findIndex((r) => Number(r.number) === Number(row.number)
                                                                       && Number(r.partIndex ?? 0) === Number(row.partIndex ?? 0))
                                    + " paneVisible=" + p.visible
                                    + " paneSize=" + Math.round(p.width) + "x" + Math.round(p.height)
                                    + " row1Visible=" + SelfTest.itemVisible("selfTestVerseRow_1")
                                    + " exists=" + (SelfTest.findItem(name) ? 1 : 0)
                                    + " selected=" + p.selected.join(","))
                    if (waited > 240) {
                        selfTestPreSplitClicked.stop()
                        console.log("[SELFTEST-PRESPLIT] result rows=" + window.preSplitFacts.rows
                                    + " splitRows=" + window.preSplitFacts.split
                                    + " openMs=" + window.preSplitFacts.openMs
                                    + " openSplitRows=" + window.preSplitFacts.openSplit
                                    + " finalSplitRows=" + window.preSplitFacts.finalSplit
                                    + " settledMs=" + window.preSplitFacts.settledMs
                                    + " agreement=false notclicked=" + name)
                        window.preSplitRestoreSettings()
                        SelfTest.quit()
                    }
                    return
                }
                SelfTest.clickItem(name)
                window.preSplitRowReady = true
                return   // judged from the next tick, once the click has arrived
            }
            if (p.selected.indexOf(Number(row.number)) < 0 && window.preSplitClickTries++ < 60)
                return
            selfTestPreSplitClicked.stop()
            let part = -1
            for (const piece of p.previewSlideParts || [])
                if (Number(piece.number) === Number(row.number))
                    part = Number(piece.partIndex ?? 0)
            const agreed = p.selected.indexOf(Number(row.number)) >= 0 && part === Number(row.partIndex)
            console.log("[SELFTEST-PRESPLIT] clicked " + window.preSplitRowName(row)
                        + " selected=" + p.selected.join(",") + " previewPart=" + part)
            window.preSplitRestoreSettings()
            console.log("[SELFTEST-PRESPLIT] result rows=" + window.preSplitFacts.rows
                        + " splitRows=" + window.preSplitFacts.split
                        + " openMs=" + window.preSplitFacts.openMs
                        + " openSplitRows=" + window.preSplitFacts.openSplit
                        + " finalSplitRows=" + window.preSplitFacts.finalSplit
                        + " settledMs=" + window.preSplitFacts.settledMs
                        + " pickedBefore=" + (window.preSplitFacts.hadPick ? 1 : 0)
                        + " clickedPart=" + Number(row.partIndex)
                        + " previewPart=" + part
                        + " agreement=" + agreed)
            SelfTest.quit()
        }
    }

    // ---- app-wide search ranking (VGR_RANK_TEST=1) --------------------------
    // User report: "compelling that you recieve" listed Bible verses that carry
    // only "you that", with the sermon that says "and compelling and compulsory,
    // that you receive the Holy Ghost" nowhere near the top — while the same
    // search leads with that sermon for "compelling and compulsory". This
    // scenario prints the rows the dialog ACTUALLY shows, in order, with how many
    // of the typed words each one carries, so the ranking is readable from a log
    // rather than a screenshot.
    readonly property string rankQuery: "compelling that you recieve"
    // The diagnostic's own async search request counter (see the read timer below).
    property int diagSearchToken: 0
    // The hover stage's own state (see rankStartHover): which row the real cursor
    // was parked on, and what its preview read.
    property var rankHover: ({ index: -1, kind: "-", chars: -1 })
    // The verse-hitting query the hover stage falls back to (see rankStartHover):
    // the ranked query is a user report's wording, and this machine's run of it
    // returned sermons only, while the preview under test is the VERSE one.
    readonly property string rankHoverQuery: "holy ghost"
    property bool rankHoverResearched: false
    property int rankHoverTries: 0
    function rankWords() {
        const source = quickSearch.highlightSource !== "" ? quickSearch.highlightSource : window.rankQuery
        return source.toLowerCase().match(/[a-z0-9]+/g) || []
    }
    // "<kind>(<words carried>/<query words>) <title>" per row, in display order.
    function rankDescribe(results) {
        const words = window.rankWords()
        const out = []
        for (let i = 0; i < results.length && i < 8; ++i) {
            const r = results[i]
            const hay = [r.title, r.subtitle, r.text, r.fullText]
                            .map((v) => String(v ?? "")).join(" ").toLowerCase()
            let hits = 0
            for (const w of words)
                if (new RegExp("\\b" + w + "\\b").test(hay)) hits++
            // The rating the row SHOWS ("MATCH n%", the ranking key) next to the
            // raw count of typed words it carries, so the log shows both the order
            // and the number behind it — the column must never go UP.
            out.push(r.kind + "(" + hits + "/" + words.length + ") match"
                     + quickSearch.matchRating(r) + "% " + String(r.title).substring(0, 40))
        }
        return out
    }
    Timer {
        id: selfTestRank
        running: typeof SelfTestRank !== "undefined" && SelfTestRank
        interval: 45000   // boot + the background sermon indexing
        onTriggered: {
            quickSearch.openSearch()
            quickSearch.query = window.rankQuery
            quickSearch.refresh()
            selfTestRankRead.restart()
        }
    }
    Timer {
        id: selfTestRankRead
        interval: 9000
        onTriggered: {
            const rows = window.rankDescribe(quickSearch.results)
            console.log("[SELFTEST-RANK] query=" + window.rankQuery + " resolved="
                        + quickSearch.highlightSource + " rows=" + quickSearch.results.length)
            // Diagnostics: is the DIALOG holding the query at all, and does the
            // SERVICE answer the same query? (rows=0 for every query in an
            // earlier run made it impossible to tell a ranking bug from a
            // search that never ran.)
            console.log("[SELFTEST-RANK] diag query='" + quickSearch.query + "' pending="
                        + quickSearch.searchPending + " bibleLoading=" + SearchService.bibleLoading
                        + " bibles=" + SearchService.bibles.length)
            for (let i = 0; i < rows.length; ++i)
                console.log("[SELFTEST-RANK] row" + i + "=" + rows[i])
            const first = quickSearch.results.length > 0 ? quickSearch.results[0] : null
            console.log("[SELFTEST-RANK] result top=" + (first ? first.kind : "-")
                        + " topTitle=" + (first ? String(first.title).substring(0, 60) : "-"))
            // The "does the SERVICE answer the same query" half of the
            // diagnostic, asked ASYNCHRONOUSLY. This line used to carry
            // `syncRows=` from a synchronous SearchService.search() inside a
            // console.log — the whole aggregation (Bible reference, the shows
            // library, The Table's paragraph scan) on the interface thread, to
            // print a number. It froze the very thread the scenario exists to
            // measure. The service's own answer now arrives on resultsReady
            // (below) and is logged there; the wait is bounded either way, so a
            // service that never answers still ends the scenario.
            // The SERVICE half of the diagnostic, and the hover stage, both wait
            // for their own answers now (see rankDiagCall / rankStartHover).
            window.rankStartHover()
        }
    }
    // THE HOVER PREVIEW, parked and read without ever leaving the row. The trap this
    // covers: SearchService.fullVerse is a PUBLISHED read — it answers "" while its
    // worker read is in flight — and the dialog had no verseCached handler at all,
    // so a row that took that empty answer showed only its snippet for as long as
    // the pointer stayed on it (leaving and coming back worked, which is what made
    // it easy to miss). One REAL cursor move, then two reads with the pointer
    // unmoved: probe1 is the verdict, probe2 the confirmation.
    // The first row of the current results that has a LOCATION (a Bible verse or a
    // sermon paragraph) — `wantBible` restricts it to SearchService's own published
    // read, which is the one this check is about.
    function rankPickRow(wantBible) {
        const results = quickSearch.results
        for (let i = 0; i < results.length; ++i) {
            const r = results[i]
            const located = (r.kind === "bible" || r.kind === "table")
                             && r.bookId !== undefined && r.bookId !== ""
            // NOTE: every Bible verse row already carries its own verse text —
            // readSearch seeds `fullText` from the engine — so a candidate cannot be
            // narrowed to "one whose preview will be fetched". The published read is
            // asked on hover regardless, and the seed is what the row shows while its
            // answer is in flight (see the entry's fullText binding).
            if (located && (!wantBible || r.kind === "bible"))
                return i
        }
        return -1
    }
    function rankStartHover() {
        let pick = window.rankPickRow(true)
        if (pick < 0 && !window.rankHoverResearched) {
            // The ranked query returned no verse of its own: ASK FOR ONE, because a
            // sermon paragraph previews through the library while a verse previews
            // through the published read under test. The ranked query's rows are
            // already logged and are not logged again, so the harness's ranking
            // checks still read exactly the rows they did before.
            window.rankHoverResearched = true
            window.rankHoverTries = 0
            quickSearch.query = window.rankHoverQuery
            quickSearch.refresh()
            selfTestRankHoverQuery.restart()   // then wait for the ROWS, not a clock
            return
        }
        if (pick < 0)
            pick = window.rankPickRow(false)   // no Bible in this setup: any located row
        window.rankHover = ({ index: pick, kind: pick >= 0 ? String(quickSearch.results[pick].kind) : "-",
                              chars: -1 })
        if (pick < 0) {
            // Say WHAT came back, not just that nothing did: an empty list (the query
            // answered nothing), rows with no location, and rows that already carry
            // their passage are three different reasons for the same silence.
            const rs = quickSearch.results
            let shape = ""
            for (let i = 0; i < rs.length && i < 4; ++i)
                shape += " [" + i + ":" + rs[i].kind + " book='" + String(rs[i].bookId ?? "")
                         + " full=" + String(rs[i].fullText ?? "").length + "]"
            console.log("[SELFTEST-RANK] hover none — no located row came back for '"
                        + window.rankHoverQuery + "' either (rows=" + rs.length + shape
                        + " bibles=" + SearchService.bibles.length + ")")
            window.rankDiagCall()
            return
        }
        const c = SelfTest.itemCenter("selfTestQuickRow" + pick)
        SelfTest.move(c.x, c.y)   // a genuine OS hover, the same shape the cursor test uses
        selfTestRankHover.restart()
    }
    // Reads the hovered row's own preview off its item — a length, not a boolean, so
    // the log says WHICH answer the row took (a snippet is tens of characters).
    function rankHoverProbe(tag) {
        const item = SelfTest.findItem("selfTestQuickRow" + window.rankHover.index)
        const text = item ? String(item.fullText ?? "") : ""
        // Did the hover LAND? The row says so the same way the UI does (its own
        // handler moves the dialog's selection), and a preview read off a row the
        // pointer is not on proves nothing about the re-ask — it would just be the
        // row's seed. Read here, at probe time, not right after the move: the OS
        // hover event is delivered a beat later.
        const landed = quickSearch.current === window.rankHover.index
        // The row's own seed, so the log says whether the preview came from the
        // published read or from the passage the search already handed the row.
        const seeded = String(quickSearch.results[window.rankHover.index].fullText ?? "").length
        console.log("[SELFTEST-RANK] hover " + tag + " row=" + window.rankHover.index
                    + " kind=" + window.rankHover.kind + " landed=" + landed
                    + " chars=" + text.length + " seeded=" + seeded
                    + " filled=" + (text.length > 0))
        return text.length
    }
    function rankDiagCall() {
        window.diagSearchToken++
        SearchService.searchAsync(window.rankQuery, 4, window.diagSearchToken, false)
        selfTestRankDiagWait.restart()
    }
    // Wait for the fallback query's ROWS rather than for a clock: refresh() clears
    // the list and sets searchPending, and a pick taken while it is empty (or while
    // the old rows are still up) would either find nothing or park the cursor on a
    // delegate the arriving answer is about to rebuild — which re-asks the preview
    // by itself and would let this check pass without the re-ask it exists for.
    Timer {
        id: selfTestRankHoverQuery
        interval: 1000
        repeat: true
        onTriggered: {
            if (quickSearch.searchPending)
                return                              // the answer is not in yet
            if (++window.rankHoverTries < 8) {
                selfTestRankHoverQuery.stop()
                window.rankStartHover()
                return
            }
            // Gave up waiting (a Bible still loading at 45 s is the likely reason):
            // say so and let the scenario finish, so the harness reports a missing
            // row rather than a timeout with no line explaining it.
            selfTestRankHoverQuery.stop()
            console.log("[SELFTEST-RANK] hover none — '" + window.rankHoverQuery
                        + "' returned no rows within " + window.rankHoverTries + "s")
            window.rankDiagCall()
        }
    }
    Timer {
        id: selfTestRankHover
        interval: 4000
        onTriggered: {
            window.rankHoverProbe("probe1")
            selfTestRankHoverAgain.restart()
        }
    }
    Timer {
        id: selfTestRankHoverAgain
        interval: 3000
        onTriggered: {
            const chars = window.rankHoverProbe("probe2")
            console.log("[SELFTEST-RANK] hover result filled=" + (chars > 0))
            window.rankDiagCall()
        }
    }
    Timer {
        id: selfTestRankDiagWait
        interval: 5000
        onTriggered: {
            console.log("[SELFTEST-RANK] serviceRows=timeout (no answer within 5s)")
            SelfTest.quit()
        }
    }
    Connections {
        target: SearchService
        function onResultsReady(token, rows) {
            if (token !== window.diagSearchToken)
                return
            console.log("[SELFTEST-RANK] serviceRows=" + rows.length
                        + " (the service's own answer for the same query)")
            selfTestRankDiagWait.stop()
            SelfTest.quit()
        }
    }

    // ---- The show document JOBS: save / open / peek / timer / copy ----------
    // (VGR_SHOWIO_TEST=1 only.) ShowService's document reads and writes are QUEUED
    // (see its header): each answers with a TOKEN and reports its verdict later, as
    // documentJobFinished. Neither compilation nor a clean boot can tell that
    // contract from a broken one — a save that never writes its file and a save
    // whose verdict never arrives both look exactly like nothing happening. So this
    // drives the whole chain with REAL files and prints every verdict:
    //   save   the working show to a file, then read back dirty/file/slides;
    //   open   that file back through the call the show screen uses;
    //   peek   it the way the centre page does — a miss must answer `pending`, the
    //          answer must land on the published revision, and a WRITE to it must
    //          drop that answer so the next read is the new file (the timer);
    //   timer  apply it, then read the peek again and expect the timer in it;
    //   copy   the recovery copy, the one write nobody asked for.
    property var showIo: ({ savePath: "showio_probe.vgr", copyPath: "showio_probe_recovery.vgr",
                            saveToken: 0, openToken: 0, timerToken: 0, copyToken: 0,
                            saveOk: false, openOk: false, peekOk: false, timerSeen: false,
                            timerPeekPending: false })
    function showIoLog(step, text) { console.log("[SELFTEST-SHOWIO] step=" + step + " " + text) }
    function showIoBegin() {
        // Two slides, then straight to the save: a new show is dirty until it has a file.
        ShowService.newShowDocument("ShowIO probe")
        ShowService.addSlide({ title: "Probe one" })
        ShowService.addSlide({ title: "Probe two" })
        window.showIo.saveToken = ShowService.saveCurrentShow(window.showIo.savePath)
    }
    function showIoSaveLanded(ok) {
        window.showIo.saveOk = ok
        const show = ShowService.currentShow
        window.showIoLog("save", "ok=" + ok + " path='" + String(ShowService.showPath) + "'"
                         + " dirty=" + ShowService.showDirty
                         + " file=" + SettingsService.fileExists(window.showIo.savePath)
                         + " slides=" + (show.slides ? show.slides.length : -1))
        window.showIo.openToken = ShowService.openShowFile(window.showIo.savePath)
    }
    function showIoOpenLanded(ok, slides) {
        window.showIo.openOk = ok
        window.showIoLog("open", "ok=" + ok + " slides=" + slides)
        const first = ShowService.peekShow(window.showIo.savePath)
        window.showIoLog("peek1", "ok=" + first.ok + " pending=" + (first.pending === true))
        selfTestShowIoPeek.restart()
    }
    function showIoPeekLanded() {
        const rev = ShowService.peekRevision
        const second = ShowService.peekShow(window.showIo.savePath)
        const slides = second.show ? second.show.slides : []
        window.showIo.peekOk = second.ok === true && slides.length > 0
        window.showIoLog("peek2", "ok=" + second.ok + " slides=" + slides.length + " revision=" + rev)
        window.showIo.timerToken = ShowService.setNextTimer(window.showIo.savePath, 5)
    }
    function showIoTimerLanded(ok) {
        window.showIoLog("timer", "ok=" + ok)
        // The timer edit is a WRITE to the show: the published peek of it must be
        // gone, so this read is either a fresh miss or the new answer — never the
        // old one.
        const after = ShowService.peekShow(window.showIo.savePath)
        window.showIo.timerPeekPending = (after.pending === true)
        window.showIoLog("peek3", "ok=" + after.ok + " pending=" + (after.pending === true))
        selfTestShowIoPeekAgain.restart()
    }
    function showIoTimerReadLanded() {
        const last = ShowService.peekShow(window.showIo.savePath)
        const slides = last.show ? last.show.slides : []
        const applied = slides.length > 0 && slides.every((s) => Math.round(s.nextTimer ?? 0) === 5)
        window.showIo.timerSeen = applied
        window.showIoLog("peek4", "ok=" + last.ok + " slides=" + slides.length + " timerApplied=" + applied)
        window.showIo.copyToken = ShowService.saveShowCopy(window.showIo.copyPath)
    }
    function showIoCopyLanded(ok) {
        window.showIoLog("copy", "ok=" + ok + " file=" + SettingsService.fileExists(window.showIo.copyPath))
        window.showIoLog("result", "save=" + window.showIo.saveOk + " open=" + window.showIo.openOk
                         + " peek=" + window.showIo.peekOk + " timer=" + window.showIo.timerSeen
                         + " timerPeekPending=" + window.showIo.timerPeekPending + " copy=" + ok)
        // Quit from its OWN tick, never from inside the job-verdict handler.
        // MEASURED, not assumed: with `SelfTest.quit()` called straight from this
        // handler the interface thread's 5 s heartbeat (engine.log,
        // `UI event-loop heartbeat at … ms`) kept ticking for the ~80 s the probe
        // watched after the verdict — so the loop never returned — while the same
        // call from this timer returns it within a second (engine.log's
        // `app event loop returned with code 0`). The verdict is delivered on a
        // QUEUED call from the engine worker, and a quit issued inside that
        // delivery is what does not take; the hop is the shape every other
        // scenario here already quits through, so nothing new is being asked of
        // the app for this one.
        selfTestShowIoQuit.restart()
    }
    Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok, error, payload) {
            if (!(typeof SelfTestShowIo !== "undefined" && SelfTestShowIo))
                return
            if (kind === "save" && token === window.showIo.saveToken)
                window.showIoSaveLanded(ok)
            else if (kind === "open" && token === window.showIo.openToken)
                window.showIoOpenLanded(ok, (payload && payload.show && payload.show.slides) ? payload.show.slides.length : -1)
            else if (kind === "timer" && token === window.showIo.timerToken)
                window.showIoTimerLanded(ok)
            else if (kind === "saveCopy" && token === window.showIo.copyToken)
                window.showIoCopyLanded(ok)
        }
    }
    Timer {
        id: selfTestShowIo
        running: typeof SelfTestShowIo !== "undefined" && SelfTestShowIo
        interval: 18000   // boot, then the engine's stores
        onTriggered: window.showIoBegin()
    }
    Timer { id: selfTestShowIoPeek; interval: 2500; onTriggered: window.showIoPeekLanded() }
    Timer { id: selfTestShowIoPeekAgain; interval: 2500; onTriggered: window.showIoTimerReadLanded() }
    Timer { id: selfTestShowIoQuit; interval: 500; onTriggered: SelfTest.quit() }

    // ---- The QUIT path: unsaved show + a live output + the window close ------
    // (VGR_QUIT_TEST=1 only.) Closing the window is the one user path that ends the
    // process, and no build can see it: the unsaved-changes guard has to REFUSE the
    // first close (or the user loses the show), the answer has to close it for real
    // (or the app never quits), the save that gates that close is a QUEUED document
    // job whose verdict decides whether the window may go, and an output is LIVE
    // while the teardown runs — the case every "it hangs when I close it" report is
    // about. This drives that chain and prints each step; the harness
    // (tools/selftest/run_quit_test.sh) reads these lines, the engine's own `Exit`
    // timings for each aboutToQuit step, and how long the process then took to go.
    property var quitProbe: ({ seedToken: 0, seeded: false, armed: false, refused: false,
                               dialogUp: false, saveClicked: false, live: false })
    function quitLog(step, text) { console.log("[SELFTEST-QUIT] step=" + step + " " + text) }
    function quitBegin() {
        // A show with a FILE to begin with: the close-time save must not need the
        // native save-path picker (a modal OS dialog cannot be driven from here),
        // and being dirty again afterwards is what makes the guard speak.
        ShowService.newShowDocument("Quit probe")
        ShowService.addSlide({ title: "Saved one" })
        ShowService.addSlide({ title: "Saved two" })
        window.quitProbe.seedToken = ShowService.saveCurrentShow("quit_probe.vgr")
        window.quitLog("seed", "token=" + window.quitProbe.seedToken)
    }
    function quitSeeded(ok) {
        window.quitProbe.seeded = ok
        window.quitLog("seeded", "ok=" + ok + " file=" + SettingsService.fileExists("quit_probe.vgr"))
        // Unsaved again, and an output that is really live when the teardown starts.
        ShowService.addSlide({ title: "Unsaved" })
        LiveOutputService.goLive()
        window.quitProbe.live = LiveOutputService.live
        window.quitProbe.armed = true
        window.quitLog("armed", "hasShow=" + ShowService.hasShow + " dirty=" + ShowService.showDirty
                       + " live=" + LiveOutputService.live + " path='" + ShowService.showPath + "'")
        selfTestQuitClose.restart()
    }
    function quitClose() {
        // The real path: Window.close() runs the window's onClosing guard, which is
        // where "save changes?" lives (see onClosing in this file).
        window.close()
        window.quitProbe.refused = window.visible === true && window.closeConfirmed === false
        window.quitProbe.dialogUp = unsavedDialog.shown === true
        window.quitLog("close1", "visible=" + window.visible + " refused=" + window.quitProbe.refused
                       + " dialog=" + window.quitProbe.dialogUp)
        selfTestQuitAnswer.restart()
    }
    function quitAnswer() {
        // The dialog FADES IN (ModalCard's opacity behavior, 130 ms), and a card at
        // opacity 0 is not a visible item — so the button probe belongs here, after
        // the fade, not in the line that closed the window. MEASURED: probing it
        // there read `saveButton=false` on a dialog that was up and clickable.
        window.quitProbe.saveClicked = SelfTest.itemVisible("unsavedChangesDialog_save")
        window.quitLog("answer", "how=click saveButton=" + window.quitProbe.saveClicked
                       + " saving=" + unsavedDialog.saving)
        // Answer it the way the user does — a real synthetic click on the dialog's
        // Save. Logged BEFORE the click, deliberately: when the click lands, the
        // verdict that follows closes the window and the app is gone before any
        // later step could report (measured: the next timer never fired), so the
        // only safe place for this scenario's "I answered" evidence is here.
        SelfTest.clickItem("unsavedChangesDialog_save")
        selfTestQuitAnswered.restart()
    }
    function quitAnswered() {
        // A synthetic click needs the window to be foreground and can simply not
        // land (run_rank_test.sh's hover check has the same limitation and says so
        // rather than pretending). If the dialog is still up, answer through the one
        // function its Save button runs — and SAY which way it was answered: a
        // fallback taken silently is how a button that does nothing reads as a pass.
        if (unsavedDialog.shown) {
            window.quitLog("fallback", "how=confirm (the click did not land) saving=" + unsavedDialog.saving)
            unsavedDialog.confirm()
            selfTestQuitConfirmed.restart()
            return
        }
        window.quitLog("closed", "the dialog is gone — the answer's save landed and closed the window")
    }
    function quitConfirmed() {
        // Only reached when the click did not land and the dialog answered instead:
        // the queued save's verdict is what closes the window, so this says whether
        // that happened rather than assuming it.
        window.quitLog("after", "dialogVisible=" + unsavedDialog.shown
                       + " closeConfirmed=" + window.closeConfirmed + " visible=" + window.visible)
    }
    Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok) {
            if (!(typeof SelfTestQuit !== "undefined" && SelfTestQuit))
                return
            if (kind !== "save")
                return
            if (token === window.quitProbe.seedToken) {
                window.quitSeeded(ok)
            } else if (window.quitProbe.armed) {
                // The close-time save's verdict, the one that gates the close.
                window.quitLog("saved", "ok=" + ok + " file=" + SettingsService.fileExists("quit_probe.vgr")
                               + " closeConfirmed=" + window.closeConfirmed)
            }
        }
    }
    Timer {
        id: selfTestQuit
        running: typeof SelfTestQuit !== "undefined" && SelfTestQuit
        interval: 18000   // boot, then the engine's stores
        onTriggered: window.quitBegin()
    }
    Timer { id: selfTestQuitClose; interval: 900; onTriggered: window.quitClose() }
    Timer { id: selfTestQuitAnswer; interval: 900; onTriggered: window.quitAnswer() }
    Timer { id: selfTestQuitAnswered; interval: 1500; onTriggered: window.quitAnswered() }
    Timer { id: selfTestQuitConfirmed; interval: 1500; onTriggered: window.quitConfirmed() }
    Timer {
        id: selfTestQuitWatchdog
        running: typeof SelfTestQuit !== "undefined" && SelfTestQuit
        interval: 45000
        onTriggered: {
            window.quitLog("timeout", "the window never closed — quitting so the harness reports the hang")
            SelfTest.quit()
        }
    }

    // ---- GO LIVE latency as the load grows -----------------------------------
    // (VGR_LIVELAT_TEST=1 only.) "Going live takes a few seconds, and it gets worse
    // as the service goes on" is a latency report with a SHAPE: what matters is not
    // one go-live but the trend as slides and the per-slide blocks pile up. So this
    // drives a LADDER of live sessions, each bigger than the last, and measures each
    // one the way the operator pays for it:
    //
    //   first frame  GO LIVE -> the live preview's first new frame (that is the
    //                moment something is on the monitor; the LOOP's own publish is
    //                what is read, so this is the machine's time, not a timer's)
    //   cadence      frames the live loop DELIVERS per second while the on-air
    //                slide is being advanced on a fixed beat
    //
    // Three dimensions grow, because all three cost the loop per frame: SLIDES and
    // the per-slide LAYERS (SceneBuilder composites every block), and the OUTPUTS
    // (the loop renders one extra pass per enabled roster row — a rung asking for
    // more enabled screens than the roster has adds them, and the ladder puts the
    // operator's roster back when it is done).
    //
    // The measure has to be read from the engine, not from the log: the loop only
    // rasterizes when the on-air scene CHANGES (a static verse re-rendered at 60Hz
    // burned a core — see LiveOutputController's change-driven note), a static scene
    // is served by a 1Hz heartbeat, and the log's own frame line is emitted on state
    // CHANGE rather than per frame. That is MEASURED, not assumed: driving the same
    // ladder with a 3 s beat delivered 7-8 frames per 8 s window — the heartbeat,
    // with the two changes it was asked for buried in it — while a 150 ms beat
    // delivered the same count, i.e. the iterations the loop actually completed.
    // Hence the window's own mode (see liveLatTakeOverlay): an OVERLAY on air
    // bypasses both the skip and the heartbeat, so the loop rasterizes every
    // iteration it can and the delivered rate IS the loop's throughput. The
    // slide-advance fallback asks for changes far faster than the loop can serve, so
    // the frames that come back are again the iterations it completed. Either way
    // the number is the rate that falls when a frame costs more to build (more
    // blocks to composite, more outputs to render).
    //
    // run_live_latency_test.sh reads the `measured` lines below and fails when the
    // worst first frame or the lowest cadence crosses its budget.
    property var liveLat: ({ step: 0, firstFrameMs: -1 })
    function liveLatLog(step, text) { console.log("[SELFTEST-LIVELAT] step=" + step + " " + text) }
    // The knobs sit in main.cpp beside the rest of the scenario switches:
    // VGR_LIVELAT_LADDER ("slides:blocks:outputs," repeats), VGR_LIVELAT_DRIVE_MS
    // (how often a change is asked for while measuring), VGR_LIVELAT_WINDOW_MS (how
    // long each rung is measured for).
    function liveLatDriveMs() {
        const v = typeof SelfTestLiveLatDriveMs !== "undefined"
                ? parseInt(String(SelfTestLiveLatDriveMs), 10) : 0
        return v > 0 ? v : 150
    }
    function liveLatWindowMs() {
        const v = typeof SelfTestLiveLatWindowMs !== "undefined"
                ? parseInt(String(SelfTestLiveLatWindowMs), 10) : 0
        return v > 0 ? v : 8000
    }
    function liveLatLadder() {
        const raw = String(typeof SelfTestLiveLatLadder !== "undefined" ? SelfTestLiveLatLadder : "")
        const rungs = []
        raw.split(",").forEach((part) => {
            const bits = part.split(":")
            const slides = parseInt(bits[0], 10)
            if (slides > 1)   // a one-slide rung has nothing to advance (see below)
                rungs.push({ slides: slides, blocks: parseInt(bits[1], 10) || 1,
                             outputs: parseInt(bits[2], 10) || 1 })
            else if (slides > 0)
                console.log("[SELFTEST-LIVELAT] skip rung " + part
                            + " — a one-slide rung cannot be advanced, so its cadence"
                            + " would read as the static heartbeat, not the loop's cost")
        })
        return rungs.length > 0 ? rungs
                                : [ { slides: 2, blocks: 2, outputs: 1 },
                                    { slides: 8, blocks: 3, outputs: 1 },
                                    { slides: 20, blocks: 5, outputs: 1 },
                                    { slides: 20, blocks: 5, outputs: 4 } ]
    }
    // ---- the OUTPUT dimension: the roster is the operator's, not this run's -----
    // What an output COSTS the loop is ONE MORE RENDER PASS PER FRAME, and the
    // engine only renders a pass for an ENABLED, STYLED row
    // (OutputListModel::pushEngineStyle pushes exactly those). So the count that
    // matters is enabled+styled, and a row is added here by DUPLICATING a styled
    // one — the copy carries that style and starts enabled and off air (no window
    // opens, nothing is sent). This is not a detail: the first version of this
    // scenario added plain rows, which are styled with nothing, so the "more
    // outputs" rung measured the identical work as the rung below it and its
    // flat result would have read as "outputs are free".
    function liveLatStyledRows() {
        let n = 0
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const row = OutputListModel.getOutput(i)
            if (row.isEnabled === true && String(row.styleId).length > 0)
                ++n
        }
        return n
    }
    function liveLatStyleSource() {
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const row = OutputListModel.getOutput(i)
            if (row.isEnabled === true && String(row.styleId).length > 0)
                return i
        }
        return -1
    }
    function liveLatEnsureOutputs(want) {
        const st = window.liveLat
        if (st.baseRows === undefined) {
            st.baseRows = OutputListModel.rowCount()
            st.added = []
        }
        // Drop this run's own rows FIRST when the rung asks for fewer than a
        // previous rung left behind: `outputs=` is then the rung's own number
        // rather than an accumulation (a two-rung probe with the outputs rung
        // first reported 4 for both, which would have hidden the comparison).
        while (st.added.length > 0 && window.liveLatStyledRows() > want)
            window.liveLatDropAdded()
        while (want > 1 && window.liveLatStyledRows() < want && st.added.length < 8) {
            const src = window.liveLatStyleSource()
            if (src < 0)
                break   // no styled row to copy: an added row would render nothing
            OutputListModel.duplicateOutput(src)
            // The copy lands right after its source (not at the end), so it is named
            // here: that name is what this run owns and what it drops by, and it can
            // never collide with a row the operator made.
            const name = "LiveLat out " + (st.added.length + 1)
            OutputListModel.renameOutput(src + 1, name)
            st.added.push(name)
        }
        return window.liveLatStyledRows()
    }
    function liveLatDropAdded() {
        const st = window.liveLat
        const name = st.added.pop()
        if (name === undefined)
            return
        for (let i = OutputListModel.rowCount() - 1; i >= 0; --i) {
            if (OutputListModel.getOutput(i).name === name) {
                OutputListModel.removeOutput(i)
                return
            }
        }
    }
    function liveLatRestoreOutputs() {
        const st = window.liveLat
        if (st.baseRows === undefined)
            return
        // THE ROSTER PERSISTS (OutputListModel::saveRoster writes the store), so a
        // selftest that grows it has to shrink it back: exactly the rows this run
        // added come out, and the operator's own roster is what the next launch
        // reads.
        while (st.added.length > 0)
            window.liveLatDropAdded()
        while (OutputListModel.rowCount() > st.baseRows)   // belt and braces
            OutputListModel.removeOutput(OutputListModel.rowCount() - 1)
        window.liveLatLog("restored", "rows=" + OutputListModel.rowCount()
                          + " styled=" + window.liveLatStyledRows())
    }
    function liveLatRun() {
        const st = window.liveLat
        // The engine has to be up before a document can be built or put on air:
        // wait for the boot rather than guessing a delay (this scenario is the one
        // that runs LAST in a harness, but a cold start can still be slower than
        // any fixed interval).
        if (!EngineBridge.booted && st.step === 0) {
            window.liveLatLog("wait", "the engine has not finished booting")
            selfTestLiveLat.restart()
            return
        }
        const rungs = window.liveLatLadder()
        if (st.planned === undefined) {
            // The ladder it is actually going to drive (the harness reads this
            // count rather than parsing the env string it may have been given).
            st.planned = true
            window.liveLatLog("plan", "rungs=" + rungs.length
                              + " driveMs=" + window.liveLatDriveMs()
                              + " windowMs=" + window.liveLatWindowMs())
        }
        if (st.step >= rungs.length) {
            window.liveLatRestoreOutputs()
            window.liveLatLog("done", "cycles=" + rungs.length)
            selfTestLiveLatQuit.restart()
            return
        }
        const rung = rungs[st.step]
        st.step++
        // A fresh document at this rung's size, and the BLOCKS matter: they are
        // the per-frame work (SceneBuilder composites every one of them), so the
        // ladder grows the thing the render loop pays for, not just the slide
        // count.
        ShowService.newShowDocument("LiveLat " + rung.slides)
        for (let i = 0; i < rung.slides; ++i) {
            const slideId = ShowService.addSlide({ title: "Lat " + (i + 1) })
            for (let b = 0; b < rung.blocks; ++b) {
                ShowService.addBlock(slideId, { kind: "text", text: "block " + b,
                                                x: 60 + b * 40, y: 60 + b * 30,
                                                width: 700, height: 90 })
            }
        }
        st.slides = rung.slides
        st.blocks = rung.slides * rung.blocks
        const liveOuts = window.liveLatEnsureOutputs(rung.outputs)
        st.outputs = liveOuts
        // A rung that never gets a frame is reported as measured (firstFrameMs=-1),
        // not skipped: "it never went live" is the worst case of the number this
        // harness polices, and a rung that silently disappeared would read as a
        // pass with one fewer line.
        st.timeoutMs = 5000 + rung.slides * 500
        st.firstFrameMs = -1
        st.frames = 0
        st.changes = 0
        st.basePreview = LiveOutputService.previewFramesNow()
        st.t0 = Date.now()
        LiveOutputService.goLive()
        const press = Date.now() - st.t0
        window.liveLatLog("cycle", "n=" + st.step + " slides=" + st.slides
                          + " blocks=" + st.blocks + " outputs=" + st.outputs
                          + " driveMs=" + window.liveLatDriveMs()
                          + " windowMs=" + window.liveLatWindowMs()
                          + " goLiveCall=" + press + "ms live=" + LiveOutputService.live)
        selfTestLiveLatPoll.start()
    }
    function liveLatPoll() {
        const st = window.liveLat
        if (LiveOutputService.previewFramesNow() > st.basePreview) {
            window.liveLatFirstFrame()
            return
        }
        if (Date.now() - st.t0 > st.timeoutMs) {
            selfTestLiveLatPoll.stop()
            window.liveLatLog("noframe", "n=" + st.step + " slides=" + st.slides
                              + " blocks=" + st.blocks + " outputs=" + st.outputs
                              + " waitedMs=" + (Date.now() - st.t0)
                              + " live=" + LiveOutputService.live
                              + " frames=" + LiveOutputService.framesSentNow())
            window.liveLatLog("measured", "n=" + st.step + " slides=" + st.slides
                              + " blocks=" + st.blocks + " outputs=" + st.outputs
                              + " firstFrameMs=-1 frames=0"
                              + " changes=0 windowMs=0 fps=0.00 live=" + LiveOutputService.live
                              + " mode=none")
            window.liveLatStop()
        }
    }
    // THE WINDOW NEEDS THE LOOP TO HAVE WORK, and an overlay does that perfectly:
    // the live loop's change-driven skip and its 1Hz heartbeat are BOTH bypassed
    // while the compositor has live content (a taken input, media on air, or an
    // active overlay — see LiveOutputController::RenderOnce), so with one overlay on
    // air the loop rasterizes every iteration it can and the delivered rate IS the
    // loop's throughput. The slide-advance driver below only reaches the same thing
    // indirectly (it asks for changes faster than the loop serves them), so it is
    // the fallback for a machine whose overlay library is empty. Which one was used
    // is printed (`mode=`) and travels with the numbers, because the two do not
    // mean quite the same thing.
    function liveLatTakeOverlay() {
        const designs = OverlayLibraryService.designs()
        for (let i = 0; i < designs.length; ++i) {
            const d = designs[i]
            const id = String(d.id !== undefined ? d.id : (d.ref !== undefined ? d.ref : ""))
            if (id.length > 0) {
                LiveOutputService.takeOverlay(id, String(d.name !== undefined ? d.name : ""))
                return id
            }
        }
        return ""
    }
    function liveLatFirstFrame() {
        const st = window.liveLat
        selfTestLiveLatPoll.stop()
        st.firstFrameMs = Date.now() - st.t0
        st.overlayId = window.liveLatTakeOverlay()
        st.mode = st.overlayId.length > 0 ? "overlay" : "slides"
        st.baseFrames = LiveOutputService.framesSentNow()
        st.sampleStart = Date.now()
        st.changes = 0
        window.liveLatLog("firstframe", "n=" + st.step + " firstFrameMs=" + st.firstFrameMs)
        window.liveLatLog("mode", "n=" + st.step + " mode=" + st.mode
                          + (st.mode === "overlay" ? " overlay='" + st.overlayId + "'"
                                                    : " (no overlay design to take)"))
        if (st.mode === "slides") {
            selfTestLiveLatDrive.interval = window.liveLatDriveMs()
            selfTestLiveLatDrive.start()
        }
        selfTestLiveLatWindow.interval = window.liveLatWindowMs()
        selfTestLiveLatWindow.start()
    }
    function liveLatDrive() {
        const st = window.liveLat
        ++st.changes
        if (!LiveOutputService.next())
            LiveOutputService.jumpTo(0)
    }
    function liveLatWindow() {
        const st = window.liveLat
        selfTestLiveLatDrive.stop()
        const elapsed = Math.max(1, Date.now() - st.sampleStart)
        const frames = LiveOutputService.framesSentNow() - st.baseFrames
        st.frames = frames
        window.liveLatLog("measured", "n=" + st.step + " slides=" + st.slides
                          + " blocks=" + st.blocks + " outputs=" + st.outputs
                          + " firstFrameMs=" + Math.round(st.firstFrameMs)
                          + " frames=" + frames + " changes=" + st.changes
                          + " windowMs=" + elapsed
                          + " fps=" + (frames * 1000 / elapsed).toFixed(2)
                          + " live=" + LiveOutputService.live
                          + " mode=" + (st.mode !== undefined ? st.mode : "none"))
        window.liveLatStop()
    }
    function liveLatStop() {
        selfTestLiveLatPoll.stop()
        selfTestLiveLatDrive.stop()
        selfTestLiveLatWindow.stop()
        LiveOutputService.clearAllOverlays()   // this run took it; this run takes it back
        LiveOutputService.stop()
        window.liveLatLog("stopped", "n=" + window.liveLat.step)
        selfTestLiveLatNext.restart()
    }
    Timer {
        id: selfTestLiveLat
        running: typeof SelfTestLiveLat !== "undefined" && SelfTestLiveLat
        interval: 18000   // boot, then the engine's stores
        onTriggered: window.liveLatRun()
    }
    Timer { id: selfTestLiveLatPoll; interval: 25; repeat: true; onTriggered: window.liveLatPoll() }
    Timer { id: selfTestLiveLatDrive; repeat: true; onTriggered: window.liveLatDrive() }
    Timer { id: selfTestLiveLatWindow; onTriggered: window.liveLatWindow() }
    Timer { id: selfTestLiveLatNext; interval: 800; onTriggered: window.liveLatRun() }
    Timer { id: selfTestLiveLatQuit; interval: 500; onTriggered: SelfTest.quit() }

    // ---- The Scripture pane's columns, as the app really fills them -----------
    // (VGR_SCRIPTURE_TEST=1 only.) User report, looking at the Scripture tab: the
    // Bibles column lists the installed Bible, and the books / chapters / verses
    // columns stay EMPTY — with no error anywhere, because an empty column is the
    // same pixels whether the outline read is still in flight, was declined, or
    // came back empty. Those three are very different faults, so this prints the
    // pane's own state AND what the service answers for the same Bible id, twice:
    // once as the tab comes up, and once after the pane re-asks exactly the way a
    // tab switch makes it (syncSources). A pane that shows books on the SECOND
    // sample but not the first is a notify that never reached it; one that shows
    // none on either is the read itself.
    function scriptureProbe(tag) {
        const p = SelfTest.findItem("selfTestScripturePane")
        if (!p) {
            console.log("[SELFTEST-SCRIPTURE] " + tag + " pane missing")
            return
        }
        const id = String(p.sourceId ?? "")
        console.log("[SELFTEST-SCRIPTURE] " + tag
                    + " sources=" + (p.sources ? p.sources.length : -1)
                    + " sourceId='" + id + "'"
                    + " paneBooks=" + (p.books ? p.books.length : -1)
                    + " visibleBooks=" + (p.visibleBooks ? p.visibleBooks.length : -1)
                    + " chapterVerses=" + (p.chapterVerses ? p.chapterVerses.length : -1)
                    + " serviceBibles=" + ScriptureService.bibles.length
                    + " serviceBooks=" + (id !== "" ? ScriptureService.books(id).length : -1))
    }
    Timer {
        id: selfTestScripture
        running: typeof SelfTestScripture !== "undefined" && SelfTestScripture
        interval: 22000   // boot, the Bible restore and the sermon indexing
        onTriggered: {
            window.currentView = "show"
            showScreen.showInLibrary("scripture", "")
            selfTestScriptureFirst.restart()
        }
    }
    Timer {
        id: selfTestScriptureFirst
        interval: 8000    // the pane's own reads land while the tab comes up
        onTriggered: {
            window.scriptureProbe("first")
            const p = SelfTest.findItem("selfTestScripturePane")
            if (p)
                p.syncSources()   // what a tab switch does to this pane
            selfTestScriptureSecond.restart()
        }
    }
    Timer {
        id: selfTestScriptureSecond
        interval: 5000
        onTriggered: {
            window.scriptureProbe("re-ask")
            SelfTest.quit()
        }
    }

    // ---- The Table pane's own search box: highlight + hit placement -----------
    // (VGR_PANEHL_TEST=1 only.) User report, looking at the pane's result rows:
    // "the highlighter is not highlighting compelling" and "i expect the highlighted
    // text to show well like in the centre not at the extreme so user can see the
    // hits well". The rows are ONE line wide, so the engine's 220-character snippet
    // (anchored 60 before the match) could put the matched words past the visible
    // edge. This drives the pane's REAL box — the text is SET on the field, whose
    // own debounce runs the search, exactly as typing does — and prints every row
    // as the pane renders it: the centred text with the wrapped words marked
    // «like this», where the first hit sits in the row, and (row 0) the markup the
    // row Text actually carries.
    readonly property string paneQuery: "compelling that"
    // The same query with a misspelled word in it: this box must offer the typo
    // fixer's correction while results are on screen (user call: "the typo fixer
    // should be there" in this box as well). Pass two of the scenario. The typo
    // sits on the FIRST word on purpose — a trailing half-typed word is a prefix
    // by design (see ResolveTermsWithSplits' typingTail), so only an earlier word
    // comes back corrected.
    readonly property string paneTypoQuery: "complling that"
    function paneMark(rendered) {
        return String(rendered ?? "")
            .replace(/<b><font color=Theme.highlightInk>/g, "«")
            .replace(/<\/font><\/b>/g, "»")
            .replace(/&amp;/g, "&")
    }
    // A rendered row with its markup taken back off (the raw text a row shows).
    function panePlain(rendered) {
        return String(rendered ?? "").replace(/<[^>]*>/g, "")
    }
    function paneWords(query) {
        return String(query ?? "").toLowerCase().match(/[a-z0-9]+/g) || []
    }
    // The typed words a row's text visibly carries but does NOT wrap — "that"
    // yellow beside a plain "compelling" is the broken highlighter the user saw.
    function paneUnmarked(marked, visible, words) {
        let missing = ""
        for (const w of words) {
            if (String(visible).toLowerCase().indexOf(w) < 0) continue
            if (marked.toLowerCase().indexOf("«" + w) < 0) missing += w + " "
        }
        return missing
    }
    // One pass, exactly as the pane renders it: every row's centred text with the
    // wrapped words marked «like this», where its first hit sits, what came back
    // unlit — and, from the row Text ITSELF (not a re-computation), the markup the
    // user is actually looking at.
    function panePass(p, query, rowWidth) {
        const rows = p.searchResults
        const words = window.paneWords(query)
        let unmarked = 0
        let firstHitPct = -1
        let worstHitPct = -1
        for (let i = 0; i < rows.length && i < 8; ++i) {
            const shown = p.centeredSnippet(rows[i].snippet, rowWidth)
            const marked = window.paneMark(p.highlight(shown))
            const missing = window.paneUnmarked(marked, shown, words)
            if (missing !== "") ++unmarked
            const at = marked.indexOf("«")
            const pct = at < 0 || marked.length === 0
                        ? -1 : Math.round(100 * at / marked.length)
            if (pct >= 0 && (firstHitPct < 0 || pct < firstHitPct)) firstHitPct = pct
            if (pct > worstHitPct) worstHitPct = pct
            console.log("[SELFTEST-PANEHL] row" + i + "=" + rows[i].reference
                        + " hitAt=" + pct + "% unmarked='" + missing + "' «"
                        + marked.substring(0, 96) + "»")
        }
        const probe = SelfTest.findItem("selfTestPaneHitSnippet")
        let rendered = ""
        let renderedUnmarked = "missing"   // no row Text on screen at all
        if (probe) {
            rendered = window.paneMark(probe.text)
            renderedUnmarked = window.paneUnmarked(rendered, window.panePlain(probe.text), words)
        }
        console.log("[SELFTEST-PANEHL] rendered query='" + query + "' unmarked='"
                    + renderedUnmarked + "' «" + rendered.substring(0, 96) + "»")
        return { rows: rows.length, unmarked: unmarked, firstHitPct: firstHitPct,
                 worstHitPct: worstHitPct, renderedUnmarked: renderedUnmarked }
    }
    // The gold hint line's payload: typed word, what the engine searched for
    // instead, and the half-typed word's readings.
    function paneHint(p) {
        return p.resolvedWords.map((w) => w.typed + "->" + w.resolved
                                   + ((w.candidates && w.candidates.length > 0)
                                      ? "(" + w.candidates.slice(0, 3).join("/") + ")" : ""))
                              .join(" ")
    }
    Timer {
        id: selfTestPaneHighlight
        running: typeof SelfTestPaneHighlight !== "undefined" && SelfTestPaneHighlight
        interval: 45000   // boot + the background sermon indexing
        onTriggered: {
            // The dock has to be SHOWING this pane for the pane's own surfaces — the hint
            // line this scenario asserts on — to be on screen at all: the launch view is
            // whatever the session left behind, so the pane is shown rather than assumed
            // (the posture the presplit and layout-probe scenarios state in their own notes).
            window.currentView = "show"
            showScreen.showInLibrary("table", "")
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p) {
                console.log("[SELFTEST-PANEHL] table pane missing")
                SelfTest.quit()
                return
            }
            const box = SelfTest.findItem("selfTestSearchBox-table")
            if (!box) {
                console.log("[SELFTEST-PANEHL] pane search box missing")
                SelfTest.quit()
                return
            }
            p.searching = true       // the magnifier's click: the box appears
            p.exact = false
            p.scopeLock = false
            box.text = window.paneQuery   // typing — the box's debounce searches
            selfTestPaneHighlightRead.restart()
        }
    }
    Timer {
        id: selfTestPaneHighlightRead
        interval: 12000   // the worker answers with the whole-library rows
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            // The width a row ACTUALLY shows: read it off a rendered row rather
            // than assuming one (the row's own text object is named for this).
            const probe = SelfTest.findItem("selfTestPaneHitSnippet")
            const rowWidth = probe && probe.width > 0 ? probe.width : 700
            console.log("[SELFTEST-PANEHL] pass1 query='" + window.paneQuery + "' rowWidth="
                        + Math.round(rowWidth) + " hint='" + window.paneHint(p) + "'")
            const pass1 = window.panePass(p, window.paneQuery, rowWidth)
            // The pixels too: the pane as it renders pass one (rows + the gold
            // hint line), so the fix can be LOOKED at, not only parsed — plus the
            // first row's own Text, the one artifact a pixel probe can judge
            // without guessing which part of the pane it is looking at
            // (tools/selftest/gold_pixels.mjs reads it).
            SelfTest.grab("selfTestTablePane", "panehl_pass1.png")
            SelfTest.grab("selfTestPaneHitSnippet", "panehl_row0.png")
            selfTestPaneHighlightTypo.pass1 = pass1
            selfTestPaneHighlightTypo.restart()
        }
    }
    // One beat after the grab request: grabToImage renders asynchronously, so the
    // query changes only once the picture of pass one is in flight.
    Timer {
        id: selfTestPaneHighlightTypo
        property var pass1: ({ rows: 0, unmarked: 0, firstHitPct: -1, worstHitPct: -1,
                               renderedUnmarked: "" })
        interval: 1500
        onTriggered: {
            const box = SelfTest.findItem("selfTestSearchBox-table")
            if (!box) {
                console.log("[SELFTEST-PANEHL] pane search box vanished")
                SelfTest.quit()
                return
            }
            // Pass two: the same query with a misspelling, typed into the same
            // box — the fixer's correction has to reach this pane's hint.
            box.text = window.paneTypoQuery
            selfTestPaneHighlightTypoRead.restart()
        }
    }
    Timer {
        id: selfTestPaneHighlightTypoRead
        interval: 14000   // the typo query searches AND resolves afresh
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            const probe = SelfTest.findItem("selfTestPaneHitSnippet")
            const rowWidth = probe && probe.width > 0 ? probe.width : 700
            const hint = window.paneHint(p)
            // The hint LINE on screen, not just its payload: the user's doubt was
            // whether the typo fixer is in THIS box at all.
            // THIS pane's hint, by its own name: the name is per-tab because both tabs' hints
            // are alive at once, and the bare name read the other tab's (empty) line.
            const hintItem = SelfTest.findItem("selfTestPaneSearchHint-table")
            const hintShown = hintItem ? (hintItem.visible && hintItem.text !== "") : false
            console.log("[SELFTEST-PANEHL] pass2 query='" + window.paneTypoQuery
                        + "' hint='" + hint + "' hintShown=" + hintShown
                        + " hintText='" + (hintItem ? hintItem.text : "-") + "'")
            const pass2 = window.panePass(p, window.paneTypoQuery, rowWidth)
            const p1 = selfTestPaneHighlightTypo.pass1
            console.log("[SELFTEST-PANEHL] result rows=" + p1.rows
                        + " unhiglightedRows=" + p1.unmarked
                        + " renderedUnmarked='" + p1.renderedUnmarked + "'"
                        + " firstHitAt=" + (p1.firstHitPct < 0 ? "none" : p1.firstHitPct + "%")
                        + " worstHitAt=" + (p1.worstHitPct < 0 ? "none" : p1.worstHitPct + "%")
                        + " typoRows=" + pass2.rows
                        + " typoRenderedUnmarked='" + pass2.renderedUnmarked + "'"
                        + " typoHint='" + hint + "'"
                        + " typoHintShown=" + hintShown)
            // Pass two's pixels (the misspelled query's rows + the correction).
            SelfTest.grab("selfTestTablePane", "panehl_pass2.png")
            selfTestPaneHighlightDone.restart()
        }
    }
    // quit() DESTROYS the window while grabToImage's async render is still in
    // flight (the pattern run_exact_test.sh learned), so quit one beat later.
    Timer {
        id: selfTestPaneHighlightDone
        interval: 2000
        onTriggered: SelfTest.quit()
    }

    // ---- The tab search's SEED: does the open sermon fill the box, and do the
    // digits typed after it move the pane? (VGR_SEED_TEST=1 only.) ---------------
    // The operator's flow, driven end to end: The Table opens a sermon, the dock's
    // search box holds that sermon's citation line ("55-0117 - How The Angel… "),
    // they focus it and type 56 — the pane must be on ¶56. Every step is printed
    // with the box's own state (text / hint / seed) so a step that does not happen
    // is visible as its own line, not inferred from the final position.
    function seedState(tag, paneName) {
        const box = SelfTest.findItem("selfTestSearchBox")
        const field = SelfTest.findItem("selfTestSearchInput")
        const p = SelfTest.findItem(paneName !== undefined ? paneName : "selfTestTablePane")
        console.log("[SELFTEST-SEED] " + tag
                    + " boxText='" + (box ? box.text : "-missing-") + "'"
                    + " hint='" + (box ? box.emptyHint : "-") + "'"
                    + " seed='" + (box ? box.seedText : "-") + "'"
                    + " follows=" + (box ? box.seedFollows : "-")
                    + " focused=" + (field ? field.activeFocus : "-")
                    + " paneBook='" + (p && p.book ? p.book.id : "") + "'"
                    + " chapter=" + (p ? p.chapterNumber : -1)
                    + " rows=" + (p ? p.mainRows.length : -1)
                    + " selected=[" + (p ? p.selected.join(",") : "") + "]"
                    + " ref='" + (p ? p.referenceText : "") + "'")
    }
    Timer {
        id: selfTestSeed
        running: typeof SelfTestSeed !== "undefined" && SelfTestSeed
        interval: 25000   // boot (the pane opens a sermon well before the indexing ends)
        onTriggered: {
            window.currentView = "show"
            showScreen.showInLibrary("table", "")
            selfTestSeedOpen.restart()
        }
    }
    Timer {
        id: selfTestSeedOpen
        interval: 6000
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p || !p.books || p.books.length === 0) {
                console.log("[SELFTEST-SEED] pane/books missing")
                SelfTest.quit()
                return
            }
            // A NAMED sermon (the user's own 55-0117 when the library has it), so the
            // seed line a failure prints is one a person can compare to their screen.
            let target = null
            for (const b of p.books) {
                if (String(b.id || "").indexOf("0117") >= 0) { target = b; break }
            }
            if (!target)
                target = p.books[0]
            const chapter = target.chapters && target.chapters.length > 0 ? target.chapters[0] : 0
            console.log("[SELFTEST-SEED] opening book='" + target.id + "' chapter=" + chapter)
            p.goTo({ bookId: target.id, chapter: chapter, verseStart: 0, verseEnd: 0 })
            selfTestSeedWait.restart()
        }
    }
    Timer {
        id: selfTestSeedWait
        interval: 5000    // the chapter read comes back off the worker
        onTriggered: {
            window.seedState("seeded")
            SelfTest.grab("selfTestLibraryTabBar", "seed_before.png")
            selfTestSeedFocus.restart()
        }
    }
    Timer {
        id: selfTestSeedFocus
        interval: 1200
        onTriggered: {
            // Focus FILLS the box (TabSearchBox.applySeed) — the real focus path,
            // through the item, not by setting `text` from outside.
            const ok = SelfTest.focusItem("selfTestSearchInput")
            window.seedState("focused=" + ok)
            selfTestSeedType.restart()
        }
    }
    Timer {
        id: selfTestSeedType
        interval: 1500
        onTriggered: {
            SelfTest.type("56")
            window.seedState("typed")
            selfTestSeedRead.restart()
        }
    }
    Timer {
        id: selfTestSeedRead
        interval: 5000    // the pane's resolve + the reading beat
        onTriggered: {
            const box = SelfTest.findItem("selfTestSearchBox")
            const p = SelfTest.findItem("selfTestTablePane")
            const picked = p ? p.selected.map((n) => Number(n)) : []
            const atNumber = picked.length > 0 ? picked[0] : -1
            window.seedState("result")
            console.log("[SELFTEST-SEED] result boxText='" + (box ? box.text : "-")
                        + " atParagraph=" + atNumber
                        + " verdict=" + (atNumber === 56 ? "moved" : "stayed"))
            SelfTest.grab("selfTestTablePane", "seed_after.png")
            SelfTest.grab("selfTestLibraryTabBar", "seed_box.png")
            selfTestSeedClick.restart()
        }
    }
    // THE TWO CLICKS, one row shape apart (user call: "double click is what should
    // push it to monitor preview, single click is preview pane"). A continuation row
    // is the case that broke it — its single click used to stage — so the row picked
    // here is the first divided piece on screen. What a click and a double click DO is
    // called directly (selectSubpart / stageSubpart), because synthesizing a real
    // double click on an unnamed row delegate is a coordinate guess; the wiring between
    // them and the pointer is DragSource's activated/opened, the same pair every other
    // list in the app uses.
    function seedStagedTitle() {
        const staged = LiveOutputService.stagedSlide
        return staged && staged.title !== undefined ? String(staged.title) : ""
    }
    Timer {
        id: selfTestSeedClick
        interval: 1200
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p) {
                SelfTest.quit()
                return
            }
            let row = null
            for (const r of p.mainRows)
                if (r.continuation === true) { row = r; break }
            if (!row)
                for (const r of p.mainRows)
                    if (Number(r.number) > 0) { row = { number: r.number, partIndex: 0 }; break }
            if (!row) {
                console.log("[SELFTEST-SEED] click no row to click")
                SelfTest.quit()
                return
            }
            window.seedClickRow = { number: Number(row.number), part: Number(row.partIndex ?? 0) }
            window.seedClickBefore = window.seedStagedTitle()
            p.selectSubpart(window.seedClickRow.number, window.seedClickRow.part)
            selfTestSeedClickSingle.restart()
        }
    }
    property var seedClickRow: null
    property string seedClickBefore: ""
    property string seedClickAfterSingle: ""
    Timer {
        id: selfTestSeedClickSingle
        interval: 1500
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            window.seedClickAfterSingle = window.seedStagedTitle()
            console.log("[SELFTEST-SEED] click single row=" + window.seedClickRow.number
                        + "-" + window.seedClickRow.part
                        + " before='" + window.seedClickBefore + "'"
                        + " after='" + window.seedClickAfterSingle + "'"
                        + " staged=" + (window.seedClickAfterSingle !== window.seedClickBefore))
            p.stageSubpart(window.seedClickRow.number, window.seedClickRow.part)
            selfTestSeedClickDouble.restart()
        }
    }
    Timer {
        id: selfTestSeedClickDouble
        interval: 1500
        onTriggered: {
            const double = window.seedStagedTitle()
            console.log("[SELFTEST-SEED] click double='" + double + "'"
                        + " verdict=" + ((window.seedClickAfterSingle === window.seedClickBefore
                                           && double !== "" && double !== window.seedClickBefore)
                                          ? "ok" : "wrong"))
            selfTestSeedScripture.restart()
        }
    }
    // THE SAME FEATURE ON SCRIPTURE (user call: "scripture too needs that feature"):
    // its box holds "Genesis 1: " and the digits typed after it are the VERSE.
    Timer {
        id: selfTestSeedScripture
        interval: 1200
        onTriggered: {
            showScreen.showInLibrary("scripture", "")
            selfTestSeedScriptureOpen.restart()
        }
    }
    Timer {
        id: selfTestSeedScriptureOpen
        interval: 4000
        onTriggered: {
            const p = SelfTest.findItem("selfTestScripturePane")
            if (!p || !p.books || p.books.length === 0) {
                console.log("[SELFTEST-SEED] scripture pane/books missing")
                SelfTest.quit()
                return
            }
            let target = null
            for (const b of p.books) {
                if (String(b.name || "").toLowerCase() === "genesis") { target = b; break }
            }
            if (!target)
                target = p.books[0]
            console.log("[SELFTEST-SEED] scripture book='" + target.name + "'")
            p.goTo({ bookId: target.id, chapter: 1, verseStart: 0, verseEnd: 0, matchText: "" })
            selfTestSeedScriptureWait.restart()
        }
    }
    Timer {
        id: selfTestSeedScriptureWait
        interval: 4000
        onTriggered: {
            window.seedState("scripture seeded", "selfTestScripturePane")
            const ok = SelfTest.focusItem("selfTestSearchInput")
            window.seedState("scripture focused=" + ok, "selfTestScripturePane")
            selfTestSeedScriptureType.restart()
        }
    }
    // What the pane's own resolve says for what the box holds — the half a position
    // check cannot see ("the line is in the box but the pane stayed on verse 1").
    function seedResolveProbe(paneName) {
        const p = SelfTest.findItem(paneName)
        if (!p)
            return "pane missing"
        const box = SelfTest.findItem("selfTestSearchBox")
        const text = box ? box.text : ""
        const resolved = p.adapter ? p.adapter.resolve(text, p.sourceId) : ({ error: "no adapter" })
        return "filter='" + p.filter + "' pending='" + p.pendingJump + "'"
               + " resolve=" + JSON.stringify(resolved)
    }
    Timer {
        id: selfTestSeedScriptureType
        interval: 1200
        onTriggered: {
            // The user's own case: verse 8 in the open chapter.
            SelfTest.type("8")
            window.seedState("scripture typed", "selfTestScripturePane")
            console.log("[SELFTEST-SEED] scripture typed " + window.seedResolveProbe("selfTestScripturePane"))
            selfTestSeedScriptureRead.restart()
        }
    }
    Timer {
        id: selfTestSeedScriptureRead
        interval: 5000    // the reference read is cached off-thread: it lands a beat later
        onTriggered: {
            const box = SelfTest.findItem("selfTestSearchBox")
            const p = SelfTest.findItem("selfTestScripturePane")
            const picked = p ? p.selected.map((n) => Number(n)) : []
            window.seedState("scripture result", "selfTestScripturePane")
            console.log("[SELFTEST-SEED] scripture result boxText='" + (box ? box.text : "-")
                        + " atVerse=" + (picked.length > 0 ? picked[0] : -1)
                        + " ref='" + (p ? p.referenceText : "") + "'")
            console.log("[SELFTEST-SEED] scripture result " + window.seedResolveProbe("selfTestScripturePane"))
            SelfTest.grab("selfTestLibraryTabBar", "seed_scripture_box.png")
            selfTestSeedWalk.restart()
        }
    }
    // THE SEARCH LIST FROM THE KEYBOARD (user call: "if press the down or up button it
    // should search result list and show their paragraph preview the the <> should
    // move to their paragraph preview", + "the return search should be 56-0101 - Why
    // are people ... so it consistent with others"). Back on The Table: "56-" typed,
    // one Down (the pane's walk), then Return (the result as the box's line).
    Timer {
        id: selfTestSeedWalk
        interval: 1200
        onTriggered: {
            showScreen.showInLibrary("table", "")
            selfTestSeedWalkSetup.restart()
        }
    }
    Timer {
        id: selfTestSeedWalkSetup
        interval: 2500
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            const box = SelfTest.findItem("selfTestSearchBox")
            if (!p || !box) {
                SelfTest.quit()
                return
            }
            // The way a keystroke reaches the pane's own list: the box asks the
            // provider, which fills citationMatches (The Table renders those IN the
            // pane). Then the box's own Up/Down signal is emitted — the same signal
            // the keys carry.
            box.resetText("56-")
            p.paneSuggestions("56-")
            box.stepMatch(1)
            selfTestSeedWalkRead.restart()
        }
    }
    Timer {
        id: selfTestSeedWalkRead
        interval: 1200
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            const row = p && p.citationMatches.length > 0 ? p.citationMatches[Math.max(0, p.matchCursor)] : null
            const peek = p && p.peekVerses.length > 0 ? String(p.peekVerses[0].text).substring(0, 48) : ""
            console.log("[SELFTEST-SEED] walk rows=" + (p ? p.citationMatches.length : -1)
                        + " cursor=" + (p ? p.matchCursor : -1)
                        + " row='" + (row ? (row.cite || row.label) : "") + "'"
                        + " peekIndex=" + (p ? p.peekIndex : -1)
                        + " peek='" + peek + "'")
            const box = SelfTest.findItem("selfTestSearchBox")
            // The selector across the columns (the ‹ › in search mode): the box hands
            // Left/Right to the pane while the result list is up.
            console.log("[SELFTEST-SEED] column listMode=" + box.listMode
                        + " before=" + (p ? p.column : -1))
            box.stepColumn(-1)
            console.log("[SELFTEST-SEED] column after=" + (p ? p.column : -1))
            // NARROW, DON'T OPEN: on the sermons column Up/Down must move the selector
            // and narrow the results, leaving the preview (the pick) alone.
            const selBefore = JSON.stringify(p.selected)
            const chBefore = p.chapterNumber
            box.stepMatch(-1)
            console.log("[SELFTEST-SEED] narrow column=" + p.column
                        + " chapterCursor=" + p.chapterCursor
                        + " matches=" + p.citationMatches.length + "/" + p.allMatches.length
                        + " chapters=" + (p.book ? p.book.chapters.length : -1)
                        + " selectionFrozen=" + (JSON.stringify(p.selected) === selBefore
                                                   && p.chapterNumber === chBefore))
            box.accepted(box.text)      // the Return the field delivers
            selfTestSeedWalkReturn.restart()
        }
    }
    Timer {
        id: selfTestSeedWalkReturn
        interval: 1500
        onTriggered: {
            const box = SelfTest.findItem("selfTestSearchBox")
            const p = SelfTest.findItem("selfTestTablePane")
            console.log("[SELFTEST-SEED] return boxText='" + (box ? box.text : "-") + "'"
                        + " matches=" + (p ? p.citationMatches.length : -1)
                        + " ref='" + (p ? p.referenceText : "") + "'")
            // ESC puts the search back where it started (the place captured on the first
            // keystroke), so this run ends by leaving search mode.
            box.dismissed()
            selfTestSeedEsc.restart()
        }
    }
    Timer {
        id: selfTestSeedEsc
        interval: 1500
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            console.log("[SELFTEST-SEED] esc matches=" + (p ? p.citationMatches.length : -1)
                        + " searchMode=" + (p ? p.searchMode : "-")
                        + " ref='" + (p ? p.referenceText : "") + "'")
            selfTestSeedDone.restart()
        }
    }
    Timer {
        id: selfTestSeedDone
        interval: 2000
        onTriggered: SelfTest.quit()
    }

    // ---- Do the pane's result lists build a VIEWPORT, not the whole model? ----
    // (VGR_PANEROWS_TEST=1 only.) The user's ask, right after the paragraph list
    // was virtualized: "Make the reference pane's search-results list virtualized
    // as well, since a two-character query can still build about a thousand rows
    // at once." A row COUNT cannot answer that — a long list is fine, building it
    // in one go is not — so this scenario measures the two things only a windowed
    // list does, for each of the three lists the pane owns (the tab search's
    // citation matches, the pill search's hits, and the hover peek's sermon
    // paragraphs):
    //
    //   * with the rows on screen, `built` stays a viewport's worth (built << rows), and
    //   * scrolling to the END materializes the LAST row — a windowed list must, or
    //     the tail of a 641-match query would be unreachable.
    //
    // It drives the REAL path: the two-character query is TYPED into the tab bar's
    // box (the provider runs per keystroke, as it does under a user's fingers), the
    // hover is the pane's own peekSuggestion(), and the pill's field is set the way
    // the pane-highlight scenario sets it. Library-independent: each list's two
    // numbers are read off its own counters and compared to each other, so a small
    // library still decides it — a list too short for the question to mean anything
    // says so instead of passing.
    readonly property string paneRowsQuery: "th"   // the user's two characters
    property var paneRowsFacts: ({})

    // A list's counters, read together (a missing counter is -1, so a pane that
    // lost them can never be read as "built nothing").
    function paneRowsRead(which) {
        const p = SelfTest.findItem("selfTestTablePane")
        if (!p)
            return { rows: -1, built: -1 }
        const n = (name) => (p[name] === undefined ? -1 : p[name])
        if (which === "match")
            return { rows: p.citationMatches.length, built: n("matchRowBuilds") }
        if (which === "hit")
            return { rows: p.searchResults.length, built: n("hitRowBuilds") }
        return { rows: p.peekVerses.length, built: n("peekRowBuilds") }
    }
    // Scroll to the END of the list the mode shows — the same object the user's
    // wheel moves, so the measurement is about the list, not a lookalike.
    function paneRowsScroll(which) {
        const facts = window.paneRowsFacts[which]
        const list = SelfTest.findItem(which === "match" ? "selfTestPaneMatchList"
                                     : which === "hit" ? "selfTestPaneHitList"
                                                        : "selfTestPanePeekList")
        if (list && facts && facts.rows > 0)
            list.positionViewAtIndex(facts.rows - 1, ListView.End)
    }
    // The LAST row's delegate, by the name its delegate carries — itemVisible and
    // not "exists", because a RECYCLED row of the same name is one the user cannot
    // see (the pre-split scenario learned this the hard way).
    function paneRowsLastRow(which, rows) {
        const name = which === "match" ? "selfTestMatchRow_" + (rows - 1)
                   : which === "hit" ? "selfTestHitRow_" + (rows - 1)
                                      : "selfTestPeekRow_" + (rows - 1)
        return SelfTest.itemVisible(name)
    }
    // One line per list per phase: `kept` = as the list stands when the query's
    // rows are on screen; `scrolled` = after the scroll to its last row. The runner
    // reads these, so the field names are the contract.
    function paneRowsLog(which, phase) {
        const now = window.paneRowsRead(which)
        const facts = window.paneRowsFacts
        if (facts[which] === undefined)
            facts[which] = { rows: now.rows, built: now.built, scrolledBuilt: -1, lastRow: false }
        if (phase === "scrolled") {
            facts[which].scrolledBuilt = now.built
            facts[which].lastRow = window.paneRowsLastRow(which, facts[which].rows)
        }
        const f = facts[which]
        console.log("[SELFTEST-PANEROWS] " + which + " " + phase + " rows=" + f.rows
                    + " built=" + f.built + " scrolledBuilt=" + f.scrolledBuilt
                    + " lastRow=" + f.lastRow)
    }
    // The pane's search box field, as the pane-highlight scenario drives it (the
    // field's own debounce runs the search).
    function paneRowsPillBox() {
        return SelfTest.findItem("selfTestSearchBox-table")
    }
    // The match whose sermon is LONGEST (the peek's own worst case — a 500-paragraph
    // sermon is what the hover actually has to open). Falls back to the first row.
    function paneRowsLongestMatch(p) {
        let best = 0
        let bestRows = -1
        const count = Math.min(12, p.citationMatches.length)
        for (let i = 0; i < count; ++i) {
            const row = p.citationMatches[i]
            const ref = row ? row.ref : null
            if (!ref || !(ref.chapter > 0))
                continue
            const rows = p.adapter.chapter(p.sourceId, ref.bookId, ref.chapter)
            const n = rows ? rows.length : 0
            if (n > bestRows) {
                bestRows = n
                best = i
            }
        }
        console.log("[SELFTEST-PANEROWS] peek target=" + best + " of " + p.citationMatches.length
                    + " matches (that sermon's paragraphs=" + bestRows + ")")
        return best
    }
    Timer {
        id: selfTestPaneRows
        running: typeof SelfTestPaneRows !== "undefined" && SelfTestPaneRows
        interval: 40000   // boot + the background sermon indexing (the pane-highlight wait)
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            if (!p) {
                console.log("[SELFTEST-PANEROWS] table pane missing")
                SelfTest.quit()
                return
            }
            SelfTest.activate()
            window.currentView = "show"             // the library dock has to be the view on screen
            showScreen.showInLibrary("table", "")   // the tab, nothing typed yet
            p.searching = false                     // no pill search: the MATCHES list is the one on screen
            // The user's own two characters, typed into the TAB BAR's box: the
            // provider runs on every keystroke, exactly as it does for a hand.
            SelfTest.focusItem("selfTestSearchInput")
            SelfTest.type(window.paneRowsQuery)
            selfTestPaneRowsMatch.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsMatch
        interval: 5000   // the provider answers per keystroke; this is the pane laying out
        onTriggered: {
            window.paneRowsLog("match", "kept")
            window.paneRowsScroll("match")
            selfTestPaneRowsMatchEnd.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsMatchEnd
        interval: 2000   // the scroll has to land, and the rows it brought in build
        onTriggered: {
            window.paneRowsLog("match", "scrolled")
            const p = SelfTest.findItem("selfTestTablePane")
            // The HOVER the matches list exists for (no floating preview: the
            // sermon opens in the pane's own right half).
            p.peekSuggestion(window.paneRowsLongestMatch(p))
            selfTestPaneRowsPeek.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsPeek
        interval: 3000   // the sermon's paragraphs land and lay out
        onTriggered: {
            window.paneRowsLog("peek", "kept")
            window.paneRowsScroll("peek")
            selfTestPaneRowsPeekEnd.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsPeekEnd
        interval: 2000
        onTriggered: {
            window.paneRowsLog("peek", "scrolled")
            // The pill's own search, the OTHER result list in this pane (the same
            // two characters, so the two lists are answering one query).
            const p = SelfTest.findItem("selfTestTablePane")
            p.searching = true
            p.exact = false
            const box = window.paneRowsPillBox()
            if (!box) {
                console.log("[SELFTEST-PANEROWS] pane search box missing")
                window.paneRowsReport()
                return
            }
            box.text = window.paneRowsQuery
            selfTestPaneRowsHits.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsHits
        interval: 18000   // the whole-library scan behind the pill's search (the pane-highlight wait)
        onTriggered: {
            window.paneRowsLog("hit", "kept")
            window.paneRowsScroll("hit")
            selfTestPaneRowsHitsEnd.restart()
        }
    }
    Timer {
        id: selfTestPaneRowsHitsEnd
        interval: 2000
        onTriggered: window.paneRowsReport()
    }
    // The scenario's own closing line (the runner's marker) + the pixels of the
    // state it measured.
    function paneRowsReport() {
        const f = window.paneRowsFacts
        window.paneRowsLog("hit", "scrolled")
        SelfTest.grab("selfTestTablePane", "panerows_pane.png")
        const line = (which) => (f[which] ? (f[which].rows + "/" + f[which].built + "/"
                                             + f[which].scrolledBuilt + "/" + f[which].lastRow)
                                          : "-/-")
        console.log("[SELFTEST-PANEROWS] result query='" + window.paneRowsQuery
                    + "' matches=" + line("match") + " peek=" + line("peek")
                    + " hits=" + line("hit"))
        selfTestPaneRowsDone.restart()
    }
    // quit() one beat after the grab request (see the pane-highlight scenario).
    Timer {
        id: selfTestPaneRowsDone
        interval: 2000
        onTriggered: SelfTest.quit()
    }

    // ---- Is the cursor still SHOWN once the app is scaled? --------------------
    // (VGR_CURSORSCALE_TEST=1 only.) User report: "for the app rescaling the
    // cursor doesn't show as well". Every layer here is drawn at its 1440x900
    // design size and scaled by window.uiScale, and this codebase has already
    // recorded that per-MouseArea cursorShape stops applying under a scaled
    // ancestor (see AppCursorCatcher.qml) — which would make the hand cursor on
    // every plain-cursorShape control (AppButton, footers, scrollbars) vanish on
    // any display that is not exactly the design size. The app's own stack can
    // be right while the OS cursor stays an arrow, so this scenario PARKS the
    // pointer on a plain-cursorShape button and says where it parked; the shell
    // runner reads the LIVE OS cursor with Win32 (cursor_shape.ps1) at uiScale 1
    // and again scaled — that pair is the whole question.
    //
    // Three parks: a plain-cursorShape button (AppButton's pointing hand) at
    // uiScale 1 and again with the window 20% over the design size — the pair
    // that answers whether the pointing hand survives a scaled ancestor — and
    // then the WINDOW's own edge, where resizing the app starts and
    // WindowResizeHandles must show a size arrow (PositionHoverArea's cursor,
    // the mechanism every other cursor in the app uses).
    property string cursorScalePark: ""
    property string cursorFrameEdge: ""
    property string cursorFrameCorner: ""
    Timer {
        id: selfTestCursorScale
        running: typeof SelfTestCursorScale !== "undefined" && SelfTestCursorScale
        interval: 14000   // the app's own QML load
        onTriggered: {
            window.openSettings("styles")
            // Phase A at the design size: min(1440/1440, 900/900) = 1, so nothing
            // on screen is scaled.
            window.width = 1440
            window.height = 900
            selfTestCursorScalePark.restart()
        }
    }
    Timer {
        id: selfTestCursorScalePark
        interval: 2200    // the settings card lays out at the new scale
        onTriggered: {
            const button = SelfTest.findItem("selfTestAddStyle")
            const c = button ? button.mapToItem(null, button.width / 2, button.height / 2)
                             : { x: -1, y: -1 }
            if (!button || c.x < 0) {
                console.log("[SELFTEST-CURSORSCALE] no button to park on (found=" + (button ? "yes" : "no") + ")")
                window.cursorScalePark = "missing"
                selfTestCursorScaleNext.restart()
                return
            }
            // Park and read in ONE call: the shape the OS shows while the
            // pointer sits on this button is the whole question, and reading
            // it from here is what keeps the answer free of the human at the
            // keyboard (a separate process reading the live cursor races them).
            const shape = SelfTest.cursorAt(c.x, c.y)
            window.cursorScalePark = "A uiScale=" + window.uiScale.toFixed(3)
                        + " cursor=" + shape
            console.log("[SELFTEST-CURSORSCALE] phase=A uiScale=" + window.uiScale.toFixed(3)
                        + " parked at " + Math.round(c.x) + "," + Math.round(c.y)
                        + " button=" + Math.round(button.width) + "x" + Math.round(button.height)
                        + " osCursor=" + shape)
            selfTestCursorScaleNext.restart()
        }
    }
    Timer {
        id: selfTestCursorScaleNext
        interval: 6000    // long enough for the shell runner to read the OS cursor
        onTriggered: {
            if (window.cursorScalePark.indexOf("A ") === 0) {
                // Phase B: a window 20% bigger than the design in both axes, so
                // uiScale is 1.2 and several layers are scaled.
                window.width = 1728
                window.height = 1080
                console.log("[SELFTEST-CURSORSCALE] phase=B uiScale=" + window.uiScale.toFixed(3))
                selfTestCursorScaleB.restart()
                return
            }
            if (window.cursorScalePark.indexOf("B ") === 0) {
                // Phase C: the app's own frame — the gesture in "the app
                // rescaling", i.e. dragging the window's border to resize it.
                selfTestCursorScaleC.restart()
                return
            }
            console.log("[SELFTEST-CURSORSCALE] result parkA='" + window.cursorScalePark
                        + "' frameEdge='" + window.cursorFrameEdge
                        + "' frameCorner='" + window.cursorFrameCorner + "' done")
            SelfTest.quit()
        }
    }
    Timer {
        id: selfTestCursorScaleB
        interval: 2200
        onTriggered: {
            const button = SelfTest.findItem("selfTestAddStyle")
            const c = button ? button.mapToItem(null, button.width / 2, button.height / 2)
                             : { x: -1, y: -1 }
            if (!button || c.x < 0) {
                console.log("[SELFTEST-CURSORSCALE] phase=B button missing")
                window.cursorScalePark = "B missing"
                selfTestCursorScaleNext.restart()
                return
            }
            const shape = SelfTest.cursorAt(c.x, c.y)
            window.cursorScalePark = "B uiScale=" + window.uiScale.toFixed(3)
                        + " cursor=" + shape
            console.log("[SELFTEST-CURSORSCALE] phase=B uiScale=" + window.uiScale.toFixed(3)
                        + " parked at " + Math.round(c.x) + "," + Math.round(c.y)
                        + " button=" + Math.round(button.width) + "x" + Math.round(button.height)
                        + " osCursor=" + shape)
            selfTestCursorScaleNext.restart()
        }
    }
    // Phase C: the window's OWN frame. WindowResizeHandles puts an 8px grip on
    // each edge and corner (unscaled on purpose — it IS the frame), and the size
    // arrow there comes from PositionHoverArea, the pointer-position mechanism
    // every other cursor in the app uses. "the cursor doesn't show when
    // rescaling the app" is exactly this strip: if the arrow is missing here,
    // the operator gets an arrow at the border of a frameless window and has no
    // cue that the edge can be dragged. Both reads are in one tick, like the
    // button parks. `windowed=` is printed next to them because the grips are
    // inert on a maximized window, which would make a missing arrow correct.
    Timer {
        id: selfTestCursorScaleC
        interval: 2200
        onTriggered: {
            // The Settings card was opened for the button parks and its scrim
            // covers the whole window — including the frame grips, which are
            // SUPPOSED to lose the pointer to an open modal. Close it first, or
            // this phase measures the scrim instead of the frame.
            settingsScrim.visible = false
            const atEdge = { x: 3, y: Math.round(window.height / 2) }
            const atCorner = { x: 3, y: 3 }
            window.cursorFrameEdge = SelfTest.cursorAt(atEdge.x, atEdge.y)
            // ...and again one pixel further along the same grip. The pair tells
            // "the shape never applies" apart from "the shape applies one mouse
            // event late" — a QML cursor claim made WHILE the move is being
            // processed can only reach the OS on the NEXT move, which is exactly
            // how a size arrow ends up flashing instead of showing.
            const edgeAgain = SelfTest.cursorAt(atEdge.x, atEdge.y + 1)
            console.log("[SELFTEST-CURSORSCALE] frame edge first=" + window.cursorFrameEdge
                        + " next=" + edgeAgain)
            const grip = SelfTest.findItem("selfTestFrameGrip")
            console.log("[SELFTEST-CURSORSCALE] frame grip found=" + (grip ? "yes" : "no")
                        + " hovered=" + (grip ? grip.hovered : "?")
                        + " want=" + (grip ? grip.cursorShape : "?")
                        + " showCursor=" + (grip ? grip.showCursor : "?")
                        + " geoHovered=" + (grip ? AppCursor.hovered(grip) : "?")
                        + " blocked=" + (grip ? AppCursor.blocked(grip) : "?")
                        + " stackDepth=" + AppCursor._stack.length
                        + " active=" + AppCursor.active + " shape=" + AppCursor.shape)
            window.cursorFrameCorner = SelfTest.cursorAt(atCorner.x, atCorner.y)
            // Marks the phase as done — the step timer branches on this string,
            // so leaving it at "B ..." re-ran this phase forever.
            window.cursorScalePark = "C uiScale=" + window.uiScale.toFixed(3)
                        + " edge=" + window.cursorFrameEdge
                        + " corner=" + window.cursorFrameCorner
            console.log("[SELFTEST-CURSORSCALE] phase=C windowed="
                        + String(window.visibility === Window.Windowed)
                        + " size=" + window.width + "x" + window.height
                        + " edge@" + atEdge.x + "," + atEdge.y + "=" + window.cursorFrameEdge
                        + " corner@" + atCorner.x + "," + atCorner.y + "=" + window.cursorFrameCorner)
            selfTestCursorScaleNext.restart()
        }
    }

    // ---- The edit canvas: the adjustment points and their cursors ------------
    // (VGR_CANVAS_TEST=1 only.) User report, looking at a text item framed to the
    // canvas edge: "the adjustment point of edit are not in the edit canvas", "the
    // cursor doesn't change when moving or resize but it does when in text mode ...
    // i meant arrow point beams that show when resize the item", and "for the app
    // rescaling the cursor doesn't show as well".
    //
    // One text item is framed to the canvas edge (the shape in the report), then
    // the REAL cursor is walked onto every adjustment point: each dot's rectangle
    // must lie inside the canvas (mCanvas clips — a dot past its edge is not on
    // screen and cannot be grabbed) and the cursor must take that handle's own
    // size arrow. The body is probed too (the I-beam a text item shows, then the
    // grab a press must take), and the whole sweep is repeated after the WINDOW
    // is resized, since every layer re-scales with it.
    property var canvasProbe: []
    property int canvasIndex: 0
    property bool canvasReading: false
    property var canvasStats: null
    property string canvasItemName: ""
    property string canvasInterior: "untested"
    property string canvasRescale: "untested"

    function canvasSceneBounds(item) {
        const p0 = item.mapToItem(null, 0, 0)
        const p1 = item.mapToItem(null, item.width, item.height)
        return { x: Math.min(p0.x, p1.x), y: Math.min(p0.y, p1.y),
                 right: Math.max(p0.x, p1.x), bottom: Math.max(p0.y, p1.y) }
    }
    function canvasInside(inner, outer) {
        return inner.x >= outer.x - 0.5 && inner.y >= outer.y - 0.5
            && inner.right <= outer.right + 0.5 && inner.bottom <= outer.bottom + 0.5
    }
    // The shape each dot must claim, by the same rule DraggableCanvasText's
    // ResizeHandle.targetShape() uses (corner 0/3 = top-left/bottom-right).
    function canvasCornerShape(index) {
        const left = index !== 1 && index !== 3
        const top = index < 2
        return left === top ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
    }
    function canvasEdgeShape(index) {
        return (index === 0 || index === 2) ? Qt.SizeVerCursor : Qt.SizeHorCursor
    }
    function canvasPushProbe(label, item, want) {
        const entry = { label: label, want: want, got: -1, x: -1, y: -1 }
        if (item) {
            const c = item.mapToItem(null, item.width / 2, item.height / 2)
            entry.x = c.x
            entry.y = c.y
            entry.rect = window.canvasSceneBounds(item)
        }
        window.canvasProbe.push(entry)
        return entry
    }
    // Every adjustment point, then the body, then the body under the press.
    // `suffix` = "_" + the item delegate's objectName (see handleSuffix).
    function canvasBuildProbes(suffix) {
        window.canvasProbe = []
        window.canvasStats = { dots: 0, inside: 0, ok: 0, total: 0, body: "untested", press: "untested" }
        const canvas = SelfTest.findItem("selfTestCanvas")
        const item = SelfTest.findItem(window.canvasItemName)
        const cb = canvas ? window.canvasSceneBounds(canvas) : null
        console.log("[SELFTEST-CANVAS] canvas=" + (cb
                        ? Math.round(cb.x) + "," + Math.round(cb.y) + " "
                          + Math.round(cb.right - cb.x) + "x" + Math.round(cb.bottom - cb.y)
                        : "MISSING")
                    + " item=" + window.canvasItemName + (item ? " found" : " MISSING"))
        for (let i = 0; i < 4; ++i) {
            const dot = SelfTest.findItem("canvasDotCorner" + i + suffix)
            const probe = window.canvasPushProbe("corner" + i, dot, window.canvasCornerShape(i))
            window.canvasCountDot(probe, dot, cb)
        }
        for (let i = 0; i < 4; ++i) {
            const dot = SelfTest.findItem("canvasDotEdge" + i + suffix)
            const probe = window.canvasPushProbe("edge" + i, dot, window.canvasEdgeShape(i))
            window.canvasCountDot(probe, dot, cb)
        }
        window.canvasPushProbe("body", item, Qt.IBeamCursor)
        window.canvasPushProbe("press", item, Qt.ClosedHandCursor).press = true
        window.canvasPushProbe("released", item, Qt.IBeamCursor).release = true
    }
    function canvasCountDot(probe, dot, canvasBounds) {
        if (!dot) {
            console.log("[SELFTEST-CANVAS] " + probe.label + " MISSING (no item named to match)")
            return
        }
        ++window.canvasStats.dots
        const inside = canvasBounds && window.canvasInside(probe.rect, canvasBounds)
        if (inside)
            ++window.canvasStats.inside
        console.log("[SELFTEST-CANVAS] " + probe.label + " at "
                    + Math.round(probe.rect.x) + "," + Math.round(probe.rect.y)
                    + " inside=" + inside)
    }
    function canvasFinishPass(phase) {
        const s = window.canvasStats
        const verdict = "dots=" + s.inside + "/" + s.dots + " shapes=" + s.ok + "/" + s.total
                      + " body=" + s.body + " press=" + s.press
        console.log("[SELFTEST-CANVAS] pass=" + phase + " " + verdict)
        if (phase === "interior") {
            window.canvasInterior = verdict
            // The app-scale pass: the window resize re-scales every layer, and
            // the adjustment points have to survive it (dots on the canvas, the
            // same arrows under the pointer).
            const before = window.width
            window.width = window.width > 1100 ? window.width - 160 : window.width + 160
            console.log("[SELFTEST-CANVAS] rescaling window " + Math.round(before) + " -> "
                        + Math.round(window.width) + " (uiScale " + window.uiScale.toFixed(3) + ")")
            selfTestCanvasRescale.restart()
            return
        }
        window.canvasRescale = verdict
        console.log("[SELFTEST-CANVAS] result interior: " + window.canvasInterior
                    + " | rescale: " + window.canvasRescale)
        SelfTest.grab("selfTestCanvas", "canvas_handles.png")
        selfTestCanvasDone.restart()
    }
    Timer {
        id: selfTestCanvasSetup
        running: typeof SelfTestCanvasCursor !== "undefined" && SelfTestCanvasCursor
        interval: 24000   // boot: QML load (~15s) + the show/slide models
        onTriggered: {
            // Take the foreground for the sweep: every probe below reads hover
            // state, and hover only reaches this window while it is the one
            // under the pointer. Measured with the terminal in front: EVERY
            // handle read -1 (no claim at all, not a wrong shape), which reads
            // like a total regression instead of a harness that lost the
            // foreground.
            SelfTest.activate()
            window.currentView = "edit"
            editScreen.newShow()
            const canvas = SelfTest.findItem("selfTestCanvas")
            const item = editScreen.addCanvasItem("text")
            if (!item || !canvas) {
                console.log("[SELFTEST-CANVAS] cannot probe: item=" + (item ? "ok" : "MISSING")
                            + " canvas=" + (canvas ? "ok" : "MISSING"))
                SelfTest.quit()
                return
            }
            window.canvasItemName = "canvasItem_" + item.key
            // The stack's own trace in the log: a shape that does not change on
            // release is answered by "who pushed what, when", not by the shape
            // alone (see AppCursor.debug).
            AppCursor.debug = true
            // The reported shape: the item framed to the canvas edge, where the
            // dots used to be clipped away (item and canvas both 754x428).
            item.x = 0
            item.y = 0
            item.width = canvas.width
            item.height = canvas.height
            item.text = "{scripture_number} {scripture_text}"
            console.log("[SELFTEST-CANVAS] framed " + window.canvasItemName + " to the canvas "
                        + Math.round(canvas.width) + "x" + Math.round(canvas.height))
            selfTestCanvasProbe.restart()
        }
    }
    Timer {
        id: selfTestCanvasProbe
        interval: 2500    // the frame + the handle animations settle
        onTriggered: {
            const item = SelfTest.findItem(window.canvasItemName)
            if (!item) {
                console.log("[SELFTEST-CANVAS] the item delegate is gone")
                SelfTest.quit()
                return
            }
            window.canvasBuildProbes("_" + window.canvasItemName)
            window.canvasIndex = 0
            window.canvasReading = false
            selfTestCanvasStep.restart()
        }
    }
    Timer {
        id: selfTestCanvasStep
        interval: 260     // one real cursor move, then one event-loop beat
        repeat: true
        onTriggered: {
            const list = window.canvasProbe
            if (window.canvasIndex >= list.length) {
                stop()
                window.canvasFinishPass(window.canvasInterior === "untested" ? "interior" : "rescale")
                return
            }
            const entry = list[window.canvasIndex]
            if (!window.canvasReading) {
                window.canvasReading = true
                if (entry.x >= 0) {
                    if (entry.press)
                        SelfTest.press(entry.x, entry.y)
                    else
                        SelfTest.move(entry.x, entry.y)
                }
                return
            }
            entry.got = AppCursor.active ? AppCursor.shape : -1
            const rescale = window.canvasInterior !== "untested"
            if (entry.label === "body" || entry.label === "press") {
                const key = entry.label === "body" ? "body" : "press"
                window.canvasStats[key] = entry.got === entry.want ? "ok" : "no"
            }
            window.canvasStats.total += 1
            if (entry.got === entry.want)
                window.canvasStats.ok += 1
            console.log("[SELFTEST-CANVAS] " + (rescale ? "rescale " : "") + entry.label
                        + " got=" + entry.got + " want=" + entry.want
                        + (entry.got === entry.want ? " ok" : " MISMATCH"))
            if (entry.release)
                SelfTest.release(entry.x, entry.y)
            window.canvasReading = false
            ++window.canvasIndex
        }
    }
    Timer {
        id: selfTestCanvasRescale
        interval: 1500    // the window resize re-lays out every scaled layer
        onTriggered: {
            SelfTest.activate()   // the resize can hand the foreground away
            window.canvasBuildProbes("_" + window.canvasItemName)
            window.canvasIndex = 0
            window.canvasReading = false
            selfTestCanvasStep.restart()
        }
    }
    // quit() DESTROYS the window while grabToImage's async render is still in
    // flight (the pattern run_exact_test.sh learned), so quit one beat later.
    Timer {
        id: selfTestCanvasDone
        interval: 2000
        onTriggered: SelfTest.quit()
    }

    // ---- Outputs · Edit -> Save (VGR_OUTSAVE_TEST=1 only) --------------------
    // User report: "the moment i add styles that is when the save button doesn't
    // respond" — Settings · Outputs · Edit stayed open over the page with a
    // "Style not saved" notice, so the Save click read as dead. Save writes the
    // row the dialog was OPENED for, and the only ways it can end without
    // closing are a row that moved and a style id the roster no longer knows —
    // both silent apart from that notice. So this scenario walks the operator's
    // own steps in the running app: add a style, open Settings · Outputs, open
    // the first output's Edit dialog, press the REAL Save button, and print what
    // the app did with it. Two passes, because the screenshot was the second
    // shape:
    //   A) the output ALREADY wears a style — the pick from an earlier session
    //      ("New Style 2" in the field) and the operator only presses Save;
    //   B) the style is CHANGED in the dialog, then saved.
    // The pair `closed=true styled=true` is the whole verdict
    // (tools/selftest/run_output_save_test.sh).
    property var outSaveScreen: null
    property string outSaveStyleId: ""
    property string outSaveWanted: ""
    property int outSaveStylesBefore: 0
    property string outSavePhase: "A"
    function outSaveDialogShown() {
        const d = SelfTest.findItem("selfTestEditOutputDialog")
        return d ? (d.opacity > 0 ? "true" : "false") : "?"
    }
    // Opens the dialog on the first output with `wanted` in its style field,
    // then presses Save (the click timer below). editStyleId is written AFTER
    // openEdit: openEdit seeds the field from the row, so setting it first would
    // be overwritten — the pull from the row is the behaviour under test in
    // pass A anyway ("the output already wears a style").
    function outSaveBeginPass(phase, wanted) {
        window.outSavePhase = phase
        window.outSaveWanted = wanted
        window.outSaveScreen.openEdit(0)
        window.outSaveScreen.editStyleId = wanted
        selfTestOutSaveClick.restart()
    }
    // One line per pass: the roster, the dialog's own state after the click,
    // the output's saved fields, and the two verdicts the script greps —
    // closed (the dialog actually went away) and styled (the style landed).
    function outSaveReport(tag) {
        const o = OutputListModel.getOutput(0)
        const styleId = o && o.styleId !== undefined ? String(o.styleId) : "?"
        const closed = window.outSaveScreen && window.outSaveScreen.editIndex < 0
        console.log("[SELFTEST-OUTSAVE] " + tag
                    + " styles=" + StyleListModel.rowCount()
                    + " rows=" + OutputListModel.rowCount()
                    + " editIndex=" + (window.outSaveScreen ? window.outSaveScreen.editIndex : "?")
                    + " dialog=" + window.outSaveDialogShown()
                    + " out0=" + JSON.stringify(o)
                    + " wanted=" + JSON.stringify(window.outSaveWanted)
                    + " closed=" + (closed ? "true" : "false")
                    + " styled=" + (styleId === window.outSaveWanted ? "true" : "false"))
    }
    Timer {
        id: selfTestOutSaveAddStyle
        running: typeof SelfTestOutputSave !== "undefined" && SelfTestOutputSave
        interval: 12000          // boot + first QML frame, like the other scenarios
        onTriggered: {
            window.outSaveStylesBefore = StyleListModel.rowCount()
            window.settingsSection = "styles"
            settingsScrim.visible = true
            SelfTest.clickItem("selfTestAddStyle")
            selfTestOutSaveSeeded.restart()
        }
    }
    Timer {
        id: selfTestOutSaveSeeded
        interval: 1500           // let the Styles screen build and the click land
        onTriggered: {
            if (StyleListModel.rowCount() === window.outSaveStylesBefore) {
                // A synthetic click needs window-foreground luck, and the driver's
                // own note says so; this is the very call the button makes, so the
                // run still reproduces "save an output with styles present".
                StyleListModel.addStyle()
                console.log("[SELFTEST-OUTSAVE] add-style click missed - called addStyle() directly")
            }
            console.log("[SELFTEST-OUTSAVE] roster before=" + window.outSaveStylesBefore
                        + " after=" + StyleListModel.rowCount())
            // Pass A's premise: the output already wears the FIRST style (what a
            // persisted assignment looks like on the next launch — the state the
            // report's screenshot was taken in).
            window.outSaveStyleId = String(StyleListModel.data(
                StyleListModel.index(0, 0), StyleListModel.IdRole))
            OutputListModel.setStyle(0, window.outSaveStyleId)
            window.settingsSection = "outputs"
            selfTestOutSaveEditA.restart()
        }
    }
    Timer {
        id: selfTestOutSaveEditA
        interval: 1500           // the section Loader rebuilds the page
        onTriggered: {
            // "outputsScreen" is the name Main.qml gives the page instance
            // (the section Loader rebuilds it on every section switch).
            const s = SelfTest.findItem("outputsScreen")
            if (!s) {
                console.log("[SELFTEST-OUTSAVE] no outputs screen — scenario cannot run")
                SelfTest.quit()
                return
            }
            window.outSaveScreen = s
            window.outSaveBeginPass("A", window.outSaveStyleId)
        }
    }
    Timer {
        id: selfTestOutSaveClick
        interval: 250            // let the dialog lay out + fade in first
        onTriggered: {
            window.outSaveReport("press" + window.outSavePhase)
            SelfTest.clickItem("selfTestEditOutputDialog_save")
            selfTestOutSaveMissed.restart()
        }
    }
    // A synthetic click needs window-foreground luck (see SelfTestDriver's own
    // note) and a miss on a modal lands on the SCRIM, which dismisses the dialog
    // with nothing saved — indistinguishable from the bug, if left silent. So a
    // miss is detected (the dialog is still up) and reported, and the pass then
    // drives the very function the Save button's click calls.
    Timer {
        id: selfTestOutSaveMissed
        interval: 450
        onTriggered: {
            const s = window.outSaveScreen
            if (s.editIndex >= 0) {
                console.log("[SELFTEST-OUTSAVE] save click missed in pass " + window.outSavePhase
                            + " — called saveEdit() directly")
                s.saveEdit()
            }
            selfTestOutSaveResult.restart()
        }
    }
    Timer {
        id: selfTestOutSaveResult
        interval: 700
        onTriggered: {
            window.outSaveReport("result" + window.outSavePhase)
            if (window.outSavePhase === "A") {
                // Pass B: CHANGE the style in the dialog, then save — the pick
                // arrives the way the picker hands it over (StylePickerModal's
                // applied -> editStyleId); the picker's own wiring is not what is
                // under test here, the Save path is.
                const last = String(StyleListModel.data(
                    StyleListModel.index(StyleListModel.rowCount() - 1, 0), StyleListModel.IdRole))
                window.outSaveBeginPass("B", last)
            } else {
                SelfTest.quit()
            }
        }
    }

    // ---- Post-fix warning regression (VGR_UIWARN_TEST=1) ---------------------
    // Two QML warnings that only REAL input produces, which is why every
    // property-level trace stayed clean while the log filled up:
    //   1. AppMenuBar, "Binding loop detected for property settingsFlyoutOpen":
    //      hovering the logo menu's "Settings" row flies the section flyout out
    //      under the pointer, which re-enters the flyout recompute.
    //   2. ReferencePane, "Cannot read property 'playPicked' of undefined":
    //      a verse double-click (double-click goes on air) that lands while the
    //      pane is being rebuilt — it is rebuilt when table.splitLongVerses
    //      flips, which is what the reporting session had just toggled.
    // Both sequences are driven here with the driver's real cursor moves and
    // real synthetic clicks. Every step is REPORTED, including the state the
    // fix is about (the flyout's own `visible`, whether a verse row existed at
    // all): a scenario that silently did nothing must not read as a pass.
    //
    // Armed only with BOTH VGR_SELFTEST=1 and VGR_UIWARN_TEST=1 (it opens menus,
    // flips a setting and quits, so a plain VGR_SELFTEST=1 run — how the NDI
    // probes launch the app — never runs it).
    property int uiWarnPhase: 0
    property bool uiWarnFlyoutOpened: false
    property bool uiWarnVerses: false
    function uiWarnItem(name) { return SelfTest.findItem(name) }
    Timer {
        id: selfTestUiWarnStart
        running: typeof SelfTestUiWarn !== "undefined" && SelfTestUiWarn
        // Boot + first frame + the pane's own content read: the menu bar is up
        // long before the panes have anything in them.
        interval: 6000
        onTriggered: {
            console.log("[SELFTEST-UIWARN] start")
            window.uiWarnPhase = 1
            SelfTest.clickItem("selfTestMenuLogo")   // open the logo menu
            selfTestUiWarnTick.restart()
        }
    }
    Timer {
        id: selfTestUiWarnTick
        interval: 1200
        repeat: true
        onTriggered: {
            switch (window.uiWarnPhase) {
            case 1: {
                // Hover the "Settings" row for real: this is what makes the
                // flyout fly out (settingsRowHovered -> recompute).
                const row = window.uiWarnItem("selfTestDropdownRow_Settings")
                if (!row) {
                    console.log("[SELFTEST-UIWARN] logo menu did not open (no Settings row)")
                    window.uiWarnPhase = 3
                    return
                }
                const c = row.mapToItem(null, row.width / 2, row.height / 2)
                SelfTest.move(c.x, c.y)
                window.uiWarnPhase = 2
                return
            }
            case 2: {
                const fly = window.uiWarnItem("selfTestSettingsFlyout")
                window.uiWarnFlyoutOpened = !!(fly && fly.visible)
                console.log("[SELFTEST-UIWARN] settings flyout visible=" + window.uiWarnFlyoutOpened)
                // Cross the real gap onto the flyout's own first row and down it
                // — the diagonal the operator's hand makes, and the crossings
                // that deliver the row enters the recompute re-enters on.
                const g = window.uiWarnItem("selfTestDropdownRow_General")
                const s = window.uiWarnItem("selfTestDropdownRow_Styles")
                if (g) {
                    const cg = g.mapToItem(null, g.width / 2, g.height / 2)
                    SelfTest.move(cg.x, cg.y)
                }
                if (s) {
                    const cs = s.mapToItem(null, s.width / 2, s.height / 2)
                    SelfTest.move(cs.x, cs.y)
                }
                window.uiWarnPhase = 3
                return
            }
            case 3: {
                // The verse half. The Table tab hosts the pane under test.
                SelfTest.clickItem("selfTestTab_table")
                window.uiWarnPhase = 4
                return
            }
            case 4: {
                const row = window.uiWarnItem("selfTestVerseRow_1")
                if (!row) {
                    if (window.uiWarnTries++ < 8)
                        return          // the pane may still be loading its passage
                    console.log("[SELFTEST-UIWARN] no verse rows in the pane: double-click NOT exercised")
                    window.uiWarnPhase = 6
                    return
                }
                window.uiWarnVerses = true
                // The reported trigger first: flip the rendering setting the
                // pane reads, twice, exactly as the settings toggle does. The
                // rebuild it causes is what the double-click then arrives into.
                const now = SettingsService.value("table.splitLongVerses") === true
                SettingsService.setValue("table.splitLongVerses", !now)
                SettingsService.setValue("table.splitLongVerses", now)
                window.uiWarnPhase = 5
                return
            }
            case 5: {
                // Double-click the row: two synthetic clicks inside Qt's
                // double-click interval are one double-click (opened -> the
                // handler that selects AND goes on air).
                SelfTest.clickItem("selfTestVerseRow_1")
                SelfTest.clickItem("selfTestVerseRow_1")
                console.log("[SELFTEST-UIWARN] verse double-click sent (pane owner re-resolved by the handler)")
                window.uiWarnPhase = 6
                return
            }
            default:
                selfTestUiWarnTick.stop()
                console.log("[SELFTEST-UIWARN] done flyoutOpened=" + window.uiWarnFlyoutOpened
                            + " verses=" + window.uiWarnVerses)
                SelfTest.quit()
                return
            }
        }
    }
    property int uiWarnTries: 0

    // ---- Hardware rendering switch (VGR_GPU_TEST=1) --------------------------
    // The report it answers: "can add a toggle in general so in case a user want
    // to turn it by them self but it should [be] software by default as not all
    // laptops have gpu". Two things have to be true, and only the RUNNING app can
    // show either of them: the row is there and its switch really writes the
    // setting, and the ENGINE really changes what it draws with — including the
    // case the report is about, a machine whose GPU cannot be used, where the
    // switch reads ON while the frames are still software.
    //
    // Every step prints THREE answers, never one: the stored value, what the
    // engine says it is drawing with (SettingsService.rendererInUse — the backend
    // really installed, not the setting), and the sentence the row itself DRAWS
    // under the switch (selfTestNote_preferences.hardwareAcceleration). A row
    // that echoed the switch back at the user instead of the truth is then
    // visible in one line, which is the half no property trace can see.
    //
    // Borrow and put back: it flips the switch away from whatever it found and
    // restores it before quitting, and the runner's verdict requires the restore
    // — a scenario that left the GPU path on would hand the next launch the black
    // picture this switch exists to avoid.
    property int gpuPhase: 0
    property int gpuTries: 0
    property bool gpuWasOn: false
    property bool gpuNowOn: false
    property bool gpuConsistent: false
    property int gpuClicks: 0
    property string gpuBy: ""
    property string gpuOnEngine: ""
    property string gpuOffEngine: ""
    readonly property string gpuKey: "preferences.hardwareAcceleration"
    readonly property string gpuToggleName: "selfTestToggle_preferences.hardwareAcceleration"
    function gpuValue() { return SettingsService.values[window.gpuKey] === true }
    // Is the row's toggle inside the WINDOW (not merely "visible")? An item
    // scrolled out of a Flickable keeps isVisible() true because the Flickable
    // CLIPS instead of hiding — which is exactly how a click aimed at it misses:
    // its centre maps outside the window entirely (measured: y=1208 on a 900-tall
    // window). The driver's own itemVisible cannot see that, so the scenario
    // checks the coordinate it is about to click.
    function gpuToggleOnScreen() {
        const c = SelfTest.itemCenter(window.gpuToggleName)
        return c.x > 0 && c.y > 0 && c.x < window.width && c.y < window.height
    }
    function gpuNoteText() {
        const t = SelfTest.findItem("selfTestNote_" + window.gpuKey)
        return t ? String(t.text) : ""
    }
    // The row's own NAME — the line above the note, straight off the setting's
    // definition. Reported because a row that rendered as a blank line with a
    // switch on it would still be clickable: this is the difference between
    // "the switch works" and "the switch is SHOWING", which is the question that
    // was actually asked. Last on the result line, so its spaces need no quoting.
    function gpuLabelText() {
        const t = SelfTest.findItem("selfTestLabel_" + window.gpuKey)
        return t ? String(t.text) : ""
    }
    // Does the row's own sentence agree with the backend that is really
    // installed? The wording is QML's and stays there; this only asks the two
    // about the SAME fact — the engine says GPU and the row talks about the card,
    // or the engine says software while the switch is on and the row says so.
    function gpuNoteConsistent(note) {
        const saysNoGpu = note.indexOf("No usable GPU") >= 0
        const saysCard = note.indexOf("graphics card") >= 0
        if (SettingsService.rendererOnGpu)
            return saysCard && !saysNoGpu
        return window.gpuNowOn ? saysNoGpu : !saysNoGpu
    }
    // Writes the value the toggle's own handler writes — the documented fallback
    // for a synthetic click that missed (see SelfTestDriver's foreground note).
    function gpuSet(on) {
        SettingsService.setValue(window.gpuKey, on)
    }
    Timer {
        id: selfTestGpuStart
        running: typeof SelfTestGpu !== "undefined" && SelfTestGpu
        interval: 9000            // boot, then the store's own read (see whenReady)
        onTriggered: {
            window.openSettings("general")
            selfTestGpuTick.start()
        }
    }
    Timer {
        id: selfTestGpuTick
        interval: 1200
        repeat: true
        onTriggered: {
            switch (window.gpuPhase) {
            case 0: {
                // Scroll the row into view FIRST: a click at an unlaid-out item's
                // coordinates lands on whatever else is there, and the Preferences
                // card sits below the fold of this page.
                const flick = SelfTest.findItem("selfTestGeneralFlick")
                const toggle = SelfTest.findItem(window.gpuToggleName)
                if (!flick || !toggle) {
                    if (window.gpuTries++ < 8)
                        return
                    console.log("[SELFTEST-GPU] result missing=yes flick=" + !!flick
                                + " toggle=" + !!toggle)
                    SelfTest.quit()
                    return
                }
                if (!window.gpuToggleOnScreen()) {
                    // Scroll the row to the MIDDLE of the viewport and look
                    // again next tick: the click has to be inside the window.
                    const y = toggle.mapToItem(flick.contentItem, 0, 0).y
                    const max = Math.max(0, flick.contentHeight - flick.height)
                    flick.contentY = Math.min(max, Math.max(0, y - flick.height / 2))
                    if (window.gpuTries++ < 12) {
                        const c = SelfTest.itemCenter(window.gpuToggleName)
                        console.log("[SELFTEST-GPU] scrolling to the row (toggle at "
                                    + Math.round(c.x) + "," + Math.round(c.y)
                                    + " contentY=" + Math.round(flick.contentY)
                                    + " of " + Math.round(max) + ")")
                        return
                    }
                    console.log("[SELFTEST-GPU] result missing=yes toggle=notonscreen")
                    SelfTest.quit()
                    return
                }
                window.gpuWasOn = window.gpuValue()
                window.gpuNowOn = !window.gpuWasOn
                console.log("[SELFTEST-GPU] switch before=" + window.gpuWasOn
                            + " clicking for=" + window.gpuNowOn)
                SelfTest.activate()
                SelfTest.clickItem(window.gpuToggleName)
                window.gpuClicks = 1
                window.gpuPhase = 1
                return
            }
            case 1: {
                if (window.gpuValue() !== window.gpuNowOn) {
                    // A synthetic click needs window-foreground luck (see
                    // SelfTestDriver's own note). Twice more before giving up, and
                    // the path that finally engaged is printed either way — a run
                    // where the click really took is the one that also proves the
                    // row is clickable, so a fallback must not read as a pass
                    // silently.
                    if (window.gpuClicks < 3) {
                        window.gpuClicks++
                        if (window.gpuClicks === 2) {
                            const c = SelfTest.itemCenter(window.gpuToggleName)
                            console.log("[SELFTEST-GPU] click missed at " + Math.round(c.x) + "," + Math.round(c.y)
                                        + " — retrying")
                        }
                        SelfTest.activate()
                        SelfTest.clickItem(window.gpuToggleName)
                        return
                    }
                    window.gpuBy = "setValue"
                    window.gpuSet(window.gpuNowOn)
                } else {
                    window.gpuBy = "click"
                }
                window.gpuTries = 0
                window.gpuPhase = 2
                return
            }
            case 2: {
                // The apply is a WORKER round-trip (SettingsService pushes the
                // preference to the engine off the interface thread), so the
                // engine's answer may arrive a tick or two after the write.
                if (SettingsService.rendererInUse === "" && window.gpuTries++ < 10)
                    return
                window.gpuOnEngine = SettingsService.rendererInUse
                window.gpuConsistent = window.gpuNoteConsistent(window.gpuNoteText())
                console.log("[SELFTEST-GPU] on value=" + window.gpuValue()
                            + " engine=" + window.gpuOnEngine
                            + " gpu=" + SettingsService.rendererOnGpu
                            + " consistent=" + window.gpuConsistent
                            + " by=" + window.gpuBy
                            + " note=" + window.gpuNoteText())
                window.gpuTries = 0
                window.gpuPhase = 3
                return
            }
            case 3: {
                // Put the operator's switch back exactly where it was found (the
                // same click, and the same retry-then-write fallback: a miss here
                // is reported by the field below, never assumed away).
                SelfTest.activate()
                SelfTest.clickItem(window.gpuToggleName)
                window.gpuClicks = 0
                window.gpuPhase = 4
                return
            }
            case 4: {
                if (window.gpuValue() !== window.gpuWasOn) {
                    if (window.gpuClicks < 3) {
                        window.gpuClicks++
                        SelfTest.activate()
                        SelfTest.clickItem(window.gpuToggleName)
                        return
                    }
                    window.gpuSet(window.gpuWasOn)
                }
                window.gpuTries = 0
                window.gpuPhase = 5
                return
            }
            case 5: {
                if (SettingsService.rendererOnGpu && window.gpuTries++ < 10)
                    return            // still on the backend the flip installed
                window.gpuOffEngine = SettingsService.rendererInUse
                console.log("[SELFTEST-GPU] result restored=" + (window.gpuValue() === window.gpuWasOn)
                            + " was=" + window.gpuWasOn
                            + " value=" + window.gpuValue()
                            + " engine=" + window.gpuOffEngine
                            + " gpu=" + SettingsService.rendererOnGpu
                            + " onEngine=" + window.gpuOnEngine
                            + " onConsistent=" + window.gpuConsistent
                            + " by=" + (window.gpuBy === "" ? "none" : window.gpuBy)
                            + " label=" + window.gpuLabelText())
                selfTestGpuTick.stop()
                SelfTest.quit()
                return
            }
            }
        }
    }

    // ---- What each output's style does to a slide (VGR_OUTS_TEST=1) --------
    // The report this answers: "scripture and The Table work, shows do not" —
    // a style that restyles the content tabs' designs, and one that lays shows
    // out as well, are different pictures on air and were indistinguishable on
    // this screen (a row said WHICH style it wears and nothing about what that
    // style does). The row now prints the engine's own answer
    // (OutputListModel::styleLayoutNote -> SlideRules::LayoutScope), so what is
    // checked here is the ROW AS RENDERED: the sentence reaches the delegate,
    // every styled row has one, and it is one of the documented answers rather
    // than an empty or stale line.
    //
    // Reads the card item by its objectName and its own `styleLayout`
    // property — the same value the Text under the Style pill binds to — so a
    // role that resolved in the model but never reached the row is visible.
    property int outsPhase: 0
    property int outsTries: 0
    readonly property var outsScopePhrases: [
        "Restyles Scripture and The Table",   // content families, shows keep their own
        "Lays out shows too",   // the style's Shows slot names a template
        "No layout of its own"                // background and placement only
    ]
    function outsSample() {
        // `reported` is the other half of this scenario: EVERY row must say
        // something about its own runtime, whatever its kind. The live line used
        // to be NDI/SDI-only, so an HDMI row was silent — read by the operator as
        // "the notification is only instantiated in one output". A row whose
        // statusText is empty, or does not open with its own kind, is exactly
        // that fault, counted here and failed by the runner.
        const found = { rows: 0, styled: 0, described: 0, reported: 0, lines: [] }
        for (let i = 0; i < 24; ++i) {
            const item = SelfTest.findItem("selfTestOutputRow_" + i)
            if (!item)
                break
            ++found.rows
            const styleName = String(item.styleName)
            const note = String(item.styleLayout)
            let named = "none"
            for (const phrase of window.outsScopePhrases)
                if (note.indexOf(phrase) === 0) { named = phrase; break }
            if (styleName !== "None" && styleName !== "") {
                ++found.styled
                if (named !== "none")
                    ++found.described
            }
            const status = String(item.statusText)
            const reports = status.indexOf(String(item.kind) + " \u00b7 ") === 0
            if (reports)
                ++found.reported
            found.lines.push("[SELFTEST-OUTS] row=" + i + " output=\"" + String(item.name)
                             + "\" kind=\"" + String(item.kind) + "\" status=\"" + status + "\""
                             + " style=\"" + styleName + "\" scope=\"" + named
                             + "\" note=\"" + note + "\"")
        }
        return found
    }
    Timer {
        id: selfTestOutsStart
        running: typeof SelfTestOutputsLayout !== "undefined" && SelfTestOutputsLayout
        interval: 9000            // boot, then the style roster adopts
        onTriggered: {
            window.openSettings("outputs")
            selfTestOutsTick.start()
        }
    }
    Timer {
        id: selfTestOutsTick
        interval: 700
        repeat: true
        onTriggered: {
            // Wait for the rows to exist AND for their style names to resolve:
            // the roster is adopted a moment after boot, and a row read before
            // that says "None" for a style it really wears.
            if (window.outsPhase === 0) {
                const probe = SelfTest.findItem("selfTestOutputRow_0")
                if (!probe) {
                    if (window.outsTries++ > 20) {
                        console.log("[SELFTEST-OUTS] result missing=yes rows=0")
                        selfTestOutsTick.stop()
                        SelfTest.quit()
                    }
                    return
                }
                window.outsPhase = 1
                return
            }
            const found = window.outsSample()
            for (const line of found.lines)
                console.log(line)
            console.log("[SELFTEST-OUTS] result missing=no rows=" + found.rows
                        + " styled=" + found.styled + " described=" + found.described
                        + " reported=" + found.reported)
            selfTestOutsTick.stop()
            SelfTest.quit()
        }
    }

    // ---- Every row's LIVE runtime line (VGR_OUTSLIVE_TEST=1) -----------------
    // The other half of the report the block above answers: "the notification is
    // only instantiated in one output". The row's runtime line must exist for
    // EVERY kind of output, and must say the same things about all of them — a
    // live feed with its delivered-frame count, and the stage-wide blackout. So
    // this scenario puts one real slide on air, opens Settings · Outputs, and
    // prints each row's statusText while live, while blacked out (a real click
    // on the wall's control), and after the blackout ends. No fixture is saved
    // and nothing is staged: a failure here is about the rows, not the show.
    function outsLiveSample(tag) {
        let rows = 0
        let reporting = 0
        let blackout = 0
        let offAir = 0
        for (let i = 0; i < 24; ++i) {
            const item = SelfTest.findItem("selfTestOutputRow_" + i)
            if (!item)
                break
            ++rows
            const status = String(item.statusText)
            const kind = String(item.kind)
            // A row REPORTS when its line opens with its own kind. A row that
            // says nothing (the NDI/SDI-only line this replaced) fails here.
            if (status.indexOf(kind + " \u00b7 ") === 0)
                ++reporting
            if (status.indexOf("blackout up") >= 0)
                ++blackout
            if (status.indexOf("go live to send") >= 0)
                ++offAir
            console.log("[SELFTEST-OUTSLIVE] " + tag + " row=" + i + " kind=\"" + kind
                        + "\" status=\"" + status + "\"")
        }
        console.log("[SELFTEST-OUTSLIVE] " + tag + " result rows=" + rows
                    + " reporting=" + reporting + " blackout=" + blackout
                    + " offair=" + offAir)
        // ...and the SAME state on the wall itself: each tile carries it as a
        // DOT (no text — the row owns the words), and runtimeState is read here
        // too (the dialog sits over the wall, which is still there and still
        // bound), so "on the tile as well" is checked, not assumed. A tile that
        // cannot be found is printed as <missing> and the runner says so rather
        // than passing on the dialog alone.
        const wallTiles = []
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const tile = window.tfTile(i)
            wallTiles.push(i + ":" + (tile ? String(tile.runtimeState) : "<missing>"))
        }
        console.log("[SELFTEST-OUTSLIVE] " + tag + " tiles=" + JSON.stringify(wallTiles.join(" | ")))
    }
    // The wall's Blackout control is BEHIND the Settings modal, so every press
    // closes the dialog first and the next read reopens it: a click that lands on
    // the scrim proves nothing, and this scenario is about the ROWS, not about the
    // control (run_blackout_test.sh drives that one). The press itself is still the
    // real control — SelfTest.clickItem, the operator's own gesture.
    Timer {
        id: selfTestOutsLiveStart
        running: typeof SelfTestOutputsLive !== "undefined" && SelfTestOutputsLive
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
            const slide = {
                valid: true,
                title: "Outputs Live Probe",
                text: "outputs live probe",
                background: "",
                blocks: [ { kind: "text", text: "outputs live probe", x: 60, y: 60,
                            width: 630, height: 300, meta: { fontSize: 48 } } ]
            }
            LiveOutputService.goLiveWithSlides("Outputs Live Probe", [ slide ])
            window.openSettings("outputs")
            selfTestOutsLiveRead.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveRead
        interval: 4000           // the 1 Hz probe has published a state by now
        onTriggered: {
            window.outsLiveSample("live")
            // THE DIALOG CLOSES FIRST — the wall is underneath it. Then the
            // pointer is put on a tile's runtime DOT and the tip it raises is
            // read: the dot is colour-only, so the numbers (frames + age) have
            // to be reachable by hover or they are not reachable at all.
            settingsScrim.visible = false
            const dot = SelfTest.itemCenter("selfTestTileRuntime")
            window.outsHoverAt = dot
            SelfTest.move(dot.x, dot.y)
            selfTestOutsLiveHover.restart()
        }
    }
    property var outsHoverAt: null
    property string outsHoverTip: ""
    Timer {
        id: selfTestOutsLiveHover
        interval: 900            // the tip's own 350 ms show delay + placement
        onTriggered: {
            window.outsHoverTip = String(TipService.text)
            console.log("[SELFTEST-OUTSLIVE] hover tip=" + JSON.stringify(window.outsHoverTip))
            // Off the dot again (a tip must not outlive the hover), then the
            // blackout press, which is what the next phase reads.
            if (window.outsHoverAt)
                SelfTest.move(window.outsHoverAt.x, window.outsHoverAt.y + 140)
            SelfTest.clickItem("selfTestWallBlackout")
            selfTestOutsLiveBlackedOpen.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveBlackedOpen
        interval: 3000           // the carrier and the 1 Hz probe both land
        onTriggered: {
            window.openSettings("outputs")
            selfTestOutsLiveBlackedSample.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveBlackedSample
        interval: 2500           // rows exist again, the dialog has laid out
        onTriggered: {
            window.outsLiveSample("blackout")
            settingsScrim.visible = false
            SelfTest.clickItem("selfTestWallBlackout")
            selfTestOutsLiveRestoredOpen.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveRestoredOpen
        interval: 3000
        onTriggered: {
            window.openSettings("outputs")
            selfTestOutsLiveRestoredSample.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveRestoredSample
        interval: 2500
        onTriggered: {
            window.outsLiveSample("restored")
            LiveOutputService.stop()
            selfTestOutsLiveQuit.restart()
        }
    }
    Timer {
        id: selfTestOutsLiveQuit
        interval: 1200
        onTriggered: SelfTest.quit()
    }

    // ---- Style background reaches the output tiles (VGR_STYLEBG_TEST=1) -----
    // The second half of the same report ("i select the style for output when
    // there was no bg image in it, then i went to add the bg image — the output
    // screen did not update its bg image, it still remained transparent"): a
    // style's background IMAGE is composed per output at read time, so the
    // question is whether the OUTPUT TILE re-reads it — the model changing is
    // not the same thing as the tile repainting, and only the tile is what the
    // operator looked at. This scenario drives the model exactly as the Styles
    // dialog does (clear the image, then add one) and prints the TILE's own
    // styleBg at each step beside the model's answer for the same output, so a
    // tile that lags the model is visible in one line.
    property var bsTileItem: null
    // The measured output's style assignment as it was BEFORE the scenario
    // borrowed it — put back at the end, same borrow-and-return rule the
    // style's background image follows (a test must not leave the operator's
    // roster pointing somewhere they never clicked).
    property string bsOriginalStyleId: ""
    property bool bsPreviewSameRoster: false
    property int bsTileIndex: -1
    property int bsStyleRow: 0
    property string bsStyleId: ""
    property string bsImage: ""
    // The style's own image before the scenario touched it, so the run puts the
    // operator's roster back the way it found it (this scenario MUTATES a
    // style: it clears the image, adds one, then restores this).
    property string bsOriginalImage: ""
    function bsFindImage() {
        for (let i = 0; i < StyleListModel.rowCount(); ++i) {
            const p = StyleListModel.getStyle(i).backgroundImage
            if (p !== undefined && p !== "" && SettingsService.fileExists(p))
                return String(p)
        }
        // Nothing in the roster carries a usable image (a fresh roster): any
        // real file on this machine does, because hasImage is about the file
        // being THERE — the tile's decode is a separate concern.
        const candidates = ["C:/Windows/Web/Wallpaper/Windows/img0.jpg",
                            "C:/Windows/Web/Wallpaper/Windows/img1.jpg",
                            "C:/Windows/Web/Screen/img100.jpg"]
        for (const c of candidates)
            if (SettingsService.fileExists(c))
                return c
        return ""
    }
    // The SHOW wall's tile n, found by descending from the wall itself. Both
    // hosts (the Show screen and the Edit screen's ITEMS tab) build tiles named
    // "selfTestMonitorTile<n>", so a window-wide findItem can hand back the
    // other surface's tile — a different output, whose style this scenario
    // never touched, which is exactly how the first version of this scenario
    // misread its own evidence ("cleared" reported an image while the style
    // under test had none, because the tile and the model answer were about two
    // different outputs).
    function bsFindTile(wall, index) {
        if (!wall)
            return null
        if (wall.objectName === "selfTestMonitorTile" + index)
            return wall
        const kids = wall.children
        if (!kids)
            return null
        for (let i = 0; i < kids.length; ++i) {
            const hit = window.bsFindTile(kids[i], index)
            if (hit)
                return hit
        }
        return null
    }
    // LAYER ORDER inside the tile under test (run_style_bg_test.sh): the
    // transparency checkerboard must sit BELOW the media/block layers in the
    // pane's child order, or it paints over them — the reported "the
    // checkerboard area is where the media is meant to show" (z-order, not
    // styling: identical visible data, wrong picture). Walks the pane's
    // children by the names OutputMonitorTile gives them.
    function bsLayerFacts() {
        return window.bsLayerFactsOf(window.bsTileItem)
    }
    function bsLayerFactsOf(tile) {
        if (!tile)
            return { line: "tileLayer=none" }
        const pane = window.bsLayerFacts_find(tile, "selfTestTilePane")
        if (!pane)
            return { line: "tilePane=missing" }
        const kids = pane.children
        let checkers = -1, design = -1, media = -1, backdrop = -1, feed = -1, order = []
        for (let i = 0; i < kids.length; ++i) {
            const n = String(kids[i].objectName || "")
            order.push(n !== "" ? n.replace("selfTestTile", "") : "-")
            if (n === "selfTestTileCheckers") checkers = i
            else if (n === "selfTestTileDesignPreview") design = i
            else if (n === "selfTestTileMediaFrame") media = i
            else if (n === "selfTestTileBackdropPreview") backdrop = i
            else if (n === "selfTestTileInputFeed") feed = i
        }
        const belowContent = checkers >= 0 && design >= 0 && checkers < design
            && (media < 0 || checkers < media)
        // THE FEED'S OWN ORDER, read off the tile's layer stack: the design's
        // backdrop paint below the taken feed, the feed below the block content
        // drawn over it — the engine's composition (bg < media < text) said as
        // positions here. run_blank_live_test.sh checks it, because "the tile
        // and the output agree about what covers a feed" is exactly what a
        // preview-side copy of the rule breaks.
        const feedOverBackdrop = backdrop < 0 || feed < 0 || backdrop < feed
        const contentOverFeed = feed < 0 || design < 0 || feed < design
        return { line: "tileLayer=checkers" + checkers + " design" + design
                      + " media" + media + " backdrop" + backdrop + " feed" + feed
                      + " belowContent=" + String(belowContent)
                      + " feedOverBackdrop=" + String(feedOverBackdrop)
                      + " contentOverFeed=" + String(contentOverFeed)
                      + " order=" + order.join(","),
                 belowContent: belowContent,
                 feedOverBackdrop: feedOverBackdrop,
                 contentOverFeed: contentOverFeed }
    }
    // Recursive helper (QML functions can't declare nested named functions
    // that recurse cleanly).
    function bsLayerFacts_find(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        const kids = item.children
        if (!kids)
            return null
        for (let i = 0; i < kids.length; ++i) {
            const hit = window.bsLayerFacts_find(kids[i], name)
            if (hit)
                return hit
        }
        return null
    }

    // The tile's OWN output index drives the model answer, so the two halves of
    // every line are always about the same output. styleMatch=true is the
    // premise check the shell script gates on: the tile really wears the style
    // this scenario edits.
    function bsReport(tag) {
        const tile = window.bsTileItem
        if (tag === "cleared")
            console.log("[SELFTEST-STYLEBG] tile layers: " + window.bsLayerFacts().line)
        const bg = tile ? tile.styleBg : null
        const ti = window.bsTileIndex
        const modelBg = ti >= 0 ? OutputListModel.styleBackground(ti) : null
        console.log("[SELFTEST-STYLEBG] " + tag
                    + " tile=" + (tile ? "yes" : "no")
                    + " tileIndex=" + ti
                    + " tileStyleId=" + (tile ? String(tile.styleId) : "?")
                    + " styleMatch=" + (tile ? String(String(tile.styleId) === window.bsStyleId) : "?")
                    + " tileHasImage=" + (bg ? String(bg.hasImage) : "?")
                    + " tileImage=" + JSON.stringify(bg ? String(bg.image) : "")
                    + " tileColor=" + (bg ? String(bg.color) : "?")
                    + " tileStyled=" + (tile ? String(tile.styled) : "?")
                    + " modelHasImage=" + (modelBg ? String(modelBg.hasImage) : "?")
                    + " styleImage=" + JSON.stringify(String(StyleListModel.getStyle(window.bsStyleRow).backgroundImage)))
    }
    // ---- What each wall tile is drawing (VGR_TILEFACTS_TEST=1) -------------
    // A DIAGNOSTIC, not a gate: the reported two-tile disagreement is a question
    // about STATE, and only the tiles' own facts can answer it — two tiles are
    // fed the same on-air/staged slide but wear their OWN style, and each draws
    // from its own layer stack (the pane's checkerboard, the style image, a
    // taken file's frames, the design blocks, the distributed frame). It prints,
    // per tile, the style it wears, the composition's own facts and which of the
    // pane's layers is visible, in three states: nothing on air, a media FILE
    // taken, and live.
    property string tfMedia: "C:/Users/znwaj/Documents/Iwaya Churh Live Stream/9_16 harvest.jpeg"
    // The tile is reached by DESCENDING FROM THE WALL, never by a window-wide
    // findItem: both hosts name their tiles "selfTestMonitorTile<n>", so a
    // window search can hand back the OTHER surface's tile — the very mistake
    // bsFindTile's comment records from an earlier scenario.
    function tfTile(i) {
        return window.bsFindTile(SelfTest.findItem("selfTestMonitorWall"), i)
    }
    function tfLine(phase, i) {
        const tile = window.tfTile(i)
        if (!tile)
            return "[SELFTEST-TILES] phase=" + phase + " tile=" + i + " missing=yes"
        const s = tile.onAirSlide ? tile.onAirSlide : ({})
        const row = OutputListModel.getOutput(i) || {}
        const pane = window.bsLayerFacts_find(tile, "selfTestTilePane")
        const checkers = window.bsLayerFacts_find(tile, "selfTestTileCheckers")
        const design = window.bsLayerFacts_find(tile, "selfTestTileDesignPreview")
        const media = window.bsLayerFacts_find(tile, "selfTestTileMediaFrame")
        const feed = window.bsLayerFacts_find(tile, "selfTestTileInputFeed")
        const frame = window.bsLayerFacts_find(tile, "selfTestTileFrame")
        return "[SELFTEST-TILES] phase=" + phase + " tile=" + i
             + " styleId=" + JSON.stringify(String(tile.styleId))
             + " active=" + tile.active + " enabled=" + tile.isEnabled
             // The tile's own runtime strip (frames / age / blackout / no feed) —
             // the same facts the Settings · Outputs row prints, read off the
             // tile as rendered so a strip that never binds is visible here.
             + " runtime=" + JSON.stringify(String(tile.runtimeState))
             + " rtFrames=" + (tile.runtime ? tile.runtime.frames : -1)
             + " rtAge=" + (tile.runtime ? tile.runtime.ageMs : -1)
             + " modelFit=" + JSON.stringify(String(row.mediaFit))
             + " live=" + LiveOutputService.live
             + " hasFrame=" + tile.hasFrame + " framePriority=" + tile.framePriority
             + " hasSlidePreview=" + tile.hasSlidePreview
             + " mediaOnAir=" + tile.mediaOnAir + " inputTaken=" + tile.inputTaken
             + " fit=" + JSON.stringify(String(tile.effectiveFit))
             + " fillMode=" + tile.effectiveFillMode
             + " | map valid=" + s.valid
             + " explicitTransparent=" + s.explicitTransparent
             + " stylePaintless=" + s.stylePaintless
             + " frameTransparent=" + s.frameTransparent
             + " styleColor=" + JSON.stringify(String(s.styleColor))
             + " background=" + JSON.stringify(String(s.background))
             + " styleImagePainted=" + s.styleImagePainted
             + " mediaFitComposed=" + JSON.stringify(String(s.mediaFit))
             + " clearedByTake=" + s.clearedByTake
             + " blocks=" + (s.blocks ? s.blocks.length : -1)
             + " | layers paneColor=" + JSON.stringify(String(pane ? pane.color : "?"))
             + " checkers=" + (checkers ? checkers.visible : "?")
             + " design=" + (design ? design.visible : "?")
             + " media=" + (media ? media.visible : "?") + "/fill" + (media ? String(media.fillMode) : "?")
             + "/src" + (media ? String(media.source).substring(0, 24) : "?")
             + " frame=" + (frame ? frame.visible : "?") + "/src" + (frame ? String(frame.source).substring(0, 40) : "?")
             + " input=" + (feed ? feed.visible : "?")
             + " | tileW=" + Math.round(tile.width) + " paneW=" + Math.round(pane ? pane.width : 0)
    }
    function tfReport(phase) {
        const count = OutputListModel.rowCount()
        for (let i = 0; i < count; ++i) {
            console.log(window.tfLine(phase, i))
            // A FRESH composition for the same (slide, style) as the tile just
            // read. The tile's binding recomposes only when its INPUTS move, and
            // the take is not one of them — so comparing the tile's cached
            // answer with a call made now separates "the composition is stale"
            // from "the composition really says this".
            const tile = window.tfTile(i)
            if (!tile)
                continue
            const raw = LiveOutputService.stagedSlideRaw
            const useRaw = (raw && Object.keys(raw).length > 0) ? raw : LiveOutputService.onAirSlideRaw
            const fresh = LiveOutputService.previewWithStyle(
                (useRaw && typeof useRaw === "object") ? useRaw : {}, String(tile.styleId))
            console.log("[SELFTEST-TILES] phase=" + phase + " tile=" + i + " fresh"
                        + " clearedByTake=" + fresh.clearedByTake
                        + " frameTransparent=" + fresh.frameTransparent
                        + " stylePaintless=" + fresh.stylePaintless
                        + " styleImagePainted=" + fresh.styleImagePainted
                        + " styleColor=" + JSON.stringify(String(fresh.styleColor))
                        + " blocks=" + (fresh.blocks ? fresh.blocks.length : -1))
        }
        // The shared state both tiles read, printed once: anything that differs
        // per tile above is NOT sourced here.
        const staged = LiveOutputService.stagedSlideRaw
        console.log("[SELFTEST-TILES] phase=" + phase + " roster=" + count
                    + " live=" + LiveOutputService.live
                    + " mediaOnAir=" + LiveOutputService.mediaOnAir
                    + " mediaLabel=" + JSON.stringify(String(LiveOutputService.mediaMeterLabel))
                    + " mediaFitService=" + JSON.stringify(String(LiveOutputService.mediaFit))
                    + " frameRev=" + LiveOutputService.frameRev
                    + " mediaRev=" + LiveOutputService.mediaRev
                    + " stagedKeys=" + (staged ? Object.keys(staged).length : 0))
    }
    // A PNG of every tile, so the facts above can be checked against what the
    // operator actually sees (the report is about a PICTURE, not a property).
    function tfGrab(phase) {
        const count = OutputListModel.rowCount()
        for (let i = 0; i < count; ++i) {
            const ok = SelfTest.grab("selfTestMonitorTile" + i,
                                     "tilefacts_" + phase + "_" + i + ".png")
            console.log("[SELFTEST-TILES] grab phase=" + phase + " tile=" + i + " ok=" + ok)
        }
    }
    Timer {
        id: selfTestTileFactsStart
        running: typeof SelfTestTileFacts !== "undefined" && SelfTestTileFacts
        interval: 9000
        onTriggered: {
            window.tfReport("idle")
            window.tfGrab("idle")
            selfTestTileFactsMedia.restart()
        }
    }
    Timer {
        id: selfTestTileFactsMedia
        interval: 3000
        onTriggered: {
            LiveOutputService.takeMedia(window.tfMedia, "9_16 harvest.jpeg")
            console.log("[SELFTEST-TILES] takeMedia label=" + JSON.stringify(String(LiveOutputService.mediaMeterLabel)))
            selfTestTileFactsAfterMedia.restart()
        }
    }
    Timer {
        id: selfTestTileFactsAfterMedia
        interval: 5000
        onTriggered: {
            window.tfReport("media")
            window.tfGrab("media")
            selfTestTileFactsAfterLive.restart()
        }
    }
    Timer {
        id: selfTestTileFactsAfterLive
        interval: 4000
        onTriggered: {
            window.tfReport("media2")
            window.tfGrab("media2")
            LiveOutputService.goLive()
            selfTestTileFactsLiveReport.start()
        }
    }
    Timer {
        id: selfTestTileFactsLiveReport
        interval: 6000
        onTriggered: {
            window.tfReport("live")
            window.tfGrab("live")
            // The grabs are PNG writes queued on the GUI thread; quitting in
            // this same tick dropped the last phase's files, so the quit waits
            // one more turn.
            selfTestTileFactsQuit.start()
        }
    }
    Timer {
        id: selfTestTileFactsQuit
        interval: 1500
        onTriggered: SelfTest.quit()
    }

    // ---- Blank GO LIVE carrier + a live input (VGR_BLANKLIVE_TEST=1) --------
    // The reported "the moment it is only media it doesn't go": take a real
    // window input, then press GO LIVE with nothing staged — which sends the ONE
    // blank carrier slide so the outputs show the compositor's layers (the take)
    // over the active style. Whatever the engine does with that scene is what
    // the operator's output, NDI feed and monitor wall show.
    property string blLabel: ""
    Timer {
        id: selfTestBlankLiveStart
        running: typeof SelfTestBlankLive !== "undefined" && SelfTestBlankLive
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
            // The SAME roster the Media pane's own take reads: a "screen" row
            // whose sublabel names a captured window (the first one that is not
            // this app's own window).
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                const label = String(row && row.sublabel ? row.sublabel : "").trim()
                if (String(row.kind || "") !== "screen" || label === "")
                    continue
                if (label === Qt.application.name)
                    continue
                window.blLabel = label
                break
            }
            LiveOutputService.takeInput(window.blLabel, "screen", "")
            console.log("[SELFTEST-BLANKLIVE] take label=" + JSON.stringify(window.blLabel)
                        + " taken=" + JSON.stringify(String(LiveOutputService.inputLabel)))
            selfTestBlankLiveGo.restart()
        }
    }
    Timer {
        id: selfTestBlankLiveGo
        interval: 5000
        onTriggered: {
            // Nothing staged and (in this run) no show open: GO LIVE lands on the
            // blank carrier — the state under test.
            LiveOutputService.goLive()
            console.log("[SELFTEST-BLANKLIVE] goLive live=" + LiveOutputService.live
                        + " input=" + JSON.stringify(String(LiveOutputService.inputLabel)))
            selfTestBlankLiveContent.restart()
        }
    }
    Timer {
        id: selfTestBlankLiveContent
        interval: 5000
        onTriggered: {
            // PHASE 2 — the same take with CONTENT on air. The operator's own
            // styles wear engine templates whose designs carry full-stage slabs,
            // so this is the state where a slab used to bury the feed: the slide
            // renders through the style's template, the template's background
            // must stay UNDER the take, and the outputs must not go black.
            const slide = {
                valid: true,
                title: "Blank Live Probe",
                text: "media plus content",
                background: "",
                blocks: [ { kind: "text", text: "media plus content", x: 60, y: 60,
                            width: 630, height: 300, meta: { fontSize: 48 } } ]
            }
            LiveOutputService.goLiveWithSlides("Blank Live Probe", [ slide ])
            console.log("[SELFTEST-BLANKLIVE] content live=" + LiveOutputService.live
                        + " input=" + JSON.stringify(String(LiveOutputService.inputLabel)))
            selfTestBlankLiveReport.restart()
        }
    }
    Timer {
        id: selfTestBlankLiveReport
        interval: 5000
        onTriggered: {
            // THE TILE'S OWN LAYER STACK, while the take is still held and the
            // content is on air: the design's backdrop paint must sit UNDER the
            // taken feed and the block content OVER it, which is what the
            // engine composed for this same scene (the runner checks both
            // halves, and they are the two surfaces the operator compares).
            const blTile = window.bsFindTile(SelfTest.findItem("selfTestMonitorWall"), 0)
            console.log("[SELFTEST-BLANKLIVE] tile layers: " + window.bsLayerFactsOf(blTile).line)
            console.log("[SELFTEST-BLANKLIVE] done live=" + LiveOutputService.live)
            LiveOutputService.clearInput()
            SelfTest.quit()
        }
    }
    // ---- An NDI output added WHILE LIVE (VGR_NDILATE_TEST=1) ----------------
    // User report: "the NDI source I created is not even getting any frames — as
    // if NDI is dead — until I restart the app." A row that exists at boot
    // feeds; a row ADDED during the session did not, until a relaunch — which is
    // the signature of state that is only assembled once, at GO LIVE or at boot.
    // So the scenario reproduces the state exactly: go live with one enabled NDI
    // row, let it send (the baseline), then ADD a second NDI row MID-SESSION and
    // read the feed's own per-output state (LiveOutputService.ndiOutputStates)
    // for the next twelve seconds. The verdict is the NEW row's `frames`
    // counter growing while the session is still the one it was added to
    // (tools/selftest/run_ndi_late_output_test.sh).
    //
    // Not read-only: the rows this adds are removed again before the quit, so a
    // run cannot leave the operator's roster changed.
    property string ndiLateName: "NDI Late"
    property string ndiLateBaseName: "NDI Late Base"
    property string ndiSilentName: "NDI Silent"
    property bool ndiSilentAdded: false
    property bool ndiLateAddedBase: false
    property bool ndiLateAddedRow: false
    property int ndiLateTick: 0
    function ndiLateRowOf(name) {
        for (let i = 0; i < OutputListModel.rowCount(); ++i)
            if (String(OutputListModel.getOutput(i).name) === String(name))
                return i
        return -1
    }
    // The feed's per-output state, one entry per NDI row: what the pill and the
    // Settings · Outputs row read, so it is the same number the operator sees.
    function ndiLateStates() {
        const states = LiveOutputService.ndiOutputStates || {}
        let line = ""
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const row = OutputListModel.getOutput(i)
            if (String(row.kind) !== "NDI") continue
            const st = states[row.name] || {}
            line += (line === "" ? "" : " ") + String(row.name) + "{frames="
                    + (st.frames !== undefined ? st.frames : "-")
                    + ",sending=" + st.sending + ",warming=" + st.warming + "}"
        }
        return line
    }
    function ndiLateReport(tag) {
        console.log("[SELFTEST-NDILATE] " + tag
                    + " live=" + LiveOutputService.live
                    + " rows=" + OutputListModel.rowCount()
                    + " ndiAvailable=" + EngineBridge.ndiAvailable
                    + " ndiState=" + JSON.stringify(String(EngineBridge.ndiState))
                    + " feedSending=" + LiveOutputService.ndiSending
                    + " feedName=" + JSON.stringify(String(LiveOutputService.ndiSendingOutputName))
                    + " " + window.ndiLateStates())
    }
    // Undo whatever this run added, highest index first (a removal shifts the
    // rows above it).
    function ndiLateRestore() {
        const names = []
        if (window.ndiLateAddedRow) names.push(window.ndiLateName)
        if (window.ndiLateAddedBase) names.push(window.ndiLateBaseName)
        for (const name of names) {
            const row = window.ndiLateRowOf(name)
            if (row >= 0) OutputListModel.removeOutput(row)
        }
    }
    // The user-visible half of the same scenario: an output added mid-session is
    // supposed to ANNOUNCE itself (LiveOutputService's "outputs.ndi.outputLive"
    // notice), and "I created a new output and got no notification" is the
    // report this listens for. Logged as a line so the runner can require it
    // rather than trust that the toast appeared.
    Connections {
        target: EventBus
        function onEventPosted(topic, payload) {
            if (String(topic).indexOf("outputs.ndi.") !== 0)
                return
            console.log("[SELFTEST-NDILATE] notif " + String(topic) + " | " + String(payload.message))
        }
    }
    Timer {
        id: selfTestNdiLateStart
        running: typeof SelfTestNdiLate !== "undefined" && SelfTestNdiLate
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
            // A BASELINE NDI ROW the session already feeds: without one there is
            // no "the one that works" to compare the new row against.
            let hasBase = false
            for (let i = 0; i < OutputListModel.rowCount(); ++i)
                if (String(OutputListModel.getOutput(i).kind) === "NDI")
                    hasBase = true
            if (!hasBase) {
                OutputListModel.addScreen(window.ndiLateBaseName, "NDI", "1920 × 1080", "30 Hz", "none")
                OutputListModel.setEnabled(window.ndiLateRowOf(window.ndiLateBaseName), true)
                window.ndiLateAddedBase = true
            }
            window.ndiLateReport("start")
            LiveOutputService.goLive()
            selfTestNdiLateBefore.restart()
        }
    }
    Timer {
        id: selfTestNdiLateBefore
        interval: 7000           // long enough for the baseline sender to open
        onTriggered: {
            window.ndiLateReport("before")
            // THE ACT: a second NDI output, created while the show is live —
            // exactly what the operator did when the new source stayed dark.
            OutputListModel.addScreen(window.ndiLateName, "NDI", "1920 × 1080", "30 Hz", "none")
            OutputListModel.setEnabled(window.ndiLateRowOf(window.ndiLateName), true)
            window.ndiLateAddedRow = true
            window.ndiLateReport("added")
            window.ndiLateTick = 0
            selfTestNdiLateSample.start()
        }
    }
    Timer {
        id: selfTestNdiLateSample
        interval: 1000
        repeat: true
        onTriggered: {
            ++window.ndiLateTick
            window.ndiLateReport("sample" + window.ndiLateTick)
            if (window.ndiLateTick < 12)
                return
            selfTestNdiLateSample.stop()
            // THE OTHER HALF of the same report: an output added mid-session that
            // CANNOT send. Switched off straight away, it never reaches the feed's
            // request list — before this it went dark with no explanation. The
            // feed is supposed to name the wait ("it is switched off on the
            // Outputs screen") once its grace has passed, and the runner requires
            // that notice by name.
            OutputListModel.addScreen(window.ndiSilentName, "NDI", "1920 × 1080", "30 Hz", "none")
            OutputListModel.setEnabled(window.ndiLateRowOf(window.ndiSilentName), false)
            window.ndiSilentAdded = true
            window.ndiLateReport("silent-added")
            selfTestNdiSilentDone.start()
        }
    }
    Timer {
        id: selfTestNdiSilentDone
        interval: 9000           // the feed's 6 s grace + a few polls
        onTriggered: {
            const row = window.ndiLateRowOf(window.ndiSilentName)
            if (row >= 0)
                OutputListModel.removeOutput(row)
            window.ndiSilentAdded = false
            window.ndiLateRestore()
            LiveOutputService.stop()
            window.ndiLateReport("done")
            selfTestNdiLateQuit.start()
        }
    }
    Timer {
        id: selfTestNdiLateQuit
        interval: 1500           // let the roster write settle before the quit
        onTriggered: SelfTest.quit()
    }
    // ---- Blackout, through the wall's own control (VGR_BLACKOUT_TEST=1) ------
    // "Nothing at all on screen" has to be a DELIBERATE gesture: Clear all now
    // keeps the output's own look, so the ONLY thing that paints an empty frame
    // is the wall toolbar's Blackout control (LiveOutputService::blackout — the
    // clearStage carrier). This scenario puts one real slide on air, presses
    // that control with a REAL click, presses it again to come back, and leaves
    // the engine's frame-sanity lines (black or not) as the evidence — the same
    // pixels the HDMI window and the NDI feed receive. Nothing is staged or
    // saved, so a failure here is about the control, not about a fixture.
    property bool boVisible: false
    property bool boPressed: false
    property bool boEnded: false
    property string boLabel: ""
    Timer {
        id: selfTestBlackoutStart
        running: typeof SelfTestBlackout !== "undefined" && SelfTestBlackout
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
            // A REAL FEED UNDER THE BLACKOUT. "Nothing at all on screen" has to
            // include the compositor's own layers: a taken camera rides the
            // engine scene's video layer, and before this the clear carrier
            // still composited it, so the screens showed the camera through a
            // blackout. The first camera in the roster is held for the whole
            // run (cleared before quitting) so the runner can prove the layer
            // is GONE while blacked and BACK after it ends.
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                const label = String(row && row.sublabel ? row.sublabel : "").trim()
                if (String(row.kind || "") !== "camera" || label === "")
                    continue
                window.boLabel = label
                break
            }
            if (window.boLabel !== "")
                LiveOutputService.takeInput(window.boLabel, "camera", "")
            console.log("[SELFTEST-BLACKOUT] take label=" + JSON.stringify(window.boLabel)
                        + " taken=" + JSON.stringify(String(LiveOutputService.inputLabel)))
            const slide = {
                valid: true,
                title: "Blackout Probe",
                text: "blackout probe",
                background: "",
                blocks: [ { kind: "text", text: "blackout probe", x: 60, y: 60,
                            width: 630, height: 300, meta: { fontSize: 48 } } ]
            }
            LiveOutputService.goLiveWithSlides("Blackout Probe", [ slide ])
            console.log("[SELFTEST-BLACKOUT] live live=" + LiveOutputService.live)
            selfTestBlackoutPress.restart()
        }
    }
    // A click and the state it produces are NOT the same tick: clickItem posts a
    // real mouse event, so the button's handler (and the engine's replacement,
    // which is what blackedOut reads) only runs after this timer returns. Every
    // press therefore reports from its OWN timer a couple of seconds later —
    // reading blackedOut right after the click read the state from BEFORE it and
    // made a working control look broken.
    Timer {
        id: selfTestBlackoutPress
        interval: 6000
        onTriggered: {
            // A REAL click on the toolbar control — the same path the operator's
            // hand takes, so a control that is missing, disabled or off screen
            // fails here rather than passing on a direct service call.
            window.boVisible = SelfTest.itemVisible("selfTestWallBlackout")
            SelfTest.clickItem("selfTestWallBlackout")
            selfTestBlackoutPressed.restart()
        }
    }
    Timer {
        id: selfTestBlackoutPressed
        interval: 2500
        onTriggered: {
            window.boPressed = LiveOutputService.blackedOut
            console.log("[SELFTEST-BLACKOUT] pressed visible=" + window.boVisible
                        + " blackedOut=" + window.boPressed
                        + " live=" + LiveOutputService.live)
            // The wall AS RENDERED with the blackout up — the control's lit row
            // and caption next to the tiles it just emptied (CWD is build/).
            SelfTest.grab("selfTestMonitorWall", "shot_blackout_wall.png")
            selfTestBlackoutEnd.restart()
        }
    }
    Timer {
        id: selfTestBlackoutEnd
        interval: 4000
        onTriggered: {
            // The same control, once lit: this press is the way OUT of a blackout
            // and must put the content back without restarting the session.
            SelfTest.clickItem("selfTestWallBlackout")
            selfTestBlackoutEnded.restart()
        }
    }
    Timer {
        id: selfTestBlackoutEnded
        interval: 2500
        onTriggered: {
            window.boEnded = !LiveOutputService.blackedOut
            console.log("[SELFTEST-BLACKOUT] ended blackedOut=" + LiveOutputService.blackedOut
                        + " live=" + LiveOutputService.live
                        + " blocks=" + (LiveOutputService.onAirSlide.blocks
                                         ? LiveOutputService.onAirSlide.blocks.length : -1))
            selfTestBlackoutQuit.restart()
        }
    }
    Timer {
        id: selfTestBlackoutQuit
        interval: 3500           // let the restored frames land before the last phase
        onTriggered: selfTestBlackoutStopPhase.restart()
    }
    // PHASE 3 — STOP WHILE BLACKED OUT. The carrier must not become the staged
    // pick: a blackout is a gesture over content, so STOP has to leave what the
    // blackout was HIDING on the monitor, or the next GO LIVE would put the
    // black frame straight back on the screens.
    Timer {
        id: selfTestBlackoutStopPhase
        interval: 100
        onTriggered: {
            SelfTest.clickItem("selfTestWallBlackout")
            selfTestBlackoutStop.restart()
        }
    }
    Timer {
        id: selfTestBlackoutStop
        interval: 2500
        onTriggered: {
            console.log("[SELFTEST-BLACKOUT] reblacked blackedOut="
                        + LiveOutputService.blackedOut)
            LiveOutputService.stop()
            selfTestBlackoutStopped.restart()
        }
    }
    Timer {
        id: selfTestBlackoutStopped
        interval: 2500
        onTriggered: {
            const s = LiveOutputService.stagedSlide
            console.log("[SELFTEST-BLACKOUT] stopped live=" + LiveOutputService.live
                        + " stagedBlocks=" + (s.blocks ? s.blocks.length : -1)
                        + " stagedClearStage=" + (s.clearStage === true)
                        + " blackedOut=" + LiveOutputService.blackedOut)
            // The take this run held (see the start timer) is released here, as
            // the other capture scenarios do — a selftest must not keep a
            // camera open after it quits.
            if (LiveOutputService.inputLabel !== "")
                LiveOutputService.clearInput()
            console.log("[SELFTEST-BLACKOUT] done")
            SelfTest.quit()
        }
    }
    // ---- THE LIVE VIDEO PATH, MEASURED (VGR_VIDEOPERF_TEST=1) ----------------
    // "when im showing hdmi and laptop is stressed the video lags badly".
    //
    // The scenario puts a REAL camera/HDMI input on air, goes LIVE with it, and
    // then samples — every 500 ms for ten seconds — the two numbers that ARE that
    // complaint:
    //
    //   frames   the frames the ENGINE says each output has received (the same
    //            count its Settings · Outputs row prints)
    //   ageMs    how long ago that output last presented a frame (the freshest
    //            available measure of end-to-end lag inside the app)
    //
    // plus the live loop's own throughput (framesSentNow) and the pump's frame
    // revisions (inputRev). Nothing here is a screenshot or a guess: these are
    // the counters the app already publishes per output. The shell runner
    // (tools/selftest/run_videoperf_test.sh) launches this under CPU load, so the
    // stressed machine of the report is the machine being measured.
    property string vpLabel: ""
    property int vpSamples: 0
    property double vpT0: 0
    property var vpFrames: ({})
    Timer {
        id: selfTestVideoPerfStart
        running: typeof SelfTestVideoPerf !== "undefined" && SelfTestVideoPerf
        interval: 9000           // boot + the roster, like the other scenarios
        onTriggered: {
            window.vpLabel = ""
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                const label = String(row && row.sublabel ? row.sublabel : "").trim()
                if (String(row.kind || "") !== "camera" || label === "")
                    continue
                window.vpLabel = label
                break
            }
            if (window.vpLabel !== "")
                LiveOutputService.takeInput(window.vpLabel, "camera", "")
            const slide = {
                valid: true,
                title: "Video Perf Probe",
                text: "video perf probe",
                background: "",
                blocks: [ { kind: "text", text: "video perf probe", x: 60, y: 60,
                            width: 630, height: 300, meta: { fontSize: 48 } } ]
            }
            LiveOutputService.goLiveWithSlides("Video Perf Probe", [ slide ])
            console.log("[SELFTEST-VIDEOPERF] take label=" + JSON.stringify(window.vpLabel)
                        + " taken=" + JSON.stringify(String(LiveOutputService.inputLabel))
                        + " live=" + LiveOutputService.live
                        + " outputs=" + OutputListModel.rowCount())
            window.vpSamples = 0
            window.vpFrames = ({})
            selfTestVideoPerfWarm.restart()
        }
    }
    Timer {
        id: selfTestVideoPerfWarm
        interval: 4000           // the tap opens off-thread; give it frames first
        onTriggered: {
            window.vpT0 = Date.now()
            selfTestVideoPerfSample.start()
        }
    }
    Timer {
        id: selfTestVideoPerfSample
        interval: 500
        repeat: true
        onTriggered: {
            const t = Date.now() - window.vpT0
            for (let i = 0; i < OutputListModel.rowCount(); ++i) {
                const out = OutputListModel.getOutput(i)
                const name = String(out && out.name !== undefined ? out.name : ("out" + i))
                const rt = LiveOutputService.outputRuntime[name] || null
                const frames = rt ? (Number(rt.frames) || 0) : 0
                let ageMs = -1
                if (rt && rt.ageMs !== undefined && rt.ageMs !== null)
                    ageMs = Number(rt.ageMs)
                console.log("[SELFTEST-VIDEOPERF] t=" + t + " out=" + JSON.stringify(name)
                            + " frames=" + frames
                            + " ageMs=" + (ageMs >= 0 ? Math.round(ageMs) : "-1")
                            + " enabled=" + (out && out.isEnabled ? 1 : 0))
            }
            console.log("[SELFTEST-VIDEOPERF] t=" + t + " loop=" + LiveOutputService.framesSentNow()
                        + " preview=" + LiveOutputService.previewFramesNow()
                        + " inputRev=" + LiveOutputService.inputRev
                        + " inputLive=" + LiveOutputService.inputLive)
            if (++window.vpSamples >= 20) {
                selfTestVideoPerfSample.stop()
                LiveOutputService.clearInput()
                console.log("[SELFTEST-VIDEOPERF] done samples=" + window.vpSamples)
                SelfTest.quit()
            }
        }
    }
    // ---- Program-feed rate for a held camera (VGR_INPUTRATE_TEST=1) ----------
    // "The video bus is lagging both hdmi and ndi". Both outputs render ONE
    // compositor, and a camera bus fills it from the service's pump — so the
    // number that decides whether any output can look smooth is how often that
    // pump sees a NEW frame. It used to be capped twice (the PAL tap published
    // one JPEG per 66 ms, and the pump itself ticked at 66 ms), which no
    // measurement anywhere would have told you.
    //
    // This scenario takes the first camera and reports inputRev over a fixed
    // window: the pump bumps it once per frame that actually moved, so the rate
    // is the feed's own. A 30 fps camera reads ~30 here now; the old ceiling was
    // ~15 and the number did not exist.
    property string irLabel: ""
    property real irFirst: 0
    Timer {
        id: selfTestInputRateStart
        running: typeof SelfTestInputRate !== "undefined" && SelfTestInputRate
        interval: 9000           // boot + the roster, like the other scenarios
        onTriggered: {
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                const label = String(row && row.sublabel ? row.sublabel : "").trim()
                if (String(row.kind || "") !== "camera" || label === "")
                    continue
                window.irLabel = label
                break
            }
            if (window.irLabel === "") {
                console.log("[SELFTEST-INPUTRATE] nomedia no camera in the roster")
                SelfTest.quit()
                return
            }
            LiveOutputService.takeInput(window.irLabel, "camera", "")
            console.log("[SELFTEST-INPUTRATE] take label=" + JSON.stringify(window.irLabel)
                        + " taken=" + JSON.stringify(String(LiveOutputService.inputLabel)))
            selfTestInputRateWarm.restart()
        }
    }
    Timer {
        id: selfTestInputRateWarm
        interval: 4000           // the tap opens off-thread; give it frames first
        onTriggered: {
            window.irFirst = LiveOutputService.inputRev
            selfTestInputRateWindow.restart()
        }
    }
    Timer {
        id: selfTestInputRateWindow
        interval: 5000
        onTriggered: {
            const revs = LiveOutputService.inputRev - window.irFirst
            console.log("[SELFTEST-INPUTRATE] measured label=" + JSON.stringify(window.irLabel)
                        + " revs=" + revs + " fps=" + (revs / 5).toFixed(1)
                        + " live=" + LiveOutputService.inputLive)
            LiveOutputService.clearInput()
            console.log("[SELFTEST-INPUTRATE] done")
            SelfTest.quit()
        }
    }
    Timer {
        id: selfTestStyleBgStart
        running: typeof SelfTestStyleBg !== "undefined" && SelfTestStyleBg
        interval: 12000          // boot + first QML frame, like the other scenarios
        onTriggered: {
            if (StyleListModel.rowCount() === 0)
                StyleListModel.addStyle()
            window.bsStyleRow = 0
            window.bsStyleId = String(StyleListModel.data(
                StyleListModel.index(0, 0), StyleListModel.IdRole))
            window.bsOriginalImage = String(StyleListModel.getStyle(0).backgroundImage)
            // The tile under test is the SHOW wall's first tile — the surface the
            // operator was looking at when they reported this.
            window.bsTileItem = window.bsFindTile(SelfTest.findItem("selfTestMonitorWall"), 0)
            window.bsTileIndex = (window.bsTileItem && window.bsTileItem.index !== undefined)
                ? Number(window.bsTileItem.index) : 0
            // The premise: THAT tile's output wears a style with NO background
            // image (the "selected the style when there was no bg image" state).
            const beforeStyle = OutputListModel.getOutput(window.bsTileIndex)
            window.bsOriginalStyleId = String(beforeStyle && beforeStyle.styleId !== undefined
                                              ? beforeStyle.styleId : "")
            OutputListModel.setStyle(window.bsTileIndex, window.bsStyleId)
            StyleListModel.setBackgroundImage(0, "")
            // IDENTITY PROBE: is the roster the OUTPUT MODEL resolves styles
            // through the same one the Styles screen edits? A colour written
            // through the QML singleton and read back through
            // OutputListModel's own style lookup answers it in one line. The
            // background-image path can't answer it, because a stale roster and
            // a stale file test look identical from here.
            const probeColor = "#0f0e0d"
            const wasColor = String(StyleListModel.getStyle(window.bsStyleRow).backgroundColor)
            StyleListModel.setBackgroundColor(window.bsStyleRow, probeColor)
            const seenColor = String(OutputListModel.styleBackground(window.bsTileIndex).color)
            StyleListModel.setBackgroundColor(window.bsStyleRow, wasColor)
            console.log("[SELFTEST-STYLEBG] identity probe wrote=" + probeColor
                        + " modelSeen=" + seenColor
                        + " sameRoster=" + String(seenColor === probeColor)
                        + " rowForId=" + StyleListModel.rowForId(window.bsStyleId))
            // SECOND IDENTITY PROBE — the COMPOSITION path. A monitor tile's
            // picture is composed by LiveOutputService.previewWithStyle, which
            // resolves the style itself; resolving it through the C++
            // singleton (the OTHER StyleListModel object) meant the tile kept
            // previewing the boot-time style while the engine's spec — this
            // model's adopted roster — rendered the edit on the real output.
            // Same trick as the colour probe above, one layer further out: set
            // a colour through the QML singleton, compose a synthetic slide
            // with THIS style, and read the composed background back. It can
            // only match if the composition read the roster the UI edits.
            const probePrevColor = "#123456"
            const wasPrevColor = String(StyleListModel.getStyle(window.bsStyleRow).backgroundColor)
            StyleListModel.setBackgroundColor(window.bsStyleRow, probePrevColor)
            const synthetic = {
                valid: true,
                title: "Probe 1:1",
                text: "probe",
                background: "",
                blocks: [ { kind: "text", text: "probe", x: 60, y: 60,
                            width: 600, height: 200, meta: { fontSize: 40 } } ]
            }
            let composed = {}
            try { composed = LiveOutputService.previewWithStyle(synthetic, window.bsStyleId) } catch (e) { composed = {} }
            StyleListModel.setBackgroundColor(window.bsStyleRow, wasPrevColor)
            const composedBg = String(composed.background !== undefined ? composed.background : "")
            window.bsPreviewSameRoster = (composedBg === probePrevColor)
            console.log("[SELFTEST-STYLEBG] preview composition probe wrote=" + probePrevColor
                        + " composedBg=" + JSON.stringify(composedBg)
                        + " blocks=" + String((composed.blocks || []).length)
                        + " sameRoster=" + String(window.bsPreviewSameRoster))
            // FOURTH PROBE — A STYLE THAT NAMES NO BACKGROUND, and the UNSTYLED
            // default beside it. In both, nothing names a background at all:
            // the engine's composition leaves the frame UNPAINTED (a keyable
            // overlay — SceneBuilder::ComposeSlide) instead of filling it with
            // an opaque black of its own. The preview has to compose "" here,
            // not a colour: a colour it invented is a black slab over whatever
            // the output is keyed onto, on the tile, while the real output (which
            // paints nothing) stays see-through.
            //
            // EVERY field that can name a background is cleared for the probe —
            // the style's colour and image AND its template keys. A picked
            // template DESIGN carries a background of its own (Big Bold is
            // #000000), the style adopts it, and that is the intended look —
            // which is exactly why it has to be out of the way to probe the
            // unnamed case this rule is about. All of it is restored below.
            const prevStyle = StyleListModel.getStyle(window.bsStyleRow)
            const prevFam = [ String(prevStyle.familyTemplateShows),
                              String(prevStyle.familyTemplateMedia),
                              String(prevStyle.familyTemplateScripture),
                              String(prevStyle.familyTemplateTable) ]
            const famKinds = [ "shows", "media", "scripture", "table" ]
            StyleListModel.setBackgroundColor(window.bsStyleRow, "")
            StyleListModel.setBackgroundImage(window.bsStyleRow, "")
            StyleListModel.setTemplateKey(window.bsStyleRow, "")
            for (let i = 0; i < famKinds.length; ++i)
                StyleListModel.setFamilyTemplateKey(window.bsStyleRow, famKinds[i], "")
            let unset = {}
            try { unset = LiveOutputService.previewWithStyle(synthetic, window.bsStyleId) } catch (e) { unset = {} }
            let unstyled = {}
            try { unstyled = LiveOutputService.previewWithStyle(synthetic, "") } catch (e) { unstyled = {} }
            StyleListModel.setBackgroundColor(window.bsStyleRow, String(prevStyle.backgroundColor))
            StyleListModel.setBackgroundImage(window.bsStyleRow, String(prevStyle.backgroundImage))
            StyleListModel.setTemplateKey(window.bsStyleRow, String(prevStyle.templateKey))
            for (let i = 0; i < famKinds.length; ++i)
                StyleListModel.setFamilyTemplateKey(window.bsStyleRow, famKinds[i], prevFam[i])
            const unsetBg = String(unset.background !== undefined ? unset.background : "")
            const unstyledBg = String(unstyled.background !== undefined ? unstyled.background : "")
            console.log("[SELFTEST-STYLEBG] unnamed background probe composedBg=" + JSON.stringify(unsetBg)
                        + " blocks=" + String((unset.blocks || []).length)
                        + " unpainted=" + String(unsetBg === ""))
            console.log("[SELFTEST-STYLEBG] unstyled default probe composedBg=" + JSON.stringify(unstyledBg)
                        + " blocks=" + String((unstyled.blocks || []).length)
                        + " unpainted=" + String(unstyledBg === ""))
            // FIFTH PROBE — THE STYLE'S BACKGROUND IMAGE RULE. clearBackground
            // OnText steps the style's image aside for a slide that CARRIES
            // content, and NOT for a slide that merely exists: the blank go-live
            // carrier keeps the branded backdrop, which is the whole point of
            // going live with nothing. The engine decides this in the
            // composition and hands the answer to the tile and the output
            // window (suppressStyleImage) — the two surfaces used to re-derive it
            // from `hasSlidePreview`, which is the case that lost the backdrop.
            const bareSlide = { valid: true, title: "", text: "", background: "", blocks: [] }
            const titleOnly = { valid: true, title: "Blank", text: "", background: "", blocks: [] }
            const prevClear = StyleListModel.getStyle(window.bsStyleRow).clearBackgroundOnText === true
            StyleListModel.setClearBackgroundOnText(window.bsStyleRow, true)
            const cleared = LiveOutputService.previewWithStyle(synthetic, window.bsStyleId)
            const bare = LiveOutputService.previewWithStyle(bareSlide, window.bsStyleId)
            const titleOnlyOut = LiveOutputService.previewWithStyle(titleOnly, window.bsStyleId)
            StyleListModel.setClearBackgroundOnText(window.bsStyleRow, prevClear)
            const suppressed = String(cleared.suppressStyleImage === true)
            const keptBare = String(bare.suppressStyleImage === false)
            const keptTitleOnly = String(titleOnlyOut.suppressStyleImage === false)
            console.log("[SELFTEST-STYLEBG] style image rule contentSup=" + suppressed
                        + " blankCarrierSup=" + keptBare
                        + " titleOnlySup=" + keptTitleOnly
                        + " ok=" + String(suppressed === "true" && keptBare === "true"
                                          && keptTitleOnly === "true"))
            // SIXTH PROBE — THE FRAME RULE. A camera/media/audio/image block
            // carries no pixels a block renderer can draw; the compositor's
            // distributed frame is the honest picture for it, and every other
            // kind draws true, so the BLOCKS win there. That decision is the
            // composition's (framePriority) and both preview surfaces — the
            // monitor tile and the output window — read that one answer; each
            // used to run its own copy of the kind test, and a rule with two
            // copies is a rule that drifts. Composed through the UNSTYLED
            // default so the slide's OWN blocks are the composition's source
            // whatever templates the roster's styles carry.
            const mediaSlide = {
                valid: true, title: "", text: "", background: "",
                blocks: [ { kind: "camera", text: "", x: 40, y: 40,
                            width: 300, height: 200,
                            meta: { source: "Camera 1" } } ]
            }
            let frMedia = {}
            let frText = {}
            try { frMedia = LiveOutputService.previewWithStyle(mediaSlide, "") } catch (e) { frMedia = {} }
            try { frText = LiveOutputService.previewWithStyle(synthetic, "") } catch (e) { frText = {} }
            const frMediaOn = frMedia.framePriority === true
            const frTextOn = frText.framePriority === true
            console.log("[SELFTEST-STYLEBG] frame rule mediaSlide=" + String(frMediaOn)
                        + " textSlide=" + String(frTextOn)
                        + " ok=" + String(frMediaOn === true && frTextOn === false))
            console.log("[SELFTEST-STYLEBG] setup style=" + window.bsStyleId
                        + " outputs=" + OutputListModel.rowCount()
                        + " styles=" + StyleListModel.rowCount())
            selfTestStyleBgCleared.restart()
        }
    }
    Timer {
        id: selfTestStyleBgCleared
        interval: 1200
        onTriggered: {
            window.bsReport("cleared")
            window.bsImage = window.bsFindImage()
            if (window.bsImage === "") {
                console.log("[SELFTEST-STYLEBG] no usable image file on this machine — cannot exercise the image path")
                SelfTest.quit()
                return
            }
            StyleListModel.setBackgroundImage(0, window.bsImage)
            selfTestStyleBgAdded.restart()
        }
    }
    Timer {
        id: selfTestStyleBgAdded
        interval: 1200           // let the dataChanged pulse reach the tile
        onTriggered: {
            window.bsReport("added")
            // A second beat: only a binding that needs another turn to land
            // would differ here, and "it updated a moment later" is still a
            // repaint the operator never sees as missing.
            selfTestStyleBgSettled.restart()
        }
    }
    Timer {
        id: selfTestStyleBgSettled
        interval: 1200
        onTriggered: {
            window.bsReport("settled")
            // Put the operator's style back (the run only borrowed it).
            StyleListModel.setBackgroundImage(0, window.bsOriginalImage)
            console.log("[SELFTEST-STYLEBG] restored style=" + window.bsStyleId
                        + " image=" + JSON.stringify(window.bsOriginalImage))
            // ...and the output's own style assignment, which the scenario
            // pointed at the style under test.
            OutputListModel.setStyle(window.bsTileIndex, window.bsOriginalStyleId)
            console.log("[SELFTEST-STYLEBG] restored output=" + window.bsTileIndex
                        + " style=" + JSON.stringify(window.bsOriginalStyleId))
            SelfTest.quit()
        }
    }

    // ---- header vs the caption buttons (VGR_HEADER_TEST=1) -------------------
    // The report: "the header tile is overlapping the minimize, maximize, close
    // button ... it only happens when the window scales down". Two separate
    // facts make that true, and both are measured here at THREE window sizes
    // (the launch size, the smallest the window goes, and one in between):
    //
    //   * the tab row's right-hand tiles (GO LIVE, then Settings) hold the
    //     window's own right end, in the SAME column as the caption buttons
    //     above them — that is the design (the tabs own the middle, GO LIVE owns
    //     the end). What must never happen is the two rows OVERLAPPING, or a
    //     tile drifting up into the caption row; the shared column itself is the
    //     intent, so the check is separation, not clearance;
    //   * the window's resize grips do NOT scale (they ARE the frame), so the
    //     smaller the window, the larger a share of the buttons an 8px strip
    //     covers. The symptom is exactly a resize: a SIZE cursor and a press
    //     that resizes instead of minimizing/closing.
    //
    // So the checks are: no rect intersection between a tile and the cluster
    // (measured in the HEADER's own design space, so scale cannot hide it), the
    // tiles still in the row BELOW the caption row, no grip rect touching the
    // cluster in window pixels, and — the end-to-end one — the LIVE OS cursor
    // (Win32 GetCursorInfo, the same call a user's eye makes)
    // parked 3px inside the close button's top edge, which must not be a size
    // arrow. The last step grabs the WHOLE WINDOW at the smallest size, so the
    // runner can sample the very corner the report is about.
    property var headerSizes: []
    property int headerStep: 0
    property int headerMeasured: 0
    property int headerBad: 0

    function headerRectIn(item, rootItem) {
        if (!item)
            return null
        const p0 = item.mapToItem(rootItem, 0, 0)
        const p1 = item.mapToItem(rootItem, item.width, item.height)
        return { x: Math.min(p0.x, p1.x), y: Math.min(p0.y, p1.y),
                 w: Math.abs(p1.x - p0.x), h: Math.abs(p1.y - p0.y) }
    }
    function headerOverlap(a, b) {
        return !!a && !!b && a.x < b.x + b.w && a.x + a.w > b.x
                        && a.y < b.y + b.h && a.y + a.h > b.y
    }
    // Every resize grip's rect in window pixels — the frame's Edge children.
    function headerGripRects() {
        const out = []
        const grips = SelfTest.findItem("selfTestFrameGrips")
        if (!grips || !grips.children)
            return out
        for (let i = 0; i < grips.children.length; ++i) {
            const r = window.headerRectIn(grips.children[i], null)
            if (r && r.w > 0 && r.h > 0)
                out.push(r)
        }
        return out
    }
    function headerReport(label) {
        const cluster = SelfTest.findItem("selfTestCaptionButtons")
        const goLive = SelfTest.findItem("selfTestGoLive")
        const settings = SelfTest.findItem("selfTestSettingsTile")
        // The tiles and the cluster are MEASURED IN THE HEADER'S OWN SPACE (the
        // design units their clearance is expressed in), the cluster again in
        // window pixels for the grips, which live outside the scaled header.
        const cD = window.headerRectIn(cluster, appHeader)
        const gD = window.headerRectIn(goLive, appHeader)
        const sD = window.headerRectIn(settings, appHeader)
        const cW = window.headerRectIn(cluster, null)
        ++window.headerMeasured
        if (!cD || !gD || !sD || !cW) {
            ++window.headerBad
            console.log("[SELFTEST-HEADER] " + label + " MISSING cluster=" + !!cD
                        + " goLive=" + !!gD + " settings=" + !!sD)
            return
        }
        const clearance = Math.round((cD.x - (gD.x + gD.w)) * 10) / 10
        const belowRow = gD.y >= cD.y + cD.h
        const tileOverlap = window.headerOverlap(cD, gD) || window.headerOverlap(cD, sD)
        const grips = window.headerGripRects()
        let gripHits = 0
        for (const g of grips)
            if (window.headerOverlap(g, cW))
                ++gripHits
        // The close button is the last 46 design units of the cluster, so its
        // centre scales with the app and then 3px inside its top edge is the
        // point an unscaled 8px strip reaches first.
        const probeX = cW.x + cW.w - 23 * window.uiScale
        const probeY = cW.y + 3
        const cursor = String(SelfTest.cursorAt(probeX, probeY))
        const cursorOk = cursor.indexOf("SIZE") < 0
        // `clearance` is reported for the record, not asserted: the tiles share
        // the caption column by design, so the failing cases are overlap and a
        // tile outside its own row.
        const clear = !tileOverlap && belowRow && gripHits === 0 && cursorOk
        if (!clear)
            ++window.headerBad
        console.log("[SELFTEST-HEADER] " + label
                    + " window=" + Math.round(window.width) + "x" + Math.round(window.height)
                    + " uiScale=" + window.uiScale.toFixed(3)
                    + " cluster=" + Math.round(cD.x) + "," + Math.round(cD.y)
                    + " " + Math.round(cD.w) + "x" + Math.round(cD.h)
                    + " tilesEnd=" + Math.round(gD.x + gD.w)
                    + " clearance=" + clearance
                    + " belowCaptionRow=" + belowRow
                    + " tileOverlap=" + tileOverlap
                    + " grips=" + grips.length + " gripHits=" + gripHits
                    + " clusterWin=" + Math.round(cW.x) + "," + Math.round(cW.y)
                    + "," + Math.round(cW.w) + "," + Math.round(cW.h)
                    + " probeCursor=" + cursor
                    + " clear=" + clear)
    }
    Timer {
        id: selfTestHeaderStart
        running: typeof SelfTestHeader !== "undefined" && SelfTestHeader
        interval: 6000              // boot + the first QML frame, like the other scenarios
        onTriggered: {
            SelfTest.activate()
            window.headerSizes = [
                { label: "launch",  w: Math.round(window.width), h: Math.round(window.height) },
                { label: "minimum", w: Math.round(window.minimumWidth), h: Math.round(window.minimumHeight) },
                { label: "mid",     w: Math.round((window.width + window.minimumWidth) / 2),
                                    h: Math.round((window.height + window.minimumHeight) / 2) }
            ]
            window.headerStep = 0
            selfTestHeaderStep.restart()
        }
    }
    Timer {
        id: selfTestHeaderStep
        interval: 800               // two beats per size: resize, then measure
        repeat: true
        onTriggered: {
            const sizes = window.headerSizes
            const idx = Math.floor(window.headerStep / 2)
            const phase = window.headerStep % 2
            if (idx >= sizes.length) {
                stop()
                // Leave the window at the size the report is about, then grab it.
                window.width = sizes[1].w
                window.height = sizes[1].h
                selfTestHeaderGrab.restart()
                return
            }
            const s = sizes[idx]
            if (phase === 0) {
                // The launch size is measured where it already stands (the
                // control: the size at which nothing looks wrong).
                if (idx > 0) {
                    window.width = s.w
                    window.height = s.h
                }
            } else {
                window.headerReport(s.label)
            }
            ++window.headerStep
        }
    }
    Timer {
        id: selfTestHeaderGrab
        interval: 1200              // the smallest size, laid out
        onTriggered: {
            // The whole window (an empty item name means the content item): the
            // runner samples the caption corner's own pixels out of this, so the
            // verdict does not have to trust the geometry above.
            SelfTest.grab("", "header_clear_small.png")
            console.log("[SELFTEST-HEADER] summary sizes=" + window.headerSizes.length
                        + " measured=" + window.headerMeasured + " bad=" + window.headerBad
                        + " window=" + Math.round(window.width) + "x" + Math.round(window.height))
            selfTestHeaderDone.restart()
        }
    }
    Timer {
        id: selfTestHeaderDone
        interval: 2500              // the grab saves asynchronously
        onTriggered: SelfTest.quit()
    }

    // ---- metadata palette: click and drag (VGR_METAPAL_TEST=1) ---------------
    // The palette ("> +" menu's Metadata chip) has two gestures and neither had a
    // scenario: CLICK a field to add a text item carrying its placeholder, or DRAG
    // one onto the canvas to land it where the pointer is. The boot check loads
    // the whole tree, but nothing drove the gesture.
    //
    // This drives the REAL path with REAL input: the + chip, the Metadata chip in
    // the + menu (a zone-hover chip, so the hover move is part of the gesture),
    // then a press/move/release drag from a field chip onto the canvas. Both
    // fields come from MetadataFields.all rather than being spelled here, so the
    // scenario follows the registry the palette draws from — a field added there
    // is covered without touching this block.
    property var metaAll: []
    property string metaClickToken: ""
    property string metaDragToken: ""
    property string metaClickKey: ""
    property string metaDragKey: ""
    property var metaKeysBefore: []
    property var metaDropAt: ({ x: 0, y: 0 })
    property var metaDragTo: ({ x: 0, y: 0 })
    // The popup half (see selfTestMetaPopup): the panel must read as lifted
    // ABOVE the canvas — no I-beam of the text item beneath it showing through,
    // the chips' own pointing hand winning — and a click outside the box must
    // close it.
    property var metaUnderAt: ({ x: 0, y: 0 })
    property var metaPadAt: ({ x: 0, y: 0 })
    property string metaChipCursor: ""
    property string metaPanelCursor: ""
    property bool metaBlocked: false
    property bool metaClosedAfterOutside: false
    property bool metaPopupOk: false
    property bool metaDismissOk: false
    // Where the outside click landed (window coords), so a run that says the
    // panel stayed open can be told apart from one whose click missed the box.
    property var metaOutsideAt: ({ x: 0, y: 0 })

    function metaChipName(token) { return "selfTestMetaChip_" + String(token).replace(/[{}]/g, "") }
    // Every canvas item delegate that exists right now, by the engine's key. The
    // delegate is what the user SEES, so an item that only exists in the model
    // does not count as added.
    function metaCanvasKeys() {
        const out = []
        const walk = (item) => {
            if (!item)
                return
            const n = String(item.objectName || "")
            if (n.indexOf("canvasItem_") === 0)
                out.push(n.substring("canvasItem_".length))
            const kids = item.children
            if (kids)
                for (let i = 0; i < kids.length; ++i)
                    walk(kids[i])
        }
        walk(SelfTest.findItem("selfTestCanvas"))
        return out
    }
    // `exclude` is the item an EARLIER gesture in this run already added. The
    // canvas keys come back in delegate order and `metaKeysBefore` was taken
    // before the first gesture, so without it the second gesture reports the
    // FIRST gesture's item again — the drag half was reading the click's box and
    // failing `textOk`/`landed` on its own bookkeeping (both reported keys were
    // "item-1"), which says nothing about the drop.
    function metaNewKey(exclude) {
        const skip = exclude || []
        const now = window.metaCanvasKeys()
        for (const k of now)
            if (window.metaKeysBefore.indexOf(k) < 0 && skip.indexOf(k) < 0)
                return k
        return ""
    }
    // The new item's own geometry and text, from the live delegate (which binds
    // x/y/width/height one-way to the item — DraggableCanvasText's note).
    function metaFacts(key) {
        const del = key === "" ? null : SelfTest.findItem("canvasItem_" + key)
        if (!del)
            return null
        const d = del.modelData
        if (!d)
            return null
        // The STYLE is part of the answer: a palette-inserted field is built to
        // its token (EditScreen.addMetadataItem), so the scenario reports the
        // box and the font it landed with, not just its text and position.
        return { text: String(d.text), x: Number(d.x), y: Number(d.y),
                 w: Number(d.width), h: Number(d.height),
                 font: Number(d.meta && d.meta.fontSize !== undefined ? d.meta.fontSize : 0),
                 autoSize: String(d.meta && d.meta.autoSize ? d.meta.autoSize : ""),
                 bind: String(d.bind !== undefined && d.bind !== null ? d.bind : "") }
    }
    Timer {
        id: selfTestMetaStart
        running: typeof SelfTestMetaPalette !== "undefined" && SelfTestMetaPalette
        interval: 9000              // boot, then a show to put on the canvas
        onTriggered: {
            SelfTest.activate()
            window.currentView = "edit"
            editScreen.newShow()
            selfTestMetaAddMenu.restart()
        }
    }
    Timer {
        id: selfTestMetaAddMenu
        interval: 2500              // the edit screen and its canvas
        onTriggered: {
            window.metaAll = MetadataFields.all
            if (window.metaAll.length < 2 || !SelfTest.findItem("selfTestCanvas")) {
                console.log("[SELFTEST-METAPAL] cannot run fields=" + window.metaAll.length
                            + " canvas=" + (SelfTest.findItem("selfTestCanvas") ? "yes" : "no"))
                SelfTest.quit()
                return
            }
            window.metaClickToken = String(window.metaAll[0].token)
            window.metaDragToken = String(window.metaAll[window.metaAll.length - 1].token)
            SelfTest.clickItem("selfTestAddChip")
            selfTestMetaMenuOpen.restart()
        }
    }
    Timer {
        id: selfTestMetaMenuOpen
        interval: 1200              // the + menu's open
        onTriggered: {
            if (!editScreen.addMenuOpen || !SelfTest.findItem("selfTestAddKindChip_metadata")) {
                console.log("[SELFTEST-METAPAL] the + menu did not open (open=" + editScreen.addMenuOpen
                            + ")")
                SelfTest.quit()
                return
            }
            // The + menu's chips are ONE zone-hover area whose zone is the
            // pointer's column, so this click has to arrive with the hover set —
            // clickItem moves the real cursor there first.
            SelfTest.clickItem("selfTestAddKindChip_metadata")
            selfTestMetaPaletteOpen.restart()
        }
    }
    Timer {
        id: selfTestMetaPaletteOpen
        interval: 1200
        onTriggered: {
            const pal = SelfTest.findItem("selfTestMetadataPalette")
            const open = editScreen.metadataPaletteOpen
            const clickChip = SelfTest.findItem(window.metaChipName(window.metaClickToken))
            const dragChip = SelfTest.findItem(window.metaChipName(window.metaDragToken))
            console.log("[SELFTEST-METAPAL] palette open=" + open
                        + " item=" + (pal ? "yes" : "no")
                        + " visible=" + (pal ? pal.visible : "?")
                        + " fields=" + window.metaAll.length
                        + " clickToken=" + window.metaClickToken
                        + " dragToken=" + window.metaDragToken)
            if (!open || !pal || !pal.visible || !clickChip || !dragChip) {
                console.log("[SELFTEST-METAPAL] palette unusable open=" + open
                            + " clickChip=" + (clickChip ? "yes" : "no")
                            + " dragChip=" + (dragChip ? "yes" : "no"))
                SelfTest.quit()
                return
            }
            // Hover it first: the palette is meant to explain itself, so the
            // field under the pointer publishes its own sentence.
            const c = SelfTest.itemCenter(window.metaChipName(window.metaClickToken))
            SelfTest.move(c.x, c.y)
            selfTestMetaHint.restart()
        }
    }
    Timer {
        id: selfTestMetaHint
        interval: 700
        onTriggered: {
            const want = String(window.metaAll[0].hint)
            const got = String(editScreen.metadataHint)
            console.log("[SELFTEST-METAPAL] hint got=" + JSON.stringify(got)
                        + " want=" + JSON.stringify(want) + " hintOk=" + (got === want))
            window.metaKeysBefore = window.metaCanvasKeys()
            SelfTest.clickItem(window.metaChipName(window.metaClickToken))
            selfTestMetaClick.restart()
        }
    }
    Timer {
        id: selfTestMetaClick
        interval: 1000
        onTriggered: {
            const key = window.metaNewKey()
            const f = window.metaFacts(key)
            window.metaClickKey = key
            console.log("[SELFTEST-METAPAL] click token=" + window.metaClickToken
                        + " newKey=" + JSON.stringify(key)
                        + " delegate=" + (f ? "yes" : "no")
                        + " text=" + JSON.stringify(f ? f.text : "")
                        + " at=" + (f ? Math.round(f.x) + "," + Math.round(f.y) : "?")
                        + " textOk=" + (!!f && f.text === window.metaClickToken)
                        + " box=" + (f ? Math.round(f.w) + "x" + Math.round(f.h) : "?")
                        + " font=" + (f ? String(f.font) : "?")
                        + " autoSize=" + JSON.stringify(f ? f.autoSize : "")
                        + " styledOk=" + (!!f && f.font === 40 && f.autoSize === "shrinkToFit")
                        // The SLOT the field stands for, against the registry's
                        // own answer for that token — the insert must carry it
                        // or the engine cannot bind the block to a slide.
                        + " bind=" + JSON.stringify(f ? f.bind : "")
                        + " wantBind=" + JSON.stringify(String(window.metaAll[0].bind === undefined ? "" : window.metaAll[0].bind))
                        + " bindOk=" + (!!f && f.bind === String(window.metaAll[0].bind === undefined ? "" : window.metaAll[0].bind)))
            if (!f) {
                SelfTest.quit()
                return
            }
            // Now the DRAG half: press the other field's chip, cross onto the
            // canvas, release. The drop must land the item under the pointer, so
            // the check is the distance between the drop point and the item's
            // own centre — a couple of units of ghost-hotspot offset allowed.
            const canvas = SelfTest.findItem("selfTestCanvas")
            window.metaDropAt = { x: Math.round(canvas.width * 0.62),
                                  y: Math.round(canvas.height * 0.30) }
            const target = canvas.mapToItem(null, window.metaDropAt.x, window.metaDropAt.y)
            window.metaDragTo = { x: target.x, y: target.y }
            const chip = SelfTest.itemCenter(window.metaChipName(window.metaDragToken))
            SelfTest.move(8, 8)     // no WM_MOUSEMOVE fires if the cursor is already there
            SelfTest.press(chip.x, chip.y)
            SelfTest.move((chip.x + target.x) / 2, (chip.y + target.y) / 2)
            SelfTest.move(target.x, target.y)
            selfTestMetaDrop.restart()
        }
    }
    Timer {
        id: selfTestMetaDrop
        interval: 500               // a beat for the drag layer to carry it across
        onTriggered: {
            SelfTest.release(window.metaDragTo.x, window.metaDragTo.y)
            selfTestMetaRead.restart()
        }
    }
    Timer {
        id: selfTestMetaRead
        interval: 1000
        onTriggered: {
            const key = window.metaNewKey(window.metaClickKey ? [window.metaClickKey] : [])
            const f = window.metaFacts(key)
            window.metaDragKey = key
            let dist = -1
            if (f) {
                const cx = f.x + f.w / 2
                const cy = f.y + f.h / 2
                dist = Math.round(Math.sqrt(Math.pow(cx - window.metaDropAt.x, 2)
                                            + Math.pow(cy - window.metaDropAt.y, 2)))
            }
            console.log("[SELFTEST-METAPAL] drag token=" + window.metaDragToken
                        + " newKey=" + JSON.stringify(key)
                        + " delegate=" + (f ? "yes" : "no")
                        + " text=" + JSON.stringify(f ? f.text : "")
                        + " center=" + (f ? Math.round(f.x + f.w / 2) + "," + Math.round(f.y + f.h / 2) : "?")
                        + " dropAt=" + window.metaDropAt.x + "," + window.metaDropAt.y
                        + " distance=" + dist
                        + " textOk=" + (!!f && f.text === window.metaDragToken)
                        + " landed=" + (dist >= 0 && dist <= 60)
                        // A DROP is the same insert as a click, so it must land
                        // the same style and the same registry slot — the two
                        // are built by one function and this is what says so.
                        + " box=" + (f ? Math.round(f.w) + "x" + Math.round(f.h) : "?")
                        + " font=" + (f ? String(f.font) : "?")
                        + " autoSize=" + JSON.stringify(f ? f.autoSize : "")
                        + " bind=" + JSON.stringify(f ? f.bind : "")
                        + " wantBind=" + JSON.stringify(String(window.metaAll[window.metaAll.length - 1].bind === undefined ? "" : window.metaAll[window.metaAll.length - 1].bind))
                        + " bindOk=" + (!!f && f.bind === String(window.metaAll[window.metaAll.length - 1].bind === undefined ? "" : window.metaAll[window.metaAll.length - 1].bind)))
            // The POPUP half needs a TEXT item UNDER the panel, or the cursor
            // check proves nothing: no item beneath it means no I-beam to shine
            // through in the first place. The item the click half added is the
            // one to put there (this is a show the scenario made, never the
            // user's), parked in the panel's lower-left padding — under the
            // panel, on its padding rather than on a chip, so the expected
            // cursor there is the panel's own arrow.
            const canvas = SelfTest.findItem("selfTestCanvas")
            const palette = SelfTest.findItem("selfTestMetadataPalette")
            const obj = editScreen.canvasObjectByKey(window.metaClickKey)
            if (canvas && palette && obj) {
                const p = palette.mapToItem(canvas, 0, 0)
                obj.x = Math.round(p.x + 24)
                obj.y = Math.round(p.y + palette.height - 46)
                window.metaUnderAt = palette.mapToItem(null, 24, palette.height - 34)
                window.metaPadAt = { x: Math.round(window.metaUnderAt.x),
                                     y: Math.round(window.metaUnderAt.y) }
            } else {
                console.log("[SELFTEST-METAPAL] popup canvas=" + (canvas ? "yes" : "no")
                            + " palette=" + (palette ? "yes" : "no")
                            + " item=" + (obj ? "yes" : "no"))
            }
            selfTestMetaPopup.restart()
        }
    }
    Timer {
        id: selfTestMetaPopup
        interval: 900               // the moved item's own delegate, re-laid out
        onTriggered: {
            const pal = SelfTest.findItem("selfTestMetadataPalette")
            const del = SelfTest.findItem("canvasItem_" + window.metaClickKey)
            // 1. On a CHIP: the field's own pointing hand must win (the canvas'
            //    I-beam used to shadow it app-wide — the report's own symptom).
            const chip = SelfTest.itemCenter(window.metaChipName(window.metaDragToken))
            window.metaChipCursor = SelfTest.cursorAt(chip.x, chip.y)
            // 2. On the panel's PADDING, over the text item beneath it: the
            //    panel's arrow — never the I-beam the item below claims.
            window.metaPanelCursor = SelfTest.cursorAt(window.metaPadAt.x, window.metaPadAt.y)
            // 3. The mechanism behind both, measured directly: while the panel
            //    is open over it, the item below reads BLOCKED (its cursor
            //    claims are dropped and its hover areas go quiet).
            window.metaBlocked = del ? AppCursor.blocked(del) : false
            const chipOk = window.metaChipCursor === "HAND"
            const padOk = window.metaPanelCursor !== "IBEAM"
            window.metaPopupOk = chipOk && padOk && window.metaBlocked
            selfTestMetaOutside.restart()
        }
    }
    // The item that was parked UNDER the panel for check 2 is no longer usable
    // for the outside click: the panel's padding is INSIDE its own rect, so a
    // click there is a click on the panel's surface and cannot close anything.
    // Move it clear of the panel first — the property the check is about is kept
    // (the click still lands on a CANVAS OBJECT, the swallowing MouseArea, not on
    // bare canvas), only the geometry is honest now.
    Timer {
        id: selfTestMetaOutside
        interval: 700               // the moved item's delegate, re-laid out clear of the panel
        onTriggered: {
            const canvas = SelfTest.findItem("selfTestCanvas")
            const palette = SelfTest.findItem("selfTestMetadataPalette")
            const obj = editScreen.canvasObjectByKey(window.metaClickKey)
            if (canvas && palette && obj) {
                const p = palette.mapToItem(canvas, 0, 0)
                const spots = [ { x: canvas.width * 0.10, y: canvas.height * 0.90 },
                                { x: canvas.width * 0.74, y: canvas.height * 0.90 },
                                { x: canvas.width * 0.10, y: canvas.height * 0.04 } ]
                let pick = spots[0]
                for (let i = 0; i < spots.length; ++i) {
                    const cx = spots[i].x + obj.width / 2
                    const cy = spots[i].y + obj.height / 2
                    const inPanel = cx >= p.x && cx <= p.x + palette.width
                                    && cy >= p.y && cy <= p.y + palette.height
                    if (!inPanel) { pick = spots[i]; break }
                }
                obj.x = Math.round(pick.x)
                obj.y = Math.round(pick.y)
                window.metaOutsideAt = { x: Math.round(obj.x + obj.width / 2),
                                         y: Math.round(obj.y + obj.height / 2) }
            }
            selfTestMetaDismiss.restart()
        }
    }
    Timer {
        id: selfTestMetaDismiss
        interval: 700               // the item's delegate at its new home
        onTriggered: {
            // 4. And a click OUTSIDE the box closes it. The click lands on the
            //    canvas item above — one that used to swallow the press in its
            //    own MouseArea, which is why "click outside its box" left the
            //    panel standing; a click on the canvas BACKGROUND always closed
            //    it and proves nothing.
            SelfTest.clickItem("canvasItem_" + window.metaClickKey)
            window.metaClosedAfterOutside = !editScreen.metadataPaletteOpen
            window.metaDismissOk = window.metaClosedAfterOutside
            console.log("[SELFTEST-METAPAL] popup chipCursor=" + window.metaChipCursor
                        + " padCursor=" + window.metaPanelCursor
                        + " blocked=" + window.metaBlocked
                        + " outsideAt=" + window.metaOutsideAt.x + "," + window.metaOutsideAt.y
                        + " closedAfterOutsideClick=" + window.metaClosedAfterOutside
                        + " panelOpen=" + editScreen.metadataPaletteOpen
                        + " cursorOk=" + window.metaPopupOk
                        + " dismissOk=" + window.metaDismissOk)
            console.log("[SELFTEST-METAPAL] summary clickAdded=" + (window.metaClickKey !== "")
                        + " dragAdded=" + (window.metaDragKey !== "")
                        + " cursorOk=" + window.metaPopupOk
                        + " dismissOk=" + window.metaDismissOk)
            SelfTest.activate()
            SelfTest.grab("selfTestMetadataPalette", "meta_palette.png")
            selfTestMetaDone.restart()
        }
    }
    Timer {
        id: selfTestMetaDone
        interval: 2500              // the grab saves asynchronously
        onTriggered: SelfTest.quit()
    }

    // ---- item colour/opacity reaches the canvas (VGR_ITEMCOLOR_TEST=1) -------
    // The report: "the opacity in the colour picker shows well but it never
    // applies to the shape in the edit canvas — the text item does apply it".
    // Both kinds go through the SAME BackgroundColorModal, so if one works and
    // the other does not, the difference is either in the apply or in what the
    // canvas reads back — and reading the code cannot tell the two apart.
    //
    // THREE WITNESSES PER ITEM, because the first two can agree while the screen
    // still shows nothing:
    //   * the ITEM's own value (style.backgroundColor / meta.color),
    //   * its DELEGATE's own read (DraggableCanvasText.styleBackgroundColor),
    //   * the PIXELS that delegate actually paints — grabbed to a PNG and
    //     sampled by the runner (run_item_color_test.sh).
    //
    // Items are resolved by KEY on every read (editScreen.canvasObjectByKey),
    // never from the object addCanvasItem handed back: the engine owns item
    // identity and a document round-trip can replace the object, after which a
    // captured reference reads as null — a read that an earlier version of this
    // scenario turned into a PASS (alpha -1 < 0.9 counted as "faded").
    //
    // A RECTANGLE takes the delegate's own fill box; a CIRCLE takes the OTHER
    // branch (drawsOwnBody — EditScreen's shapeContent paints the body from the
    // same style), so both render paths are measured. The text item's colour is
    // applied in the same run as the known-good comparison.
    //
    // Every apply is WHITE at 100% first (a colour no style in this app carries,
    // so a white pixel in the grab is unambiguous), then WHITE at 30% (alpha).
    property string itemColorRectKey: ""
    property string itemColorCircleKey: ""
    property string itemColorTextKey: ""
    property string itemColorRectBefore: ""
    property string itemColorRectSolidItem: ""
    property string itemColorRectSolidDelegate: ""
    property string itemColorRectFadedItem: ""
    property string itemColorRectFadedDelegate: ""
    property string itemColorCircleBefore: ""
    property string itemColorCircleSolidItem: ""
    property string itemColorCircleSolidDelegate: ""
    property string itemColorCircleFadedItem: ""
    property string itemColorCircleFadedDelegate: ""
    property string itemColorTextBefore: ""
    property string itemColorTextAfter: ""
    property int itemColorStep: 0
    property string itemColorCycleFirst: ""
    property string itemColorCycleSecond: ""

    // The colour every apply uses.
    readonly property string itemColorPaint: "#ffffff"

    // -1 for anything that is not a colour at all, so a MISSING value is a
    // number in the report instead of an exception that ends the run — and the
    // runner treats -1 as "missing", never as "faded".
    function itemColorAlpha(hex) {
        const s = String(hex)
        if (s.length === 0 || s.charAt(0) !== "#")
            return -1
        return Qt.color(s).a
    }
    function itemColorHex(v) {
        if (v === undefined || v === null)
            return ""
        return String(v)
    }
    function itemColorItemOf(key) {
        return editScreen.canvasObjectByKey(key)
    }
    // The LIVE item's fill, or "" when the item is gone (the runner reports that
    // as FAIL item, not as a pass).
    function itemColorItemHex(key) {
        const it = window.itemColorItemOf(key)
        return it && it.style ? window.itemColorHex(it.style.backgroundColor) : ""
    }
    // What the delegate the operator looks at is drawing right now.
    function itemColorDelegateHex(key) {
        const d = SelfTest.findItem("canvasItem_" + key)
        if (!d)
            return "(no delegate)"
        return window.itemColorHex(d.styleBackgroundColor)
    }
    // Selects the item, then opens the picker through the ROW'S OWN expression
    // (EditScreen's Background row: "itemBackground" when something is
    // selected, the slide background otherwise) rather than a target this
    // scenario hardcodes. A hardcoded target would keep passing while the row
    // itself decided otherwise for a selected shape — which is exactly the
    // reported "the shape's colour goes somewhere else".
    function itemColorOpenFor(key, target) {
        editScreen.handleCanvasSelect(key, 0)
        const rowTarget = editScreen.primarySelectedItemStyle ? "itemBackground" : "background"
        console.log("[SELFTEST-ITEMCOLOR] row key=" + key
                    + " selected=" + JSON.stringify(editScreen.selectedCanvasObjects)
                    + " styleNull=" + (editScreen.primarySelectedItemStyle === null)
                    + " primaryKind=" + (editScreen.primarySelectedItem ? editScreen.primarySelectedItem.kind : "(none)")
                    + " rowTarget=" + rowTarget
                    + " hint=" + target)
        editScreen.openBackgroundPicker(rowTarget)
        editScreen.selectedCanvasObjects = []
    }
    // The picker driven the way the operator drives it: a typed CUSTOM colour
    // (liveKind "color" is what typing sets) plus the Opacity slider, then the
    // SAME accept() the Save button calls.
    function itemColorApply(opacityPct) {
        const picker = SelfTest.findItem("selfTestBgPicker")
        if (!picker || !picker.open) {
            console.log("[SELFTEST-ITEMCOLOR] no open picker to apply with (open="
                        + (picker ? picker.open : "no instance") + ")")
            return false
        }
        console.log("[SELFTEST-ITEMCOLOR] apply title=" + picker.title + " target=" + editScreen.bgModalTarget)
        picker.customHex = window.itemColorPaint
        picker.liveKind = "color"
        picker.opacityPct = opacityPct
        try {
            picker.accept()
        } catch (e) {
            console.log("[SELFTEST-ITEMCOLOR] save threw: " + e)
            return false
        }
        return true
    }
    function itemColorRead(key) {
        return { item: window.itemColorItemHex(key),
                 delegate: window.itemColorDelegateHex(key) }
    }
    // WHICH ITEM ANSWERS A CLICK AT A POINT TWO ITEMS COVER — the reported
    // "you pick the text item instead of the shape". clickItem delivers a real
    // OS click at the named delegate's own centre, so this is the operator's
    // gesture, not a selection the scenario made up from the model.
    // The CLICK-THROUGH probe: two taps on ONE point that two items cover.
    // `key` is what Qt hands the topmost item's handler, which is what the
    // operator's click reports; the second tap must reach the item UNDER it.
    function itemColorCycleClick(label, key) {
        editScreen.handleCanvasSelect(key, 0, Qt.point(100, 90))
        const sel = editScreen.selectedCanvasObjects.slice()
        console.log("[SELFTEST-ITEMCOLOR] cycle " + label + " key=" + key
                    + " selected=" + JSON.stringify(sel))
        return sel.length > 0 ? sel[0] : ""
    }
    function itemColorClickProbe(label, objectName) {
        SelfTest.clickItem(objectName)
        const sel = editScreen.selectedCanvasObjects.slice()
        const kinds = sel.map((k) => {
            const it = window.itemColorItemOf(k)
            return it ? (it.kind + (it.meta && it.meta.shapeType ? "/" + it.meta.shapeType : "")) : "?"
        })
        console.log("[SELFTEST-ITEMCOLOR] click " + label + " -> selected=" + JSON.stringify(sel)
                    + " kinds=" + JSON.stringify(kinds))
        return sel
    }
    // Opens the picker with an EXPLICIT target — the Text panel's own colour
    // row (TextItemPanel -> openBackgroundPicker("textColor")), a different door
    // from the Items tab's Background row.
    function itemColorOpenWith(key, target) {
        editScreen.handleCanvasSelect(key, 0)
        editScreen.openBackgroundPicker(target)
        editScreen.selectedCanvasObjects = []
    }

    Timer {
        id: selfTestItemColorStart
        running: typeof SelfTestItemColor !== "undefined" && SelfTestItemColor
        interval: 9000              // boot, then a show to put on the canvas
        onTriggered: {
            SelfTest.activate()
            window.currentView = "edit"
            editScreen.newShow()
            window.itemColorStep = 0
            selfTestItemColorTick.restart()
        }
    }
    // One step per tick: a picker apply and the read that judges it must not
    // share a tick (the dialog's own signals and the delegate's relayout land
    // between them, the way they do for a human).
    Timer {
        id: selfTestItemColorTick
        interval: 700
        repeat: true
        onTriggered: {
            const step = window.itemColorStep++
            if (step === 0) {
                // RECTANGLE — the delegate's own fill box.
                const shape = editScreen.addCanvasItem("shape")
                if (!shape) {
                    console.log("[SELFTEST-ITEMCOLOR] no shape item — the scenario cannot run")
                    SelfTest.quit()
                    return
                }
                shape.meta = Object.assign({}, shape.meta, { shapeType: "rectangle" })
                shape.style.backgroundColor = Theme.controlBg
                window.itemColorRectKey = shape.key
                window.itemColorRectBefore = window.itemColorItemHex(shape.key)
                SelfTest.grab("canvasItem_" + shape.key, "itemcolor_rect_before.png")
                window.itemColorOpenFor(shape.key, "itemBackground")
            } else if (step === 1) {
                window.itemColorApply(100)
            } else if (step === 2) {
                const r = window.itemColorRead(window.itemColorRectKey)
                window.itemColorRectSolidItem = r.item
                window.itemColorRectSolidDelegate = r.delegate
                console.log("[SELFTEST-ITEMCOLOR] rect solid key=" + window.itemColorRectKey
                            + " before=" + window.itemColorRectBefore
                            + " item=" + window.itemColorRectSolidItem
                            + " itemA=" + window.itemColorAlpha(window.itemColorRectSolidItem).toFixed(3)
                            + " delegate=" + window.itemColorRectSolidDelegate
                            + " delegateA=" + window.itemColorAlpha(window.itemColorRectSolidDelegate).toFixed(3))
                SelfTest.grab("canvasItem_" + window.itemColorRectKey, "itemcolor_rect_solid.png")
                window.itemColorOpenFor(window.itemColorRectKey, "itemBackground")
            } else if (step === 3) {
                window.itemColorApply(30)
            } else if (step === 4) {
                const r = window.itemColorRead(window.itemColorRectKey)
                window.itemColorRectFadedItem = r.item
                window.itemColorRectFadedDelegate = r.delegate
                console.log("[SELFTEST-ITEMCOLOR] rect faded item=" + window.itemColorRectFadedItem
                            + " itemA=" + window.itemColorAlpha(window.itemColorRectFadedItem).toFixed(3)
                            + " delegate=" + window.itemColorRectFadedDelegate
                            + " delegateA=" + window.itemColorAlpha(window.itemColorRectFadedDelegate).toFixed(3))
                // CIRCLE — the drawsOwnBody branch (shapeContent paints it).
                const circle = editScreen.addCanvasItem("shape")
                if (circle) {
                    circle.meta = Object.assign({}, circle.meta, { shapeType: "circle" })
                    circle.style.backgroundColor = Theme.controlBg
                    window.itemColorCircleKey = circle.key
                    window.itemColorCircleBefore = window.itemColorItemHex(circle.key)
                    SelfTest.grab("canvasItem_" + circle.key, "itemcolor_circle_before.png")
                    window.itemColorOpenFor(circle.key, "itemBackground")
                } else {
                    console.log("[SELFTEST-ITEMCOLOR] no circle item — the body branch is unmeasured")
                    window.itemColorCircleKey = ""
                }
            } else if (step === 5) {
                if (window.itemColorCircleKey !== "")
                    window.itemColorApply(100)
            } else if (step === 6) {
                const r = window.itemColorRead(window.itemColorCircleKey)
                window.itemColorCircleSolidItem = r.item
                window.itemColorCircleSolidDelegate = r.delegate
                console.log("[SELFTEST-ITEMCOLOR] circle solid key=" + window.itemColorCircleKey
                            + " before=" + window.itemColorCircleBefore
                            + " item=" + window.itemColorCircleSolidItem
                            + " itemA=" + window.itemColorAlpha(window.itemColorCircleSolidItem).toFixed(3)
                            + " delegate=" + window.itemColorCircleSolidDelegate
                            + " delegateA=" + window.itemColorAlpha(window.itemColorCircleSolidDelegate).toFixed(3))
                if (window.itemColorCircleKey !== "") {
                    SelfTest.grab("canvasItem_" + window.itemColorCircleKey, "itemcolor_circle_solid.png")
                    window.itemColorOpenFor(window.itemColorCircleKey, "itemBackground")
                }
            } else if (step === 7) {
                if (window.itemColorCircleKey !== "")
                    window.itemColorApply(30)
            } else if (step === 8) {
                const r = window.itemColorRead(window.itemColorCircleKey)
                window.itemColorCircleFadedItem = r.item
                window.itemColorCircleFadedDelegate = r.delegate
                console.log("[SELFTEST-ITEMCOLOR] circle faded item=" + window.itemColorCircleFadedItem
                            + " itemA=" + window.itemColorAlpha(window.itemColorCircleFadedItem).toFixed(3)
                            + " delegate=" + window.itemColorCircleFadedDelegate
                            + " delegateA=" + window.itemColorAlpha(window.itemColorCircleFadedDelegate).toFixed(3))
                // THE KNOWN-GOOD PATH, in the same run: a text item's own colour.
                const text = editScreen.addCanvasItem("text")
                if (text) {
                    window.itemColorTextKey = text.key
                    window.itemColorTextBefore = window.itemColorHex(text.meta ? text.meta.color : "")
                    window.itemColorOpenWith(text.key, "textColor")
                } else {
                    window.itemColorTextKey = ""
                    window.itemColorTextBefore = "(no text item)"
                }
            } else if (step === 9) {
                window.itemColorApply(100)
            } else if (step === 10) {
                // Read back by KEY, like every other item in this scenario.
                const text = window.itemColorTextKey !== ""
                    ? window.itemColorItemOf(window.itemColorTextKey) : null
                window.itemColorTextAfter = text && text.meta
                    ? window.itemColorHex(text.meta.color) : ""
                console.log("[SELFTEST-ITEMCOLOR] text before=" + window.itemColorTextBefore
                            + " after=" + window.itemColorTextAfter
                            + " a=" + window.itemColorAlpha(window.itemColorTextAfter).toFixed(3))
            } else if (step === 11) {
                window.itemColorClickProbe("circle-under-text", "canvasItem_" + window.itemColorCircleKey)
            } else if (step === 12) {
                window.itemColorClickProbe("text", "canvasItem_" + window.itemColorTextKey)
            } else if (step === 13) {
                // THE CLICK-THROUGH. (100,90) lies inside the CIRCLE's box AND
                // inside the text item's box above it, so Qt delivers the tap to
                // the text item — the reported "you pick the text item instead of
                // the shape". One tick = 700 ms, inside the 350-900 ms window
                // that separates a deliberate second click from a double-click.
                window.itemColorCycleFirst = window.itemColorCycleClick("click1", window.itemColorTextKey)
            } else if (step === 14) {
                window.itemColorCycleSecond = window.itemColorCycleClick("click2", window.itemColorTextKey)
            } else if (step === 15) {
                console.log("[SELFTEST-ITEMCOLOR] cycle summary first=" + window.itemColorCycleFirst
                            + " second=" + window.itemColorCycleSecond)
                selfTestItemColorTick.stop()
                selfTestItemColorDone.start()
            }
        }
    }
    Timer {
        id: selfTestItemColorDone
        interval: 1500              // the grabs save asynchronously
        onTriggered: {
            // The property half of the verdict; the runner adds the pixel half.
            const ok = (v) => window.itemColorAlpha(v) >= 0.9
            const faded = (v) => window.itemColorAlpha(v) > 0 && window.itemColorAlpha(v) < 0.4
            const rectOk = ok(window.itemColorRectSolidItem) && ok(window.itemColorRectSolidDelegate)
                && faded(window.itemColorRectFadedItem) && faded(window.itemColorRectFadedDelegate)
            const circleOk = window.itemColorCircleKey !== ""
                && ok(window.itemColorCircleSolidItem) && ok(window.itemColorCircleSolidDelegate)
                && faded(window.itemColorCircleFadedItem) && faded(window.itemColorCircleFadedDelegate)
            // The click-through: the second tap at the shared point must land
            // on the SHAPE, not on the text item that answered the first one.
            const cycleOk = window.itemColorCycleSecond === window.itemColorCircleKey
            console.log("[SELFTEST-ITEMCOLOR] summary rectPropsOk=" + rectOk
                        + " circlePropsOk=" + circleOk
                        + " cycleOk=" + cycleOk)
            console.log("[SELFTEST-ITEMCOLOR] verdict=" + (rectOk && circleOk && cycleOk
                        ? "PROPS-OK" : "NOT-APPLIED"))
            SelfTest.quit()
        }
    }


    // ---- The sermon-folder sweep's OWN notice, read off the screen ----
    // "Add sermons folder" reports what it imported and, for every file it would
    // not take, WHY — the wording the operator actually reads. That wording was
    // covered only by the engine's unit tests, which see the ImportReport and
    // never the notice drawn from it, so this scenario runs the REAL sweep (the
    // same service call the folder picker makes, minus the dialog a script
    // cannot answer) over a folder the shell runner prepared — one sermon beside
    // a document, an image and a renamed spreadsheet — then reads every notice
    // the app raised, including the card's own rendered text, and logs them
    // verbatim for the runner to assert on.
    //
    // Armed only with BOTH VGR_SELFTEST=1 and VGR_TABLEFOLDER_TEST=1, and told
    // the folder by VGR_TABLEFOLDER_DIR: it imports real files and quits, so a
    // plain VGR_SELFTEST=1 run (how the NDI probes launch the app) never does.
    property int tableFolderTick: 0
    property var tableFolderSeen: []      // one entry per notice, oldest first
    function tableFolderCollect() {
        const items = NotificationCenter.items
        for (let i = 0; i < items.length; ++i) {
            const e = items[i]
            // The topic is this scenario's own filter: the sweep's notices are
            // the only ones the Table library raises, so a boot warning from
            // another producer can never be mistaken for the toast under test.
            if (String(e.topic).indexOf("table.") !== 0)
                continue
            if (window.tableFolderSeen.indexOf(e.message) !== -1)
                continue
            window.tableFolderSeen.push(e.message)
            console.log("[SELFTEST-TABLEFOLDER] notice " + window.tableFolderSeen.length
                        + " level=" + e.level + " title=" + e.title
                        + " message=" + e.message)
        }
    }
    Timer {
        id: selfTestTableFolderStart
        running: typeof SelfTestTableFolder !== "undefined" && SelfTestTableFolder
        // Boot, the library's own load and the first frame: the sweep writes
        // into the library the boot is still filling, so it must not race it.
        interval: 9000
        onTriggered: {
            const dir = (typeof SelfTestTableFolderDir !== "undefined" && SelfTestTableFolderDir) || ""
            console.log("[SELFTEST-TABLEFOLDER] start dir=" + dir)
            const started = dir !== "" && TheTableService.importSermonFolder(dir)
            console.log("[SELFTEST-TABLEFOLDER] started=" + started)
            if (!started) {
                console.log("[SELFTEST-TABLEFOLDER] result started=false notices=0")
                selfTestTableFolderQuit.restart()
                return
            }
            selfTestTableFolderTick.restart()
        }
    }
    Timer {
        id: selfTestTableFolderTick
        interval: 500
        repeat: true
        onTriggered: {
            ++window.tableFolderTick
            window.tableFolderCollect()
            // The outcome notices are pushed from the sweep's own queued callback,
            // so a few ticks after `importing` goes false is what lets them reach
            // the center — and 120 ticks (a minute) is the bound, so a sweep that
            // never finishes still reports instead of hanging the run.
            const settled = !TheTableService.importing && window.tableFolderTick > 4
            const bounded = window.tableFolderTick >= 120
            if (!settled && !bounded)
                return
            selfTestTableFolderTick.stop()
            window.tableFolderCollect()
            let refused = ""
            for (let i = 0; i < window.tableFolderSeen.length; ++i)
                if (String(window.tableFolderSeen[i]).indexOf("Skipped ") === 0)
                    refused = window.tableFolderSeen[i]
            // What is ON SCREEN for those notices: the overlay stacks one card
            // per item, in order, so the last card is the newest notice (the
            // refusals) and its own `message` is the wording it was handed. A
            // wording that reached the center but never reached a card would be
            // no answer to "what does the operator read?".
            const overlay = SelfTest.findItem("selfTestToast")
            // The overlay's children are the cards (the Repeater itself is one
            // child too, and carries no objectName): the last CHILD that is a
            // card is the newest notice on screen.
            const kids = overlay ? overlay.children : []
            let cards = 0
            let last = null
            for (let i = 0; i < kids.length; ++i) {
                if (kids[i].objectName !== "selfTestToastCard")
                    continue
                ++cards
                last = kids[i]
            }
            console.log("[SELFTEST-TABLEFOLDER] summary importing=" + TheTableService.importing
                        + " tick=" + window.tableFolderTick
                        + " notices=" + window.tableFolderSeen.length
                        + " refusedLen=" + refused.length
                        + " bounded=" + bounded)
            console.log("[SELFTEST-TABLEFOLDER] cards=" + cards
                        + " lastMessage=" + (last ? last.message : ""))
            selfTestTableFolderQuit.restart()
        }
    }
    Timer {
        id: selfTestTableFolderQuit
        interval: 1500              // the log lines above, on their way to the log
        onTriggered: SelfTest.quit()
    }

}
