import QtQuick
import QtQuick.Controls
import VGRPresenterUI
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
            EventBus.notify(
                qsTr("Recovered from an unexpected shutdown last run (%1). Details were written to the crash log.")
                    .arg(pendingCrashSummary),
                "error", qsTr("Stability"), "ui.crash.recovered")
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
        onTabSelected: (tab) => {
            // Edit means "edit what I'm looking at": a show clicked open in
            // the Show screen's centre preview (ShowCenter) is what Edit
            // should load — switching the tab alone used to leave the Edit
            // screen on whatever it already had (often nothing), ignoring
            // the show right there on screen. Only reload when it's a
            // DIFFERENT show than what's already open, so flipping back and
            // forth while actively editing doesn't reset undo history.
            if (tab === "edit") {
                const item = showScreen.centerItem
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
    Shortcut {
        sequence: "Ctrl+K"
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
        sequence: "Ctrl+I"
        onActivated: importDialog.open = true
    }

    // Ctrl+R: File > Record… — the same toggle.
    Shortcut {
        sequence: "Ctrl+R"
        onActivated: {
            if (RecordingService.recording)
                RecordingService.stopRecording()
            else
                RecordingService.startRecording(RecordingService.config)
        }
    }

    // Ctrl+N / O / S / Shift+S — the same actions as the File menu.
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: {
            editScreen.newShow()
            window.currentView = "edit"
        }
    }
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: {
            window.currentView = "edit"
            editScreen.openShow()
        }
    }
    Shortcut {
        sequence: "Ctrl+S"
        onActivated: editScreen.saveShow()
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: editScreen.saveShowAs()
    }

    // "Save changes?" over any screen; the show session asks through it.
    UnsavedChangesDialog {
        id: unsavedDialog
        saveAction: function () { return editScreen.saveShow() }
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
    // Shared ModalScrim: click-dismisses, and consumes wheel events so a
    // modal's scroll never leaks into the Flickables on the page behind.
    ModalScrim {
        id: settingsScrim
        z: 30000
        visible: false
        // NOT scaled (see uiScale): this rectangle IS the modal's black box — the
        // full-window dim that makes the dialog read as modal. Scaling the overlay
        // shrank the backdrop away from the window's own edges, so the card floated
        // over an undimmed app. The CARD below carries the app scale; anything
        // nested inside it inherits that and must not scale again.
        // ModalScrim's #99000000 is exactly the 0.6 black this overlay
        // always used — no visual change, just the shared behavior.
        onDismissed: settingsScrim.visible = false
        // Opening and closing Settings is part of "where you were" (see
        // rememberSettingsPage): open records the page on screen, closed clears it.
        onVisibleChanged: window.rememberSettingsPage()

        ModalShell {
            id: settingsShell
            // The hook this card's fit scenario grabs (ModalShell has no
            // objectName of its own — the shell is the app's only one).
            objectName: "selfTestSettingsShell"
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
            // the popup closes with the X (or by clicking outside it).
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

    // ---- app-wide search ranking (VGR_RANK_TEST=1) --------------------------
    // User report: "compelling that you recieve" listed Bible verses that carry
    // only "you that", with the sermon that says "and compelling and compulsory,
    // that you receive the Holy Ghost" nowhere near the top — while the same
    // search leads with that sermon for "compelling and compulsory". This
    // scenario prints the rows the dialog ACTUALLY shows, in order, with how many
    // of the typed words each one carries, so the ranking is readable from a log
    // rather than a screenshot.
    readonly property string rankQuery: "compelling that you recieve"
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
                        + " bibles=" + SearchService.bibles.length
                        + " syncRows=" + SearchService.search(window.rankQuery, 4, false).length)
            for (let i = 0; i < rows.length; ++i)
                console.log("[SELFTEST-RANK] row" + i + "=" + rows[i])
            const first = quickSearch.results.length > 0 ? quickSearch.results[0] : null
            console.log("[SELFTEST-RANK] result top=" + (first ? first.kind : "-")
                        + " topTitle=" + (first ? String(first.title).substring(0, 60) : "-"))
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
            .replace(/<b><font color="#ffd54a">/g, "«")
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
            const hintItem = SelfTest.findItem("selfTestPaneSearchHint")
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
