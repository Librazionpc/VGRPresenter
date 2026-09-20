import QtQuick
import QtQuick.Controls
import VGRPresenterUI
import "components"
import "screens"
import "screens/main"
import "screens/edit"

ApplicationWindow {
    id: window
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 640
    visible: true
    title: "VGRPresenter"
    color: Theme.windowBg

    property string settingsSection: "general"
    // "show" | "edit" | "stage" — Stage has no screen yet, so its tab click
    // is accepted but doesn't switch the view.
    property string currentView: "show"

    // Previous-run crash report, delivered on the standard notification
    // pipeline the moment QML is alive. (See main.cpp: the CrashHandler can
    // only leave a log file from inside a dying process — publishing to the
    // bus it may have crashed is unsafe by design — so this is the UI half
    // of that contract. Rotation in ConsumePendingCrashSummary makes it
    // exactly-once per crash.)
    Component.onCompleted: {
        // Previous-run crash report — exactly-once (rotated aside at read).
        if (typeof pendingCrashSummary !== "undefined" && pendingCrashSummary !== "") {
            EventBus.notify(
                qsTr("Recovered from an unexpected shutdown last run (%1). Details were written to the crash log.")
                    .arg(pendingCrashSummary),
                "error", qsTr("Stability"), "ui.crash.recovered")
        }
        // Boot report with REAL platform facts: the engine's device
        // enumeration ran during boot, before any QML existed, so the
        // "engine.boot" event had no subscriber — the summary is pulled
        // here instead of pushed.
        const boot = EngineBridge.bootSummary()
        if (boot !== "")
            EventBus.notify(boot, "success", qsTr("Engine"), "engine.boot")
    }

    // Screens first (opaque, fill the window), then the shared header strip
    // and the menu layer on top — AppMenuBar must sit above AppHeader because
    // its logo/menu labels are drawn inside the same 48px strip.
    VGRPresenterMainScreen {
        anchors.fill: parent
        visible: window.currentView === "show"
    }

    EditScreen {
        id: editScreen
        anchors.fill: parent
        visible: window.currentView === "edit"
    }

    // UI self-test: resolve an edit-screen + menu chip into window coords
    // (C++ findChild can't reach Repeater delegates; see EditScreen note).
    function selfTestCenter(name) {
        if (name === "selfTestAddContent")
            return SelfTest.itemCenter(name)
        const p = editScreen.selfTestItemCenter(name)
        return p || SelfTest.itemCenter(name)   // fall back to C++ path
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
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        activeTab: window.currentView
        onTabSelected: (tab) => {
            if (tab === "show" || tab === "edit") window.currentView = tab
        }
        onSettingsClicked: window.openSettings("general")
    }

    AppMenuBar {
        anchors.fill: parent
        onSettingsRequested: (section) => window.openSettings(section)
    }

    // Topmost cursor layer — renders the AppCursor override stack's shape
    // while any gesture (canvas move/resize, AV line-drag) has one pushed;
    // inert the rest of the time. Declared LAST so it z-orders above every
    // screen, the menu bar, and all modals. Also the app's single pointer-
    // position truth source: every position-driven hover (PositionHoverArea)
    // reads AppCursor's point, so this must never be hidden or disabled.
    AppCursorCatcher { }

    // ---- UI self-test scenario (TEMPORARY diagnostic, env-gated) ----------
    // Drives the real UI with real OS cursor moves through SelfTestDriver
    // (main.cpp only wires it when VGR_SELFTEST=1) to reproduce the two
    // rendering-layer bug reports end to end and grab PNGs for offline
    // pixel sampling:
    //   1. "shape canvas items paint nothing"  -> canvas grab with a shape
    //      rectangle on it (VGR_SELFTEST_SHOT_CANVAS)
    //   2. "chip row highlight flickers"       -> a scripted hover sweep
    //      ACROSS the chip row: multiple passes, then a grab mid-sweep and
    //      two grabs of the SAME chip 400ms apart while stationary, to
    //      compare pixel-identicalness (VGR_SELFTEST_SHOT_CHIPS)
    // Coordinates are the app's fixed 1440x900 layout positions read from
    // the QML (Add slide chip, + content chip, menu chips, modal buttons).
    Timer {
        id: selfTestStage
        running: typeof SelfTest !== "undefined"
        interval: 1200
        onTriggered: {
            console.log("[SELFTEST] stage 1: edit view")
            window.currentView = "edit"
            selfTestStage2.restart()
        }
    }
    Timer {
        id: selfTestStage2
        interval: 500
        onTriggered: {
            console.log("[SELFTEST] stage 2: add slide")
            // Empty-state veil Column centerIn mCanvas (286,308,754,428);
            // chip is the Column's last child -> center ~(663, 564).
            SelfTest.click(663, 564)
            selfTestStage2b.restart()
        }
    }
    Timer {
        id: selfTestStage2b
        interval: 500
        onTriggered: {
            // Verification grab: the veil must be GONE in this shot.
            SelfTest.grab("", "shot_stage2.png")
            selfTestStage3.restart()
        }
    }
    Timer {
        id: selfTestStage3
        interval: 500
        onTriggered: {
            console.log("[SELFTEST] stage 3: open + menu")
            // Self-locating: the chip's center by objectName.
            SelfTest.clickItem("selfTestAddContent")
            selfTestStage4.restart()
        }
    }
    Timer {
        id: selfTestStage4
        interval: 500
        onTriggered: {
            console.log("[SELFTEST] stage 4: pick Shape (self-locating)")
            // Chip positions shift whenever the contentTypes roster changes
            // (NDI was added) — resolve the live center from QML (see
            // selfTestCenter), then click it.
            const c = window.selfTestCenter("selfTestChip4")
            if (c)
                SelfTest.click(c.x, c.y)
            else
                console.log("[SELFTEST] chip1 not resolvable — menu closed?")
            selfTestStage5.restart()
        }
    }
    Timer {
        id: selfTestStage5
        interval: 600
        onTriggered: {
            console.log("[SELFTEST] stage 5: Add Shape (self-locating)")
            // Footer button position depends on modal metrics that have
            // changed more than once — click by objectName. "rectangle"
            // is the picker's DEFAULT selection, so no grid click needed.
            SelfTest.clickItem("selfTestAddShape")
            selfTestStage6.restart()
        }
    }
    Timer {
        id: selfTestStage6
        interval: 700
        onTriggered: {
            console.log("[SELFTEST] stage 6: move shape onto canvas + grab")
            // The new 220x44 shape item lands at (40,40) on the canvas.
            // Also grab the whole window for full context.
            SelfTest.grab("", "shot_window.png")
            SelfTest.grab("selfTestCanvas", "shot_canvas.png")
            selfTestStage7.restart()
        }
    }
    // Chip-row flicker probe. The Add Shape click closed the + menu, so
    // first RE-OPEN it, then park the cursor exactly on one chip (Camera,
    // center 430,860) and grab the menu region twice 400ms apart while
    // stationary — a real flicker shows as a pixel difference between the
    // two grabs.
    Timer {
        id: selfTestStage7
        interval: 400
        onTriggered: {
            console.log("[SELFTEST] stage 7: reopen + menu for chip probe")
            SelfTest.clickItem("selfTestAddContent")
            // NEXT tick resolves + sweeps: the click above has not been
            // processed yet (same-frame itemCenter returned null while the
            // menu was still closed — the async click lesson from stage 4).
            selfTestStage7r.restart()
        }
    }
    Timer {
        id: selfTestStage7r
        interval: 500
        onTriggered: {
            const target = window.selfTestCenter("selfTestChip4")
            const from = SelfTest.itemCenter("selfTestAddContent")
            if (!target) {
                console.log("[SELFTEST] stage 7: chip4 unresolvable — aborting probe")
                SelfTest.quit()
                return
            }
            // Stepped sweep like a real hand — now SLOW: 60 small moves at
            // 80ms (~5s total), reproducing the user's slow-hover flicker
            // report (the earlier 12-step/40ms sweep never reproduced it).
            // A mid-sweep window grab catches the menu WITH the truth
            // readout (ptr=… zone=… under the menu) mid-motion.
            selfTestStage7b.path = []
            for (let i = 1; i <= 60; ++i)
                selfTestStage7b.path.push(
                            [from.x + (target.x - from.x) * i / 60,
                             from.y + (target.y - from.y) * i / 60])
            selfTestStage7b.step = 0
            selfTestStage7b.restart()
        }
    }
    Timer {
        id: selfTestStage7b
        interval: 80
        repeat: true
        property int step: 0
        property var path: []
        onTriggered: {
            if (step < path.length) {
                const p = path[step++]
                SelfTest.move(p[0], p[1])
                // Mid-sweep truth capture: the menu + readout while moving.
                if (step === 30)
                    SelfTest.grab("", "shot_midsweep.png")
            } else {
                repeat = false
                selfTestStage8.restart()
            }
        }
    }
    Timer {
        id: selfTestStage8
        interval: 600
        onTriggered: {
            SelfTest.grab("selfTestChips", "shot_chips_a.png")
            selfTestStage9.restart()
        }
    }
    Timer {
        id: selfTestStage9
        interval: 400
        onTriggered: {
            SelfTest.grab("selfTestChips", "shot_chips_b.png")
            selfTestStage10.restart()
        }
    }
    Timer {
        id: selfTestStage10
        interval: 400
        onTriggered: {
            console.log("[SELFTEST] done")
            SelfTest.quit()
        }
    }

    // ---- Settings overlay ----
    // Shared ModalScrim: click-dismisses, and consumes wheel events so a
    // modal's scroll never leaks into the Flickables on the page behind.
    ModalScrim {
        id: settingsScrim
        visible: false
        // ModalScrim's #99000000 is exactly the 0.6 black this overlay
        // always used — no visual change, just the shared behavior.
        onDismissed: settingsScrim.visible = false

        ModalShell {
            anchors.centerIn: parent
            currentKey: window.settingsSection
            onSectionSelected: (key) => window.settingsSection = key
            onCloseRequested: settingsScrim.visible = false
            onCancelRequested: settingsScrim.visible = false
            onSaveRequested: settingsScrim.visible = false

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
                        case "av": return audioVideoScreenComponent
                        default: return placeholderComponent
                        }
                    }
                }
        }
    }

    Component {
        id: generalScreenComponent
        GeneralScreen {}
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
        OutputsScreen {}
    }

    Component {
        id: stylesScreenComponent
        StylesScreen {}
    }

    Component {
        id: audioVideoScreenComponent
        AudioVideoScreen {}
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

    // Declared LAST (after the Settings overlay) so a crash/error toast
    // always z-orders above an open modal, not just the screens underneath.
    NotificationOverlay {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.space5
        z: 10000
    }
}
