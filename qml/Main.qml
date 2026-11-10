import QtQuick
import QtQuick.Controls
import VGRPresenterUI
import "shortcuts"
import "components"
import "screens"
import "screens/main"
import "screens/edit"

ApplicationWindow {
    id: window
    // ---- the desktop this app opens on and scales to -------------------------
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
    readonly property real availableScreenWidth: Math.min(window.desktopWidth,
                                                          Screen.desktopAvailableWidth > 0 ? Screen.desktopAvailableWidth : window.desktopWidth)
    readonly property real availableScreenHeight: Math.min(window.desktopHeight,
                                                           Screen.desktopAvailableHeight > 0 ? Screen.desktopAvailableHeight : window.desktopHeight)
    width: Math.max(Math.min(820, availableScreenWidth), availableScreenWidth - 32)
    height: Math.max(Math.min(560, availableScreenHeight), availableScreenHeight - 64)
    minimumWidth: Math.min(820, availableScreenWidth)
    minimumHeight: Math.min(560, availableScreenHeight)
    maximumWidth: Math.max(minimumWidth, availableScreenWidth)
    maximumHeight: Math.max(minimumHeight, availableScreenHeight)

    // ---- ONE app scale, enforced on every layer -------------------------------
    readonly property real designWidth: 1440
    readonly property real designHeight: 900
    // The user's text size (Settings · General · Appearance) multiplies the scale, so
    // every surface — text included — grows together.
    readonly property real textScale: (Number(SettingsService.values["appearance.textScale"]) || 120) / 100
    readonly property real uiScale: Math.min(window.width / designWidth, window.height / designHeight) * textScale
    readonly property real canvasWidth: window.width / (uiScale || 1)
    readonly property real canvasHeight: (window.height - window.headerExtra) / (uiScale || 1)
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    title: (ShowService.hasShow ? ShowService.showName + (ShowService.showDirty ? " •" : "") + " — " : "") + "VGRPresenter"
    color: Theme.windowBg

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
            // "screen" rows name a monitor or a captured window (both live in the screen roster) and
            // "camera" rows name a capture device. EVERY OTHER kind is not a device at all: a media row's
            // sublabel is a FILE PATH, The Table's is its own name, and an NDI source is found by the NDI
            // finder, not the camera roster. They used to be pushed here too, so each start reported
            // "Not connected this time: <your video file>, The Table" for sources that were perfectly fine.
            const kind = String(row.kind || "")
            if (kind === "screen") screens.push(label)
            else if (kind === "camera") video.push(label)
        }
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const row = OutputListModel.getOutput(i)
            const screen = String(row && row.screenName ? row.screenName : "").trim()
            if (screen !== "") screens.push(screen)
        }
        EngineBridge.requestUsedDevices(audio, video, screens)
    }

    // ---- The engine boot report (see Component.onCompleted) ------------------
    property bool bootReportShown: false
    function reportBoot(force) {
        if (window.bootReportShown)
            return
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
            EngineBridge.log("info", "Engine", boot)
            EventBus.notify(boot, "success", qsTr("Engine"), "engine.boot",
                            EngineBridge.bootInUseDevices())
        }
    }
    // ---- A recovered crash, and whether this machine could report one -------
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
        EventBus.notify(window.crashToolingNoteText(), "error", qsTr("Stability"), "ui.crash.tooling")
    }
    Connections {
        target: EngineBridge
        function onCrashToolingChanged() { window.noteCrashTooling() }
    }

    // ---- Startup-forensics verdict self-test (VGR_FORENSICS_TEST=1) ----------
    property int forensicsTries: 0
    property int forensicsNoteTries: 0
    property bool forensicsNoteSeen: false
    Timer {
        id: selfTestForensicsPoll
        running: typeof SelfTestForensics !== "undefined" && SelfTestForensics
        interval: 700
        repeat: true
        onTriggered: {
            const note = window.crashToolingNoteText()
            const items = NotificationCenter.items
            for (let i = 0; i < items.length; ++i)
                if (items[i].message === note)
                    window.forensicsNoteSeen = true
            if (EngineBridge.crashToolingState === "checking" && window.forensicsTries++ < 20)
                return
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
        interval: 6000
        onTriggered: {
            window.reportBoot(true)
            window.reportMissingDevices()
        }
    }

    property string settingsSection: "general"
    property int pendingScreenEdit: -1
    // The Hotkeys scope Settings pushes while it is open (Esc closes it) — see
    // the settingsScrim's onVisibleChanged and qml/settings/Hotkeys.qml.
    property int settingsHotkeyScope: 0
    readonly property int headerHeight: Math.round(appHeader.height * window.uiScale)
    readonly property int headerExtra: Math.round((appHeader.height - 48) * window.uiScale)
    // "show" | "edit" | "stage" — Stage has no screen yet, so its tab click
    // is accepted but doesn't switch the view.
    property string currentView: "show"
    // The window-level layer that carries a drag between panes (DragSource / DropArea); see DragLayer.qml.
    readonly property var dragLayer: dragLayerItem

    Component.onCompleted: {
        // Set the saved Outputs-page media-fit default before any take can
        // fall back to the service's startup default.
        LiveOutputService.setDefaultMediaFit(
            String(SettingsService.value("outputs.defaultMediaFit") ?? "contain"))
        try {
            OutputListModel.adoptStyleRoster(StyleListModel)
            console.log("[SELFTEST-STYLEBG] style roster hand-over done")
        } catch (e) {
            console.log("[SELFTEST-STYLEBG] style roster hand-over FAILED: " + e)
        }
        AppScale.register(window)
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
            window.noteCrashTooling()
        }
        if (EngineBridge.bootError.length > 0)
            EventBus.notify(EngineBridge.bootError, "error", qsTr("Engine failed to boot"), "engine.boot")
        else
            window.reportBoot(false)
        bootReportFallback.restart()

        requestBootDevices()

        // Search: the Bible files are read into the engine (background thread) so verses
        // can be found.
        SearchService.loadBibles()
    }

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
        onDesignEditRequested: (kind, id) => {
            if (editScreen.openDesign(kind, id))
                window.currentView = "edit"
            else
                EventBus.notify(qsTr("That %1 no longer exists.").arg(kind), "warning", qsTr("Edit"))
        }
        onShowTemplateEditRequested: (id) => {
            if (editScreen.openDesign("template", id))
                window.currentView = "edit"
            else
                EventBus.notify(qsTr("The new template could not be opened for editing."), "warning", qsTr("Templates"))
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

    function rememberSettingsPage() {
        if (!behaviors.started)
            return
        const at = settingsScrim.visible ? window.settingsSection : ""
        if (at !== String(SettingsService.values["session.lastSettingsSection"] ?? "")) {
            SettingsService.setValue("session.lastSettingsSection", at)
            EngineBridge.log("info", "Navigation",
                             at === "" ? qsTr("Settings closed") : qsTr("Settings page: ") + at)
        }
    }

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
        editAvailable: ShowService.hasShow
            || !!(showScreen.centerItem && showScreen.centerItem.type === "show")
        onTabSelected: (tab) => {
            if (tab === "edit") {
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
        onResultChosen: (result) => window.openSearchResult(result)
    }

    // What picking a search result does. The dialog only finds things (SearchService, backed
    // by the engine); this is the one place that knows how to go to them.
    function openSearchResult(r) {
        const openT0 = Date.now()
        switch (r.kind) {
        case "show":
            showScreen.centerItem = { type: "show", ref: r.path, name: r.title, category: r.category ?? "", layout: "" }
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
        // Record…: start/stop the real engine session — the first Recording output
        // on Routing, with the preset it is wired to (Settings · Recording).
        onRecordToggled: {
            if (RecordingService.recording)
                RecordingService.stopRecording()
            else
                RecordingService.startPrimaryRecording()
        }
    }

    Shortcut {
        action: "importShow"
        enabled: !editScreen.textStyleKeysLive
        onActivated: importDialog.open = true
    }

    // Ctrl+R: File > Record… — the same toggle.
    Shortcut {
        action: "record"
        onActivated: {
            if (RecordingService.recording)
                RecordingService.stopRecording()
            else
                RecordingService.startPrimaryRecording()
        }
    }

    // Ctrl+Shift+T: the wall's timer button, on the keyboard. It is one toggle in the service, so the key,
    // the wall button and the scenario that proves the timer all press the same thing.
    Shortcut {
        action: "toggleTimer"
        enabled: !Hotkeys.canAccept
        onActivated: LiveOutputService.toggleAutoAdvance()
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

    WindowResizeHandles {
        anchors.fill: parent
        captionReserveWidth: appHeader.captionClusterWidth * window.uiScale
        captionReserveHeight: appHeader.captionClusterHeight * window.uiScale
    }

    AppCursorCatcher { }

    // ---- Settings overlay ----
    // Shared ModalScrim: traps outside clicks and consumes wheel events so a
    // modal stays open and scrolling never leaks into the page behind.
    ModalScrim {
        id: settingsScrim
        z: 30000
        visible: false
        blurSource: ModalStack.pageLayer ? ModalStack.pageLayer() : null
        Component.onCompleted: ModalStack.pageLayer = function() {
            return window.currentView === "edit" ? editScreen : showScreen
        }
        onDismissed: settingsScrim.visible = false
        onVisibleChanged: {
            // Opening and closing Settings is part of "where you were" (see
            // rememberSettingsPage): open records the page on screen, closed clears it.
            window.rememberSettingsPage()
            // The page underneath must not answer clicks that belong to this modal.
            ModalStack.settingsOpen = settingsScrim.visible
            if (settingsScrim.visible) {
                // A menu the page lifted into the window overlay would float ABOVE this
                // card — both live in Overlay.overlay, and the menu was opened later.
                showScreen.closeContextMenus()
            }
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
            Component.onCompleted: ModalStack.settingsShell = settingsShell
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
                        case "routing": return routingScreenComponent
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
        OutputsScreen {
            objectName: "outputsScreen"
            // Routing's "Edit screen" hop: the row it wants opened, -1 = none.
            editRequestRow: window.pendingScreenEdit
            // Cleared once the page has opened it, so returning here later does
            // not pop the same editor again.
            onEditRequestConsumed: window.pendingScreenEdit = -1
        }
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
        id: routingScreenComponent
        RoutingScreen {
            objectName: "routingScreen"
            onSectionRequested: (key) => window.settingsSection = key
            onScreenEditRequested: (row) => {
                window.pendingScreenEdit = row
                window.settingsSection = "outputs"
            }
        }
    }

    Component {
        id: placeholderComponent
        PlaceholderScreen {
            title: {
                switch (window.settingsSection) {
                case "outputs": return "Screens"
                default: return window.settingsSection
                }
            }
        }
    }

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

    ToolTipLayer {
        id: toolTipLayer
        anchors.fill: parent
    }

    OutputWindowManager { id: outputWindows }

    // File > Import: every format the engine reads (FreeShow's Import screen). Above the screens and the Settings overlay, below the toasts.
    ImportDialog {
        id: importDialog
        objectName: "selfTestImportDialog"
        // The app scale lives on the dialog's CARD (ImportDialog.qml, through the
        // AppScale singleton); scaling this root would shrink its dim backdrop.
        onClosed: importDialog.open = false
    }

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

    NotificationOverlay {
        // Hook for the modal-fit scenario's toast step (see the sweep below).
        objectName: "selfTestToast"
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: Theme.space5
        scale: window.uiScale
        transformOrigin: Item.BottomRight
        z: 40000
    }

    // ---- Settings-inset self-test (VGR_SELFTEST=1 only) ----------------------
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

    // ---- SCROLL MODE / SLIDE MODE: the switch every reading walk lives behind --
    function setReadingScrollMode(want, scenario) {
        const on = !!want
        let by = "already"
        if (LiveOutputService.scrollMode !== on) {
            by = "service"
            if (SelfTest.itemVisible("selfTestWallScrollMode")) {
                SelfTest.activate()
                SelfTest.clickItem("selfTestWallScrollMode")
                if (LiveOutputService.scrollMode === on)
                    by = "button"
            }
            if (by !== "button")
                LiveOutputService.scrollMode = on
        }
        console.log("[SELFTEST-READING] scenario=" + scenario
                    + " mode=" + (on ? "scroll" : "slide") + " by=" + by
                    + " now=" + (LiveOutputService.scrollMode ? "scroll" : "slide"))
        return LiveOutputService.scrollMode === on
    }

    // ---- Reading-step self-test (VGR_LINESTEP_TEST=1 only) --------------------
    //  1. the position in the PICK (`abs`, see readingAbsoluteLine) grows by
    //  2. THE WALL IS ON THE PAGE THE WALK IS ON. Every monitor tile's own
    //  3. NO SKIPPING: the position is sampled every 16 ms through the glide, and
    property int selfTestLineStepPress: 0
    property bool selfTestLineStepBack: false
    property int selfTestLineStepBackPress: 0
    property var selfTestLineStepSlides: []
    // The per-frame sampler's state (see the sample timer below).
    property int selfTestLineStepDir: 1
    property real selfTestLsPrevAbs: -1
    property real selfTestLsGlideWorst: 0
    property int selfTestLsGlideSkips: 0
    property string selfTestLsGlideSkipAt: ""
    // A sample that arrives LATE says the interface thread was busy, not that the glide
    // jumped a line: with 137 ms spent inside one tell, two 16 ms samples straddle the
    // whole 190 ms glide and the difference is a full line either way. Those are counted
    // as late, and only an on-time sample can report a skipped frame.
    property real selfTestLsPrevAt: 0
    property int selfTestLsGlideLate: 0
    function lsCompText(comp) {
        const bs = (comp && comp.blocks) ? comp.blocks : []
        let best = ""
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            if (!b || (b.kind !== undefined && b.kind !== "text"))
                continue
            const t = String(b.text ?? "")
            if (t.length > best.length)
                best = t
        }
        return best
    }
    function lsTileText(tile) {
        return window.lsCompText(tile ? tile.onAirSlide : null)
    }
    function lsCompSig(comp) {
        const bs = (comp && comp.blocks) ? comp.blocks : []
        let s = ""
        for (let i = 0; i < bs.length; ++i)
            s += String(bs[i].text ?? "") + "\u0001"
        return s
    }
    function lsTileShowsPage(tile) {
        const text = window.lsTileText(tile)
        if (text === "")
            return -1
        for (let k = 0; k < selfTestLineStepSlides.length; ++k) {
            const body = String(selfTestLineStepSlides[k].body ?? "")
            if (body !== "" && text.indexOf(body.slice(0, 24)) === 0)
                return k
        }
        return -1
    }
    function lsWallLines(press) {
        const wall = SelfTest.findItem("selfTestMonitorWall")
        if (!wall)
            return ["[SELFTEST-LINESTEP] press=" + press + " wall=missing"]
        const flow = String(LiveOutputService.readingFlowText ?? "")
        const lines = []
        const count = OutputListModel.rowCount()
        for (let i = 0; i < count; ++i) {
            const tile = window.bsFindTile(wall, i)
            if (!tile) {
                lines.push("[SELFTEST-LINESTEP] press=" + press + " tile=" + i + " missing=yes")
                continue
            }
            const text = window.lsTileText(tile)
            lines.push("[SELFTEST-LINESTEP] press=" + press + " tile=" + i
                       + " shows=" + window.lsTileShowsPage(tile)
                       + " flow=" + (text !== "" && flow.indexOf(text) >= 0)
                       + " tileLen=" + text.length
                       + " flowLen=" + flow.length
                       + " flowHead=" + JSON.stringify(flow.slice(0, 24))
                       + " text=" + JSON.stringify(text.slice(0, 20)))
        }
        return lines
    }
    Timer {
        id: selfTestLineStepStage
        running: typeof SelfTestLineStep !== "undefined" && SelfTestLineStep
        interval: 2600          // let the first frame land
        onTriggered: {
            if (!window.setReadingScrollMode(true, "linestep"))
                console.log("[SELFTEST-LINESTEP] FAIL not-scroll — the walk needs Scroll mode")
            const words = ["alpha", "bravo", "charlie", "delta", "echo",
                           "foxtrot", "golf", "hotel", "india", "juliet"]
            const slides = []
            // 24 pages: more than the run's own window (4 pages each way), so the
            // walk makes the window SLIDE — see this scenario's header comment.
            for (let p = 0; p < 24; ++p) {
                let body = "p" + (p + 1) + " "
                for (let i = 0; i < 12; ++i)
                    body += words[(i + p) % words.length] + " "
                slides.push({ valid: true, title: "Probe " + (p + 1), body: body,
                              blocks: [], background: "" })
            }
            selfTestLineStepSlides = slides
            LiveOutputService.stageSlideSet("LineStep probe", slides, 0)
            console.log("[SELFTEST-LINESTEP] staged " + slides.length + " page(s)"
                        + " mode=" + (LiveOutputService.scrollMode ? "scroll" : "slide"))
            selfTestLineStepTick.restart()
            selfTestLineStepSample.restart()
        }
    }
    // 0.02 lines per millisecond, so even a sample delayed by 50 ms by a busy
    Timer {
        id: selfTestLineStepSample
        running: (typeof SelfTestLineStep !== "undefined" && SelfTestLineStep)
                 || (typeof SelfTestTableWalk !== "undefined" && SelfTestTableWalk)
        interval: 16
        repeat: true
        onTriggered: {
            const now = Date.now()
            const abs = Number(LiveOutputService.readingAbsoluteLine)
            if (selfTestLsPrevAbs >= 0) {
                const d = abs - selfTestLsPrevAbs
                const backward = d * selfTestLineStepDir < -0.001
                // Three timer intervals: up to two missed ticks is still an on-time sample.
                const late = selfTestLsPrevAt > 0 && (now - selfTestLsPrevAt) > 48
                if (late)
                    ++selfTestLsGlideLate
                else if (Math.abs(d) > selfTestLsGlideWorst)
                    selfTestLsGlideWorst = Math.abs(d)
                if (!late && (backward || Math.abs(d) > 1.0)) {
                    ++selfTestLsGlideSkips
                    if (selfTestLsGlideSkipAt === "")
                        selfTestLsGlideSkipAt = "frame " + selfTestLineStepPress
                            + " moved " + d.toFixed(3) + " to " + abs.toFixed(2)
                }
            }
            selfTestLsPrevAbs = abs
            selfTestLsPrevAt = now
        }
    }
    Timer {
        id: selfTestLineStepTick
        // 1.0 and never restart.
        interval: 700
        repeat: true
        onTriggered: {
            const dir = selfTestLineStepBack ? -1 : 1
            selfTestLineStepDir = dir
            const before = Number(LiveOutputService.readingScroll)
            const absBefore = Number(LiveOutputService.readingAbsoluteLine)
            const flowBefore = String(LiveOutputService.readingFlowText || "").length
            const pageBefore = Number(LiveOutputService.readingPage)
            const moved = LiveOutputService.lineStep(dir)
            const after = Number(LiveOutputService.readingScroll)
            const absAfter = Number(LiveOutputService.readingAbsoluteLine)
            const flowAfter = String(LiveOutputService.readingFlowText || "").length
            const blocks = (LiveOutputService.stagedSlide && LiveOutputService.stagedSlide.blocks)
                ? LiveOutputService.stagedSlide.blocks.length : -1
            // The glide that just finished shipping is logged WITH the press that
            // follows it: worst single-frame move and how many frames skipped.
            console.log("[SELFTEST-LINESTEP] press=" + selfTestLineStepPress
                        + " dir=" + dir
                        + " mode=" + (LiveOutputService.scrollMode ? "scroll" : "slide")
                        + " moved=" + moved
                        + " from=" + before.toFixed(2)
                        + " to=" + after.toFixed(2)
                        + " abs=" + absAfter.toFixed(2)
                        + " absFrom=" + absBefore.toFixed(2)
                        + " page=" + LiveOutputService.readingPage
                        + " pageBefore=" + pageBefore
                        + " passage=" + flowAfter
                        + " passageBefore=" + flowBefore
                        + " blocks=" + blocks
                        + " glideWorst=" + selfTestLsGlideWorst.toFixed(3)
                        + " glideSkips=" + selfTestLsGlideSkips
                        + " glideLate=" + selfTestLsGlideLate
                        + (selfTestLsGlideSkipAt !== "" ? " glideSkipAt=" + JSON.stringify(selfTestLsGlideSkipAt) : ""))
            const wall = window.lsWallLines(selfTestLineStepPress)
            for (let w = 0; w < wall.length; ++w)
                console.log(wall[w])
            selfTestLsGlideWorst = 0
            selfTestLsGlideSkips = 0
            selfTestLsGlideSkipAt = ""
            ++selfTestLineStepPress
            if (selfTestLineStepBack)
                ++selfTestLineStepBackPress
            if (!selfTestLineStepBack && selfTestLineStepPress > 33) {
                // the end of the pick: now walk the same boundaries back up
                selfTestLineStepBack = true
            } else if (selfTestLineStepBack && selfTestLineStepBackPress > 11) {
                selfTestLineStepTick.stop()
                selfTestLineStepSample.stop()
                selfTestLineStepDone.restart()
            }
        }
    }
    Timer {
        id: selfTestLineStepDone
        interval: 1200
        onTriggered: {
            console.log("[SELFTEST-LINESTEP] done")
            SelfTest.quit()
        }
    }

    // ---- REAL-PASSAGE reading walk (VGR_TABLEWALK_TEST=1 only) ---------------
    // "live:" puts the pick on air; "bible:" selects a Scripture result instead of an
    // earlier Table match. The prefixes can be combined for the real-output Bible walk.
    // A trailing "~back" turns the walk round after press 30: presses 31..36 step BACK, then the tiles
    // are photographed (tw_back6_*.png) — the check that every style follows a back-step like Main.
    readonly property string twQueryFull: (typeof SelfTestTableWalkQuery !== "undefined")
                                      ? String(SelfTestTableWalkQuery) : "47-1100X"
    readonly property bool twBack: window.twQueryFull.endsWith("~back")
    readonly property string twQueryRaw: window.twBack ? window.twQueryFull.slice(0, -5) : window.twQueryFull
    readonly property bool twGoLive: window.twQueryRaw.indexOf("live:") === 0
    // "live:swap:<table query>" = after the Table pick is on air, change to a Bible pick, show it, and
    // go off air (the user's stall report), timing every step with a GUI-gap heartbeat.
    readonly property string twAfterLive: window.twGoLive ? window.twQueryRaw.slice(5) : window.twQueryRaw
    readonly property bool twSwap: window.twAfterLive.indexOf("swap:") === 0
    readonly property string twSearchQuery: window.twSwap ? window.twAfterLive.slice(5) : window.twAfterLive
    readonly property bool twExpectBible: window.twSearchQuery.indexOf("bible:") === 0
    readonly property string twQuery: window.twExpectBible
                                    ? window.twSearchQuery.slice(6) : window.twSearchQuery
    property int twTries: 0
    property int twPress: 0
    property int twJump: 0
    // The rewind's own counter (see selfTestTableWalkRewind), declared for the same
    // reason: an undeclared property on the root throws on every write.
    property int twRewound: 0
    property var twExpectedBibleRef: null
    property int twBibleSettleTries: 0
    function twRefPara(text) {
        // The Table's reference carries the paragraph it is showing ("47-1100X -
        // Fellowship 2"): the trailing number IS the value the report was about.
        const m = String(text).match(/(\d+)\s*$/)
        return m ? Number(m[1]) : -1
    }
    // The reference pane the picked result opened: Scripture when a Bible chapter was chosen,
    // else The Table (the walk is the same for both).
    function twPane() {
        const bible = SelfTest.findItem("selfTestScripturePane")
        if (window.twExpectBible)
            return bible
        if (bible && bible.visible && bible.book)
            return bible
        return SelfTest.findItem("selfTestTablePane")
    }
    function twPaneLine(press) {
        const pane = window.twPane()
        if (!pane)
            return "[SELFTEST-TABLEWALK] press=" + press + " pane=missing"
        return "[SELFTEST-TABLEWALK] press=" + press + " pane=yes"
            + " page=" + LiveOutputService.readingPage
            + " char=" + LiveOutputService.readingCharOffset()
            + " refTop=" + JSON.stringify(window.twStyleView(String(OutputListModel.activeStyleId)).words)
            + " ref=" + JSON.stringify(String(pane.referenceText))
            + " para=" + window.twRefPara(pane.referenceText)
            + " selected=" + JSON.stringify(pane.selected)
            + " previewIndex=" + pane.previewSlideIndex
            + " ownerRef=" + JSON.stringify(String(pane.lineStepOwnerRef))
    }
    Timer {
        id: selfTestTableWalkOpen
        running: typeof SelfTestTableWalk !== "undefined" && SelfTestTableWalk
        interval: 70000
        onTriggered: {
            console.log("[SELFTEST-TABLEWALK] query=" + JSON.stringify(window.twQuery))
            quickSearch.openSearch()
            selfTestTableWalkQuery.restart()
        }
    }
    Timer {
        id: selfTestTableWalkQuery
        interval: 3000
        repeat: true
        onTriggered: {
            if (window.twTries === 0 || window.twTries % 5 === 0) {
                quickSearch.query = window.twQuery
                quickSearch.refresh()
            }
            ++window.twTries
            const r = window.twExpectBible
                ? quickSearch.results.find((result) => result.kind === "bible")
                : (quickSearch.results.length > 0 ? quickSearch.results[0] : null)
            if (r) {
                selfTestTableWalkQuery.stop()
                console.log("[SELFTEST-TABLEWALK] picking kind=" + r.kind + " verse=" + r.verse
                            + " title=" + JSON.stringify(String(r.title)))
                quickSearch.choose(r)
                selfTestTableWalkSettle.restart()
            } else if (window.twTries >= 25) {
                selfTestTableWalkQuery.stop()
                console.log("[SELFTEST-TABLEWALK] FAIL no-results after " + window.twTries
                            + " expectedKind=" + (window.twExpectBible ? "bible" : "any")
                            + " tries token=" + quickSearch.searchToken
                            + " pending=" + quickSearch.searchPending
                            + " kinds=" + JSON.stringify(quickSearch.results.slice(0, 5)
                                                         .map((result) => result.kind))
                            + " dialogVisible=" + quickSearch.visible)
                SelfTest.quit()
            }
        }
    }
    // The pane has a selection once the result is opened; taking it is the pane's
    // OWN play (the Enter key's body), which stages the whole chapter as the pick.
    Timer {
        id: selfTestTableWalkSettle
        interval: 3000
        onTriggered: {
            const pane = window.twPane()
            if (window.twExpectBible && pane) {
                const ref = pane.adapter.resolve(window.twQuery, pane.sourceId)
                if (!ref.bookId) {
                    if (++window.twBibleSettleTries < 20) {
                        selfTestTableWalkSettle.restart()
                        return
                    }
                    console.log("[SELFTEST-TABLEWALK] FAIL bible-reference-not-resolved query="
                                + JSON.stringify(window.twQuery))
                    SelfTest.quit()
                    return
                }
                if (window.twExpectedBibleRef === null) {
                    window.twExpectedBibleRef = ref
                    pane.goTo(ref)
                    selfTestTableWalkSettle.restart()
                    return
                }
                if (!pane.book || pane.book.id !== ref.bookId
                        || pane.chapterNumber !== Number(ref.chapter)
                        || pane.chapterLoading || pane.selected.length === 0) {
                    if (++window.twBibleSettleTries < 20) {
                        selfTestTableWalkSettle.restart()
                        return
                    }
                    console.log("[SELFTEST-TABLEWALK] FAIL bible-reference-not-open query="
                                + JSON.stringify(window.twQuery)
                                + " book=" + (pane.book ? pane.book.id : "-")
                                + " chapter=" + pane.chapterNumber)
                    SelfTest.quit()
                    return
                }
            }
            if (!pane || !pane.book || pane.selected.length === 0) {
                console.log("[SELFTEST-TABLEWALK] FAIL no-pick pane=" + (pane ? "yes" : "no")
                            + " book=" + (pane && pane.book ? pane.book.id : "-")
                            + " selected=" + (pane ? JSON.stringify(pane.selected) : "[]"))
                SelfTest.quit()
                return
            }
            if (!window.setReadingScrollMode(true, "tablewalk"))
                console.log("[SELFTEST-TABLEWALK] FAIL not-scroll — the walk needs Scroll mode")
            pane.playPicked()
            if (window.twGoLive)
                LiveOutputService.goLive()
            window.twRewound = 0
            selfTestTableWalkRewind.restart()
        }
    }
    Timer {
        id: selfTestTableWalkRewind
        interval: 120
        repeat: true
        onTriggered: {
            if (window.twRewound < 400 && LiveOutputService.lineStep(-1)) {
                ++window.twRewound
                return
            }
            selfTestTableWalkRewind.stop()
            console.log("[SELFTEST-TABLEWALK] rewound=" + window.twRewound
                        + " page=" + LiveOutputService.readingPage
                        + " abs=" + Number(LiveOutputService.readingAbsoluteLine).toFixed(2)
                        + " lines=" + LiveOutputService.readingRunLineStarts(String(OutputListModel.activeStyleId)).length)
            window.selfTestLsPrevAbs = -1
            window.selfTestLsGlideWorst = 0
            window.selfTestLsGlideSkips = 0
            selfTestTableWalkPress.restart()
        }
    }
    Timer {
        id: selfTestTableWalkPress
        // Past the glide's 600ms cap, so the position each press reports is the
        // settled one the previous press left on screen.
        interval: 700
        repeat: true
        onTriggered: {
            const press = window.twPress
            window.twPress = press + 1
            window.selfTestLineStepPress = press
            try {
                const absFrom = Number(LiveOutputService.readingAbsoluteLine)
                if (window.twBack && press === 37) {
                    window.twGrab("back6")
                    selfTestTableWalkPress.stop()
                    return
                }
                const moved = LiveOutputService.lineStep(window.twBack && press >= 31 && press <= 36 ? -1 : 1)
                if (window.twBack && press === 36) {
                    window.twMidN = 0
                    twMidGrab.restart()   // frames DURING the back-glide: every tile, same instants
                }
                const abs = Number(LiveOutputService.readingAbsoluteLine)
                const page = LiveOutputService.readingPage
                console.log("[SELFTEST-TABLEWALK] press=" + press
                            + " moved=" + moved
                            + " mode=" + (LiveOutputService.scrollMode ? "scroll" : "slide")
                            + " abs=" + abs.toFixed(2)
                            + " absFrom=" + absFrom.toFixed(2)
                            + " page=" + page
                            + " glideSkips=" + window.selfTestLsGlideSkips
                            + " glideLate=" + window.selfTestLsGlideLate
                            + " glideWorst=" + window.selfTestLsGlideWorst.toFixed(3))
                const wall = window.twTileLines(press)
                for (let w = 0; w < wall.length; ++w)
                    console.log(wall[w])
                console.log(window.twPaneLine(press))
                if (press === 0 || press === 30)
                    window.twGrab("press" + press)
                if (press === 30) {
                    // Hold still so tiles and the NDI output can be photographed AT REST: the
                    // glide has ended 1.5 s from now, and the walk stays put for 14 s.
                    selfTestTableWalkPress.stop()
                    twHold.restart()
                }
            } catch (err) {
                console.log("[SELFTEST-TABLEWALK] log-error press=" + press + " " + err)
            }
            window.selfTestLsGlideWorst = 0
            window.selfTestLsGlideSkips = 0
            if (window.twPress > 120) {
                selfTestTableWalkPress.stop()
                selfTestTableWalkJump.restart()
            }
        }
    }
    property int twMidN: 0
    Timer {
        id: twMidGrab
        interval: 45
        repeat: true
        onTriggered: {
            const n = window.twMidN++
            window.twGrab("mid" + n)
            if (n >= 3)
                twMidGrab.stop()
        }
    }
    Timer {
        id: twHold
        interval: 1500
        onTriggered: {
            window.twGrab("rest30")
            twResume.restart()
        }
    }
    Timer {
        id: twResume
        interval: 14000
        onTriggered: {
            // ON AIR (a "live:" query): do what a double-click on paragraph/verse 3 does, and let the
            // walk stay still while the output settles — the report was that NDI kept showing the
            // OLD start until the first step. TWGRAB phase=pick3 marks it in engine.log.
            if (window.twGoLive) {
                const pane = window.twPane()
                if (pane) {
                    EngineBridge.log("info", "TWGRAB", "phase=pick3")
                    if (!pane.selfTestDoubleClickRow(0))   // a row's real click + double-click
                        pane.stageSubpart(1, 0)
                    twAfterPick.restart()
                    return
                }
            }
            selfTestTableWalkPress.restart()
        }
    }
    Timer {
        id: twAfterPick
        interval: 9000
        onTriggered: {
            window.twGrab("afterpick")      // the tiles, right after the pick has settled
            const listPane = window.twPane()
            if (listPane)
                EngineBridge.log("info", "TWGRAB", "list afterpick " + listPane.selfTestListState())
            EngineBridge.log("info", "TWGRAB", "phase=pick3done")
            if (window.twSwap) {
                twSwapOpen.restart()
                return
            }
            // The same double-click in SLIDE mode, for the timing breakdown (PickCost lines in engine.log).
            if (window.setReadingScrollMode(false, "tablewalk-slide")) {
                twSlidePick.restart()
                return
            }
            selfTestTableWalkPress.restart()
        }
    }
    // ---- swap scenario: Table live -> Bible live -> off air (VGR_TABLEWALK_QUERY="live:swap:...") ----
    property double twSwapBeat: 0
    property int twSwapTries: 0
    Timer {
        id: twSwapHeartbeat
        interval: 250
        repeat: true
        running: window.twSwap
        onTriggered: {
            const now = Date.now()
            if (window.twSwapBeat > 0 && now - window.twSwapBeat > 1500)
                console.log("[SELFTEST-SWAP] GUI GAP " + (now - window.twSwapBeat) + " ms")
            window.twSwapBeat = now
        }
    }
    Timer {
        id: twSwapOpen
        interval: 2000
        onTriggered: {
            console.log("[SELFTEST-SWAP] table live settled; opening a Bible pick")
            quickSearch.openSearch()
            quickSearch.query = "Genesis 1:2"
            quickSearch.refresh()
            window.twSwapTries = 0
            twSwapPick.restart()
        }
    }
    Timer {
        id: twSwapPick
        interval: 2500
        repeat: true
        onTriggered: {
            const r = quickSearch.results.find((result) => result.kind === "bible")
            if (!r && ++window.twSwapTries < 10)
                return
            twSwapPick.stop()
            if (!r) {
                console.log("[SELFTEST-SWAP] FAIL no bible result")
                SelfTest.quit()
                return
            }
            console.log("[SELFTEST-SWAP] choosing bible result " + JSON.stringify(String(r.title)))
            quickSearch.choose(r)
            twSwapPlay.restart()
        }
    }
    Timer {
        id: twSwapPlay
        interval: 6000
        onTriggered: {
            const pane = SelfTest.findItem("selfTestScripturePane")
            console.log("[SELFTEST-SWAP] bible pane=" + (pane ? "yes" : "no")
                        + " selected=" + (pane ? JSON.stringify(pane.selected) : "[]"))
            if (pane)
                pane.playPicked()
            twSwapStop.restart()
        }
    }
    Timer {
        id: twSwapStop
        interval: 6000
        onTriggered: {
            console.log("[SELFTEST-SWAP] bible on air; going off air now (live=" + LiveOutputService.live + ")")
            const t0 = Date.now()
            LiveOutputService.stop()
            console.log("[SELFTEST-SWAP] stop() returned after " + (Date.now() - t0) + " ms")
            twSwapDone.restart()
        }
    }
    Timer {
        id: twSwapDone
        interval: 45000
        onTriggered: {
            console.log("[SELFTEST-SWAP] done")
            SelfTest.quit()
        }
    }
    Timer {
        id: twSlidePick
        interval: 3000
        onTriggered: {
            const pane = window.twPane()
            if (pane) {
                EngineBridge.log("info", "TWGRAB", "phase=slidepick")
                pane.stageSubpart(5, 0)
            }
            twSlideDone.restart()
        }
    }
    Timer {
        id: twSlideDone
        interval: 4000
        onTriggered: {
            EngineBridge.log("info", "TWGRAB", "phase=slidepickdone")
            window.setReadingScrollMode(true, "tablewalk")
            selfTestTableWalkPress.restart()
        }
    }
    function twGrab(phase) {
        // (engine.log marker: an outside tool photographs the NDI output at this same moment)
        EngineBridge.log("info", "TWGRAB", "phase=" + phase)
        const count = OutputListModel.rowCount()
        for (let i = 0; i < count; ++i) {
            const ok = SelfTest.grab("selfTestMonitorTile" + i, "tw_" + phase + "_" + i + ".png")
            console.log("[SELFTEST-TABLEWALK] grab phase=" + phase + " tile=" + i + " ok=" + ok)
        }
    }
    Timer {
        id: selfTestTableWalkJump
        interval: 2500
        repeat: true
        onTriggered: {
            const pane = window.twPane()
            if (!pane) {
                console.log("[SELFTEST-TABLEWALK] FAIL no-pane-at-jump")
                SelfTest.quit()
                return
            }
            const n = (window.twJump === undefined) ? 0 : window.twJump
            window.twJump = n + 1
            if (n > 0) {
                // Photograph the state the previous tick staged.
                console.log("[SELFTEST-TABLEWALK] jump=" + n
                            + " stageSerial=" + LiveOutputService.readingStageSerial
                            + " page=" + LiveOutputService.readingPage
                            + " ref=" + JSON.stringify(String(pane.referenceText)))
                window.twGrab("jump" + n)
            }
            if (n <= 2) {
                if (n === 2)
                    pane.playPicked()        // the double-click's own body
                else
                    pane.stepAndPlay(1)      // the ‹ › arrow's own body
            } else {
                selfTestTableWalkJump.stop()
                selfTestTableWalkMode.restart()
            }
        }
    }
    Timer {
        id: selfTestTableWalkMode
        interval: 2000
        repeat: true
        property int step: 0
        onTriggered: {
            const raw = (function () {
                const staged = LiveOutputService.stagedSlideRaw
                if (staged && Object.keys(staged).length > 0)
                    return staged
                return LiveOutputService.onAirSlideRaw || {}
            })()
            const flow = (raw && raw.flowText !== undefined) ? String(raw.flowText) : ""
            const step = selfTestTableWalkMode.step
            if (step === 1 || step === 3) {
                console.log("[SELFTEST-TABLEWALK] mode="
                            + (LiveOutputService.scrollMode ? "scroll" : "slide")
                            + " step=" + step
                            + " slideFlow=" + flow.length
                            + " readingFlow=" + String(LiveOutputService.readingFlowText ?? "").length)
            }
            if (step === 0) {
                window.setReadingScrollMode(false, "tablewalk-mode")
                selfTestTableWalkMode.step = 1
                return
            }
            if (step === 1) {
                window.setReadingScrollMode(true, "tablewalk-mode")
                selfTestTableWalkMode.step = 2
                return
            }
            if (step === 2) {
                // The Scroll photograph is step 3's, one beat after the restage.
                selfTestTableWalkMode.step = 3
                return
            }
            selfTestTableWalkMode.stop()
            selfTestTableWalkDone.restart()
        }
    }
    Timer {
        id: selfTestTableWalkDone
        interval: 1500
        onTriggered: {
            console.log("[SELFTEST-TABLEWALK] done presses=" + window.twPress)
            SelfTest.quit()
        }
    }
    function twStyleView(styleId) {
        const starts = LiveOutputService.readingRunLineStarts(styleId)
        const flow = String(LiveOutputService.readingFlowText ?? "")
        const ch = LiveOutputService.readingChar
        if (starts.length === 0 || !(ch >= 0))
            return { lines: 0, line: -1, char: -1, pchar: -1, span: 0, words: "" }
        const line = LiveOutputService.readingLineForStyle(styleId, ch)
        const idx = Math.max(0, Math.min(starts.length - 1, Math.floor(line)))
        const at = Number(starts[idx])
        // where this run's text begins in the pick (both terms are the reference
        // grid's own numbers, so their difference is the run's origin)
        const base = LiveOutputService.readingPassageChar() - LiveOutputService.readingCharOffset()
        let span = 0
        for (let i = 0; i + 1 < starts.length; ++i)
            span = Math.max(span, Number(starts[i + 1]) - Number(starts[i]))
        return { lines: starts.length, line: line, char: at,
                 pchar: (base >= 0 ? base + at : -1), span: span,
                 words: flow.slice(at, at + 30) }
    }
    function twTileLines(press) {
        const wall = SelfTest.findItem("selfTestMonitorWall")
        if (!wall)
            return ["[SELFTEST-TABLEWALK] press=" + press + " wall=missing"]
        const stagedRaw = LiveOutputService.stagedSlideRaw
        const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
            ? stagedRaw : LiveOutputService.onAirSlideRaw
        const flow = String(LiveOutputService.readingFlowText ?? "")
        const out = []
        const count = OutputListModel.rowCount()
        for (let i = 0; i < count; ++i) {
            const tile = window.bsFindTile(wall, i)
            if (!tile) {
                out.push("[SELFTEST-TABLEWALK] press=" + press + " tile=" + i + " missing=yes")
                continue
            }
            const fresh = LiveOutputService.previewWithStyle(
                (raw && typeof raw === "object") ? raw : {}, String(tile.styleId))
            const tileText = window.lsCompText(tile.onAirSlide)
            const freshText = window.lsCompText(fresh)
            const view = window.twStyleView(String(tile.styleId))
            out.push("[SELFTEST-TABLEWALK] press=" + press + " tile=" + i
                     + " stale=" + (window.lsCompSig(tile.onAirSlide) !== window.lsCompSig(fresh))
                     + " flow=" + (tileText !== "" && flow.indexOf(tileText) >= 0)
                     + " tileText=" + JSON.stringify(tileText.slice(0, 28))
                     + " freshText=" + JSON.stringify(freshText.slice(0, 28))
                     + " style=" + String(tile.styleId).slice(0, 8)
                     + " glines=" + view.lines
                     + " drawn=" + Number(tile.readingDrawnLines) + " at=" + String(tile.readingDrawnAt)
                     + " expected=" + Number(tile.readingExpectedLines)
                     + " gline=" + Number(view.line).toFixed(2)
                     + " gchar=" + view.char
                     + " gpchar=" + view.pchar
                     + " gspan=" + view.span
                     + " gwords=" + JSON.stringify(view.words))
        }
        return out
    }

    // ---- Popup-modal fit self-test (VGR_MODALFIT_TEST=1 only) ----------------
    function modalFitCard(rootName) {
        const named = SelfTest.findItem(rootName + "_card")   // ModalCard names its own card
        if (named)
            return named
        const root = SelfTest.findItem(rootName)
        if (!root || root.width === undefined)
            return null
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
        const r = modalFitRect(root)
        const smaller = r.w < window.width * 0.98 || r.h < window.height * 0.98
        return smaller ? root : null
    }
    function modalFitRect(item) {
        const p0 = item.mapToItem(null, 0, 0)
        const p1 = item.mapToItem(null, item.width, item.height)
        return { x: p0.x, y: p0.y, w: p1.x - p0.x, h: p1.y - p0.y }
    }
    function reportUiScales() {
        const rows = []
        const add = (name, item) => rows.push(name + "=" + (item && item.scale !== undefined ? item.scale.toFixed(3) : "?"))
        // The layers that carry the design canvas itself.
        add("screens", showScreen)
        add("edit", editScreen)
        add("header", appHeader)
        add("menubar", appMenuBar)
        add("drag", dragLayerItem)
        add("settings", settingsShell)
        add("quicksearch", SelfTest.findItem("selfTestQuickSearch_card"))
        add("import", SelfTest.findItem("selfTestImportDialog_card"))
        add("newshow", SelfTest.findItem("selfTestNewShowDialog_card"))
        console.log("[SELFTEST-UISCALE] uiScale=" + window.uiScale.toFixed(3) + " " + rows.join(" "))
    }
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
    function reportScrims() {
        reportScrim("settings", settingsScrim)
        reportScrim("quicksearch", quickSearch)
        reportScrim("import", importDialog)
        reportScrim("newshow", newShowDialog)
    }

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
                    + " blank=" + (contentH > 1 && flickH < 1)
                    + " kids=[" + out.join(" ") + "]")
    }
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

    function reportToastDedup() {
        const items = NotificationCenter.items
        const cards = items.length
        const count = cards === 1 ? (items[0].groupCount || 1) : 0
        console.log("[SELFTEST-TOASTDEDUP] pushed=6 cards=" + cards
                    + " groupCount=" + count
                    + " dedupe=" + (cards === 1 && count === 6))
    }
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

    readonly property var modalFitSweep: [
        {
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
            label: "av-source-drop-open", card: "",
            open: function () { SelfTest.clickItem("selfTestAvAddVideo") },
            close: function () { }
        },
        {
            label: "av-source-drop", card: "selfTestAddSourceSourceField_menu",
            scrim: "selfTestAddSourceDialog",
            open: function () { SelfTest.clickItem("selfTestAddSourceSourceField_box") },
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
            label: "toast", card: "",
            open: function () {
                const text = "Not connected this time: WindowsClipboard.cpp - WindowsUI - Visual Studio Code — the inputs that use them read no signal until “Camera 1” is back (Settings · Audio & Video)."
                const crux = ["WindowsClipboard.cpp - WindowsUI - Visual Studio Code"]
                window.toastActionRan = false
                NotificationCenter.registerAction("ui.selftest.toastAction",
                                                   function() { window.toastActionRan = true })
                const action = { id: "ui.selftest.toastAction", label: qsTr("Open recovered show") }
                for (let i = 0; i < 6; ++i)
                    EventBus.notify(text, "warning", qsTr("Devices"), "ui.selftest.toast", crux, action)
            },
            close: function () { NotificationCenter.clear() },
            report: function () { reportToast(); reportToastDedup(); reportToastCrux(); reportToastAction() }
        },
        {
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
                if (BusListModel.rowCount() === 0) {
                    BusListModel.addBus("SelfTest Pill Bus", "audio")
                    window.pillAddedBus = BusListModel.rowCount() - 1
                }
                window.pillRow = BusListModel.rowCount() - 1
                {
                    const pane = SelfTest.findItem("selfTestMediaPane")
                    if (pane) {
                        try {
                            const pillBusId = Number(BusListModel.getBus(window.pillRow).id)
                            pane.toggleBusMonitorPreview(window.pillRow)
                            const opened = pane.busMonitorAudioBusId === pillBusId
                            const taps = pane.meteredLabels.length
                            pane.toggleBusMonitorPreview(window.pillRow)
                            console.log("[SELFTEST-PILL] busmon bus=" + window.pillRow
                                        + " id=" + pillBusId
                                        + " opened=" + opened + " audioTaps=" + taps
                                        + " released=" + (pane.busMonitorAudioBusId === -1))
                        } catch (e) {
                            console.log("[SELFTEST-PILL] busmon bus=" + window.pillRow
                                        + " threw: " + e)
                        }
                    }
                }
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
                    window.pillStep = 1
                    return
                }
                if (window.pillBy === "click") {
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
    function newBusCheckLights() {
        let state = ""
        window.newBusLightChecked = 0
        window.newBusLightWrong = 0
        const read = (kind, row, label, sourceKind, name) => {
            const card = SelfTest.findItem("newBusPicker_row_" + kind + "_" + row)
            const light = card ? String(card.lightState) : "missing"
            const alive = LiveOutputService.inputAvailable(label, sourceKind)
            const starved = LiveOutputService.inputDelivery(label, sourceKind) === "starved"
            const routed = card ? card.alreadyFed === true : false
            const wanted = label === "" ? "none"
                         : (!alive ? "offline" : (starved ? "starved" : "connected"))
            const formOk = card ? card.lightRouted === routed : false
            window.newBusLightChecked++
            if (light !== wanted || !formOk)
                window.newBusLightWrong++
            state += (state === "" ? "" : " ") + name + "=" + light
                     + (light === wanted ? "" : "(wanted " + wanted + ")")
                     + (formOk ? "" : "(form: routed=" + routed + ")")
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
            window.newBusEditPicker = !SelfTest.itemVisible("editBusPicker_card")
                                      && BusListModel.rowCount() === window.newBusBefore
            console.log("[SELFTEST-NEWBUS] editRing noCreate=" + window.newBusEditPicker)
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
    property int busLightStep: 0
    property int busLightTries: 0
    property var busLightLines: []
    property int busLightGreen: 0
    property int busLightRouted: 0
    property var busLightHold: ({ bus: -1, label: "", kind: "" })
    property var busLightCandidates: []
    property string busLightDelivery: ""
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
        const videoWanted = window.busLightDelivery === "starved" ? "starved" : "connected"
        const videoConsistent = window.busLightHold.bus < 0
            || window.busLightHoldStatus === videoWanted
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
            if (++window.busLightTries < 3)
                return
            window.busLightTries = 0
            if (window.busLightStep === 2) {
                window.busLightReadDelivery()
                return
            }
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
                if (routedVideo > 0 && video !== "")
                    window.busLightCandidates.push({ bus: i, busId: Number(b && b.id !== undefined ? b.id : -1),
                                                     label: video, kind: vkind })
                window.busLightLines.push("bus=" + i + " name=\"" + name + "\" type=" + type
                    + " routed=" + routed + " status=" + status
                    + " video=\"" + video + "\"(" + vkind + ")"
                    + " videoAlive=" + (video !== ""
                        ? LiveOutputService.inputHealthy(video, vkind) : "n/a")
                    + " audio=\"" + audioLabel + "\"(" + audioKind + ")"
                    + " audioAlive=" + (routedAudio > 0
                        ? LiveOutputService.inputHealthy(audioLabel, "audio") : "n/a"))
            }
            const ghostKind = "screen"
            const ghost = "selftest-buslight-ghost-window"
            const ghostAlive = LiveOutputService.inputAvailable(ghost, ghostKind)
            const bus1Video = String((SelfTest.findItem("selfTestBusCard_1") || {}).busVideoLabel || "")
            const wrongKindAlive = bus1Video !== ""
                ? LiveOutputService.inputHealthy(bus1Video, "window") : "n/a"
            window.busLightLines.push("ping label=\"" + ghost + "\" kind=" + ghostKind
                + " alive=" + ghostAlive)
            window.busLightLines.push("wrongkind label=\"" + bus1Video
                + "\" kind=window alive=" + wrongKindAlive)
            // ---- THE LIVE HALF, MEASURED ON A REAL TAP ----------------------
            const pane = SelfTest.findItem("selfTestMediaPane")
            if (pane) {
                const wants = window.busLightCandidates.slice()
                wants.sort((a, b) => (a.kind === "camera" ? 0 : 1) - (b.kind === "camera" ? 0 : 1))
                for (let c = 0; c < wants.length; ++c) {
                    const cand = wants[c]
                    try {
                        pane.toggleBusMonitorPreview(cand.bus)
                        if (Number(pane.busMonitorVideoBusId) !== cand.busId)
                            pane.toggleBusMonitorPreview(cand.bus)   // the pane's own undo
                        if (Number(pane.busMonitorVideoBusId) === cand.busId) {
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
        const heldCards = LiveOutputService.cardPreviews.indexOf(held.label) >= 0
        const devId = String(EngineBridge.resolvedPreviewDeviceId(held.label) || "")
        const producing = LiveOutputService.inputProducing(held.label)
        const failure = String(LiveOutputService.inputFailure(held.label, held.kind) || "")
        window.busLightLines.push("delivery label=\"" + held.label + "\" kind=" + held.kind
            + " verdict=" + (delivery === "" ? "<none>" : delivery)
            + " producing=" + producing
            + " failure=\"" + failure + "\""
            + " cardPreviews=" + heldCards + " devId=\"" + devId + "\""
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
    // Real files with real audio, longest first: each OS's own stock sounds, which every machine of
    // that OS has, and the platform's media decoder reads a WAV/AIFF with no codec pack.
    readonly property var mediaMeterFiles: [
        "C:/Windows/Media/Ring05.wav",
        "C:/Windows/Media/Ring08.wav",
        "C:/Windows/Media/Ring09.wav",
        "C:/Windows/Media/Alarm05.wav",
        "C:/Windows/Media/Alarm06.wav",
        "C:/Windows/Media/Alarm01.wav",
        // ...and the same on the other platforms: macOS's system sounds, and the sample ALSA ships.
        "/System/Library/Sounds/Glass.aiff",
        "/System/Library/Sounds/Ping.aiff",
        "/usr/share/sounds/alsa/Front_Center.wav",
        "/usr/share/sounds/freedesktop/stereo/complete.oga"
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
    property real openTestT0: 0
    Timer {
        id: selfTestOpenDialog
        running: typeof SelfTestOpen !== "undefined" && SelfTestOpen
        interval: 45000
        onTriggered: {
            const t0 = Date.now()
            quickSearch.openSearch()
            console.log("[SELFTEST-OPEN] dialog open=" + (Date.now() - t0) + "ms")
            selfTestOpenQuery.restart()
        }
    }
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
    readonly property string exactProbeWord: "frien"
    readonly property string exactRealWord: "friend"
    property int exactFilteredLocked: -1
    property string exactToggleBy: ""
    function exactSermonRows() {
        return quickSearch.results.filter((r) => r.kind === "table")
    }
    function exactPhraseRe() {
        const words = String(quickSearch.query).trim().toLowerCase().split(/\s+/).filter((w) => w !== "")
        const esc = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        return new RegExp("\\b" + esc.join("[\\s,;:.!?“”'\\-—]+\\b") + "\\b", "i")
    }
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
                SelfTest.clickItem("selfTestSearchExactLock")
                window.exactPhase = 1
                window.exactTries = 0
                return
            }
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
                window.exactPhase = 4
                window.exactTries = 0
                quickSearch.query = window.exactTypoWord
                quickSearch.refresh()
                return
            }
            if (window.exactPhase === 4 && !window.exactDone2 && window.exactTries >= 2) {
                window.exactDone2 = true
                const h = SelfTest.findItem("selfTestSearchHint")
                window.exactHint = h && h.text !== undefined ? String(h.text) : "(no hint item)"
                console.log("[SELFTEST-EXACT] hint locked=" + (quickSearch.exact === true)
                            + " text=" + JSON.stringify(window.exactHint))
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
    readonly property string scopeWord: "faith"
    readonly property string scopeScriptureWord: "beginning"
    property var scopeLoose: ({ rows: -1, outside: -1, places: -1 })
    property var scopeLocked: ({ rows: -1, outside: -1, places: -1 })
    property string scopeBy: ""
    property string scopeOpen: ""
    property var scopeScriptureLoose: ({ rows: -1, outside: -1, places: -1 })
    property var scopeScriptureLocked: ({ rows: -1, outside: -1, places: -1 })
    property string scopeScriptureBy: ""
    property string scopeScriptureOpen: ""
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
    property var preSplitOpenRow: null
    property bool preSplitRowReady: false
    property var preSplitFacts: ({ rows: -1, split: -1, hadPick: false, openMs: -1 })
    property var preSplitRestore: ({})
    property double preSplitOpenAt: 0
    property int preSplitTries: 0
    property int preSplitClickTries: 0
    property int preSplitWaitTicks: 0
    function preSplitRowName(row) {
        if (!row)
            return ""
        return "selfTestVerseRow_" + Number(row.number)
                + (row.continuation === true ? "-" + Number(row.partIndex) : "")
    }
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
            p.openBook(p.bookIndex === 0 ? 1 : 0, 1)
            selfTestPreSplitPoll.restart()
        }
    }
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
            const splitNow = p.mainRows.filter((r) => r.continuation === true).length
            if (window.preSplitFacts.openMs < 0) {
                window.preSplitFacts.openMs = Date.now() - window.preSplitOpenAt
                window.preSplitFacts.openSplit = splitNow
            }
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
            window.preSplitRowReady = false
            p.revealRow(p.mainRows.findIndex((r) => Number(r.number) === Number(split[0].number)
                                               && Number(r.partIndex ?? 0) === Number(split[0].partIndex ?? 0)))
            SelfTest.activate()
            selfTestPreSplitClicked.restart()
        }
    }
    // ---- THE LAYOUT PROBE SCENARIO (VGR_LAYOUT_TEST, tools/selftest/run_layout_probe.sh) ----
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
               + ((window.layoutProbePendingStep && window.layoutProbePendingStep.scroll !== undefined)
                  ? window.layoutProbeScrollFields() : "")
               + window.layoutProbeScrolledFields(prev, now, window.layoutProbePendingStep)
    }

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
    property var scrollStep: null
    property bool scrollFound: false
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

    function layoutProbeFlushScroll() {
        if (window.scrollStep === null)
            return
        window.scrollSeconds = selfTestScrollMeter.elapsedTime
        window.scrollWallMs = window.scrollT0 > 0 ? Date.now() - window.scrollT0 : 0
        window.scrollLogArmed = false
        window.scrollArmed = false
        window.layoutProbeLogScroll()
    }

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
        selfTestScrollAnim.stop()
        selfTestScrollAnim.target = item
        selfTestScrollAnim.from = window.scrollStartY
        selfTestScrollAnim.to = maxY
        window.scrollArmed = true
        window.scrollLogArmed = true
        selfTestScrollAnim.start()
    }

    function layoutProbeScrollFields() {
        const item = window.scrollFound ? SelfTest.findItem(window.scrollListName) : null
        const contentPx = item ? Math.round(item.contentHeight) : -1
        const viewPx = item ? Math.round(item.height) : -1
        const scrolled = item ? Math.round(item.contentY - window.scrollStartY) : 0
        const seconds = window.scrollSeconds
        const fps = (seconds > 0 && window.scrollFrames > 0) ? (window.scrollFrames / seconds) : 0
        const avg = window.scrollFrames > 0 ? (window.scrollMs / window.scrollFrames) : 0
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
        if (window.layoutProbePendingStep === step)
            window.scrollStep = null
    }
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
            if (window.layoutProbePending !== "" && !wasScroll)
                EngineBridge.log("info", "LayoutProbe",
                                 window.layoutProbeLine(window.layoutProbePending,
                                                        Date.now() - window.layoutProbeT0))
            else if (wasScroll) {
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

    Timer {
        id: selfTestPreSplitClicked
        interval: 250
        repeat: true
        onTriggered: {
            const p = SelfTest.findItem("selfTestTablePane")
            const row = window.preSplitOpenRow
            const name = window.preSplitRowName(row)
            if (!window.preSplitRowReady) {
                if (!SelfTest.itemVisible(name)) {
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
    readonly property string rankQuery: "compelling that you recieve"
    // The diagnostic's own async search request counter (see the read timer below).
    property int diagSearchToken: 0
    // The hover stage's own state (see rankStartHover): which row the real cursor
    // was parked on, and what its preview read.
    property var rankHover: ({ index: -1, kind: "-", chars: -1 })
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
            console.log("[SELFTEST-RANK] diag query='" + quickSearch.query + "' pending="
                        + quickSearch.searchPending + " bibleLoading=" + SearchService.bibleLoading
                        + " bibles=" + SearchService.bibles.length)
            for (let i = 0; i < rows.length; ++i)
                console.log("[SELFTEST-RANK] row" + i + "=" + rows[i])
            const first = quickSearch.results.length > 0 ? quickSearch.results[0] : null
            console.log("[SELFTEST-RANK] result top=" + (first ? first.kind : "-")
                        + " topTitle=" + (first ? String(first.title).substring(0, 60) : "-"))
            window.rankStartHover()
        }
    }
    function rankPickRow(wantBible) {
        const results = quickSearch.results
        for (let i = 0; i < results.length; ++i) {
            const r = results[i]
            const located = (r.kind === "bible" || r.kind === "table")
                             && r.bookId !== undefined && r.bookId !== ""
            if (located && (!wantBible || r.kind === "bible"))
                return i
        }
        return -1
    }
    function rankStartHover() {
        let pick = window.rankPickRow(true)
        if (pick < 0 && !window.rankHoverResearched) {
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
    property var showIo: ({ savePath: "showio_probe\u2019s.vgr", copyPath: "showio_probe\u2019s_recovery.vgr",
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
    property var quitProbe: ({ seedToken: 0, seeded: false, armed: false, refused: false,
                               dialogUp: false, saveClicked: false, live: false })
    function quitLog(step, text) { console.log("[SELFTEST-QUIT] step=" + step + " " + text) }
    function quitBegin() {
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
        window.quitProbe.saveClicked = SelfTest.itemVisible("unsavedChangesDialog_save")
        window.quitLog("answer", "how=click saveButton=" + window.quitProbe.saveClicked
                       + " saving=" + unsavedDialog.saving)
        SelfTest.clickItem("unsavedChangesDialog_save")
        selfTestQuitAnswered.restart()
    }
    function quitAnswered() {
        if (unsavedDialog.shown) {
            window.quitLog("fallback", "how=confirm (the click did not land) saving=" + unsavedDialog.saving)
            unsavedDialog.confirm()
            selfTestQuitConfirmed.restart()
            return
        }
        window.quitLog("closed", "the dialog is gone — the answer's save landed and closed the window")
    }
    function quitConfirmed() {
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
    property var liveLat: ({ step: 0, firstFrameMs: -1 })
    function liveLatLog(step, text) { console.log("[SELFTEST-LIVELAT] step=" + step + " " + text) }
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
        while (st.added.length > 0 && window.liveLatStyledRows() > want)
            window.liveLatDropAdded()
        while (want > 1 && window.liveLatStyledRows() < want && st.added.length < 8) {
            const src = window.liveLatStyleSource()
            if (src < 0)
                break   // no styled row to copy: an added row would render nothing
            OutputListModel.duplicateOutput(src)
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
        while (st.added.length > 0)
            window.liveLatDropAdded()
        while (OutputListModel.rowCount() > st.baseRows)   // belt and braces
            OutputListModel.removeOutput(OutputListModel.rowCount() - 1)
        window.liveLatLog("restored", "rows=" + OutputListModel.rowCount()
                          + " styled=" + window.liveLatStyledRows())
    }
    function liveLatRun() {
        const st = window.liveLat
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

    // ---- PICK LATENCY: a verse pick, measured to the tile ----------------------
    property int plNumber: -1
    property int plPart: 0
    property string plNeed: ""
    property real plT0: 0
    function plLandedOn(need) {
        const tile = SelfTest.findItem("selfTestMonitorTile0")
        if (tile && window.lsCompText(tile.onAirSlide).indexOf(need) >= 0)
            return "tile"
        if (window.lsCompText(LiveOutputService.stagedSlide).indexOf(need) >= 0)
            return "service"
        return ""
    }
    function plTileText() {
        const tile = SelfTest.findItem("selfTestMonitorTile0")
        const fromTile = tile ? window.lsCompText(tile.onAirSlide) : ""
        const fromService = window.lsCompText(LiveOutputService.stagedSlide)
        return fromTile.length > 0 ? fromTile : fromService
    }
    Timer {
        id: selfTestPickLat
        running: typeof SelfTestPickLat !== "undefined" && SelfTestPickLat
        interval: 22000      // boot, the Bible restore and the sermon indexing
        onTriggered: {
            window.currentView = "show"
            showScreen.showInLibrary("scripture", "")
            selfTestPickLatReady.restart()
        }
    }
    Timer {
        id: selfTestPickLatReady
        interval: 8000       // the pane's own reads land while the tab comes up
        onTriggered: {
            const p = SelfTest.findItem("selfTestScripturePane")
            const verses = p ? (p.chapterVerses || []) : []
            if (!p || verses.length < 2) {
                console.log("[SELFTEST-PICKLAT] FAIL no-pane pane=" + (p ? "yes" : "no")
                            + " verses=" + verses.length)
                SelfTest.quit()
                return
            }
            // The SECOND verse: the first is what a fresh tab already has staged,
            // and a pick that changes nothing measures nothing.
            const v = verses[1]
            window.plNumber = Number(v.number ?? 0)
            window.plPart = Number(v.partIndex ?? 0)
            window.plNeed = String(v.text ?? "").trim().slice(0, 24)
            if (window.plNeed === "") {
                console.log("[SELFTEST-PICKLAT] FAIL no-text verse=" + window.plNumber)
                SelfTest.quit()
                return
            }
            const t0 = Date.now()
            const pickOk = p.stageOutputPart(window.plNumber, window.plPart)
            const syncMs = Date.now() - t0
            window.plT0 = t0
            const hit = window.plLandedOn(window.plNeed)
            console.log("[SELFTEST-PICKLAT] kind=scripture verse=" + window.plNumber
                        + " syncMs=" + syncMs + " pickOk=" + pickOk
                        + " landedOn=" + (hit === "" ? "none" : hit)
                        + " tile=" + (SelfTest.findItem("selfTestMonitorTile0") ? "yes" : "missing")
                        + " staged='" + String(LiveOutputService.stagedName ?? "") + "'"
                        + " paneRef=" + JSON.stringify(String(p.referenceText ?? ""))
                        + " allows=" + OutputListModel.activeStyleAllows("scripture")
                        + " need=" + JSON.stringify(window.plNeed))
            if (hit !== "") {
                console.log("[SELFTEST-PICKLAT] tileMs=" + (Date.now() - window.plT0)
                            + " (the tile already held it when the pick returned)")
                selfTestPickLatQuit.restart()   // ...the poll owns the quit otherwise
            } else {
                selfTestPickLatPoll.restart()
            }
        }
    }
    Timer {
        id: selfTestPickLatPoll
        interval: 8
        repeat: true
        onTriggered: {
            const waited = Date.now() - window.plT0
            const landed = window.plLandedOn(window.plNeed)
            const got = window.plTileText()
            if (landed !== "") {
                selfTestPickLatPoll.stop()
                console.log("[SELFTEST-PICKLAT] tileMs=" + waited + " landedOn=" + landed
                            + " text=" + JSON.stringify(got.slice(0, 32)))
                selfTestPickLatQuit.restart()
            } else if (waited > 5000) {
                selfTestPickLatPoll.stop()
                console.log("[SELFTEST-PICKLAT] FAIL no-tile after " + waited
                            + " ms text=" + JSON.stringify(got.slice(0, 32)))
                selfTestPickLatQuit.restart()
            }
        }
    }
    Timer {
        id: selfTestPickLatQuit
        interval: 600
        onTriggered: {
            console.log("[SELFTEST-PICKLAT] done")
            SelfTest.quit()
        }
    }

    // ---- The Table pane's own search box: highlight + hit placement -----------
    readonly property string paneQuery: "compelling that"
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

    // ---- Quiet Time pick -> the WALL'S OWN PIXELS (VGR_QUIETTIME_TEST=1) ------
    // The C++ scenario stages the pick and presses GO LIVE; this side photographs what the wall
    // actually drew afterwards, with the app's own grab (SelfTest.grab, the same one the tablewalk
    // uses), and prints each tile's own facts beside its picture. "The lower third went black on
    // GO LIVE" then reads as pixels plus the reason, instead of being inferred from a map.
    function quietTimeFacts(i) {
        const t = SelfTest.findItem("selfTestMonitorTile" + i)
        if (!t)
            return "tile=" + i + " facts=<missing>"
        const s = t.onAirSlide || {}
        return "tile=" + i
            + " name=" + t.name
            + " slide=" + t.hasSlidePreview
            + " blocks=" + ((s.blocks && s.blocks.length) || 0)
            + " engineComposed=" + t.engineComposed
            + " framePriority=" + (s.framePriority === true)
            + " frameClear=" + t.frameClear
            + " cleared=" + (s.cleared === true)
            + " category='" + (s.showCategory || "") + "'"
            + " enabled=" + ((OutputListModel.getOutput(i) || {}).isEnabled === true)
    }
    Timer {
        id: selfTestQuietTimeGrab
        running: typeof SelfTestQuietTime !== "undefined" && SelfTestQuietTime
        interval: 14000   // boot + the C++ scenario's stage/GO LIVE, then a settled frame
        onTriggered: {
            const count = OutputListModel.rowCount()
            for (let i = 0; i < count; ++i) {
                const ok = SelfTest.grab("selfTestMonitorTile" + i, "qt_tile_" + i + ".png")
                console.log("[SELFTEST-QUIETTIME] grabbed ok=" + ok
                            + " live=" + LiveOutputService.live
                            + " onAir='" + LiveOutputService.onAirTitle + "'"
                            + " " + window.quietTimeFacts(i))
            }
            selfTestQuietTimeDone.restart()
        }
    }
    Timer {
        id: selfTestQuietTimeDone
        interval: 2500    // grabToImage renders asynchronously; quit() would destroy the picture
        onTriggered: SelfTest.quit()
    }

    // ---- The tab search's SEED: does the open sermon fill the box, and do the
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
            window.seedEditStep = 0
            selfTestSeedEdit.restart()
        }
    }
    function seedStagedTitle() {
        const staged = LiveOutputService.stagedSlide
        return staged && staged.title !== undefined ? String(staged.title) : ""
    }
    // ---- editing the box with the keyboard: the caret keys and Backspace must act on the text ----
    property int seedEditStep: 0
    function seedEditLog(tag) {
        const f = SelfTest.findItem("selfTestSearchInput")
        const box = SelfTest.findItem("selfTestSearchBox")
        console.log("[SELFTEST-SEEDEDIT] " + tag + " text='" + (f ? f.text : "-") + "' caret=" + (f ? f.cursorPosition : -1)
                    + " sel='" + (f ? f.selectedText : "-") + "' len=" + (f ? f.text.length : -1) + " boxW=" + (box ? Math.round(box.width) : -1)
                    + " focus=" + (f ? f.activeFocus : "-"))
    }
    Timer {
        id: selfTestSeedEdit
        interval: 900
        onTriggered: {
            const step = window.seedEditStep++
            if (step === 0) {
                window.seedEditLog("start")
                SelfTest.type("←←←")
            } else if (step === 1) {
                window.seedEditLog("after-3-left")
                SelfTest.type("")
            } else if (step === 2) {
                window.seedEditLog("after-backspace")
            } else if (step === 3) {
                window.seedEditLog("after-backspace+settle")
                SelfTest.grab("selfTestLibraryTabBar", "seed_edit.png")
                SelfTest.type("→→→→→→")
            } else if (step === 4) {
                window.seedEditLog("after-6-right")
                const f = SelfTest.findItem("selfTestSearchInput")
                let erase = ""
                for (let i = 0; i < (f ? f.text.length : 0) + 2; ++i)
                    erase += ""
                SelfTest.type(erase)
            } else if (step === 5) {
                window.seedEditLog("after-erase-all")
            } else if (step < 14) {
                if (step === 6) showScreen.showInLibrary("scripture", "")
                if (step === 7) { SelfTest.focusItem("selfTestSearchInput"); SelfTest.type(""); SelfTest.type("1joh") }
                if (step === 9) SelfTest.type("
")
                if (step === 10) SelfTest.type("1joh")
                if (step === 11) SelfTest.type("n")
                if (step === 12) SelfTest.type("2")
                window.seedEditLog("erased+" + ((step - 5) * 0.9).toFixed(1) + "s")
            } else {
                window.seedEditLog("final")
                selfTestSeedEdit.stop()
                selfTestSeedClick.restart()
                return
            }
            selfTestSeedEdit.restart()
        }
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
    function paneRowsLastRow(which, rows) {
        const name = which === "match" ? "selfTestMatchRow_" + (rows - 1)
                   : which === "hit" ? "selfTestHitRow_" + (rows - 1)
                                      : "selfTestPeekRow_" + (rows - 1)
        return SelfTest.itemVisible(name)
    }
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
    Timer {
        id: selfTestCursorScaleC
        interval: 2200
        onTriggered: {
            settingsScrim.visible = false
            const atEdge = { x: 3, y: Math.round(window.height / 2) }
            const atCorner = { x: 3, y: 3 }
            window.cursorFrameEdge = SelfTest.cursorAt(atEdge.x, atEdge.y)
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
    property var outSaveScreen: null
    property string outSaveStyleId: ""
    property string outSaveWanted: ""
    property int outSaveStylesBefore: 0
    property string outSavePhase: "A"
    function outSaveDialogShown() {
        const d = SelfTest.findItem("selfTestEditOutputDialog")
        return d ? (d.opacity > 0 ? "true" : "false") : "?"
    }
    function outSaveBeginPass(phase, wanted) {
        window.outSavePhase = phase
        window.outSaveWanted = wanted
        window.outSaveScreen.openEdit(0)
        window.outSaveScreen.editStyleId = wanted
        selfTestOutSaveClick.restart()
    }
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
                StyleListModel.addStyle()
                console.log("[SELFTEST-OUTSAVE] add-style click missed - called addStyle() directly")
            }
            console.log("[SELFTEST-OUTSAVE] roster before=" + window.outSaveStylesBefore
                        + " after=" + StyleListModel.rowCount())
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
                const now = SettingsService.value("table.splitLongVerses") === true
                SettingsService.setValue("table.splitLongVerses", !now)
                SettingsService.setValue("table.splitLongVerses", now)
                window.uiWarnPhase = 5
                return
            }
            case 5: {
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
    function gpuToggleOnScreen() {
        const c = SelfTest.itemCenter(window.gpuToggleName)
        return c.x > 0 && c.y > 0 && c.x < window.width && c.y < window.height
    }
    function gpuNoteText() {
        const t = SelfTest.findItem("selfTestNote_" + window.gpuKey)
        return t ? String(t.text) : ""
    }
    function gpuLabelText() {
        const t = SelfTest.findItem("selfTestLabel_" + window.gpuKey)
        return t ? String(t.text) : ""
    }
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
    property int outsPhase: 0
    property int outsTries: 0
    readonly property var outsScopePhrases: [
        "Restyles Scripture and The Table",   // content families, shows keep their own
        "Lays out shows too",   // the style's Shows slot names a template
        "No layout of its own"                // background and placement only
    ]
    function outsSample() {
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
            if (status.indexOf(kind + " \u00b7 ") === 0 || status.indexOf("Live \u00b7 ") === 0)
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
        const wallTiles = []
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const tile = window.tfTile(i)
            wallTiles.push(i + ":" + (tile ? String(tile.runtimeState) : "<missing>"))
        }
        console.log("[SELFTEST-OUTSLIVE] " + tag + " tiles=" + JSON.stringify(wallTiles.join(" | ")))
    }
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
    property var bsTileItem: null
    property string bsOriginalStyleId: ""
    property bool bsPreviewSameRoster: false
    property int bsTileIndex: -1
    property int bsStyleRow: 0
    property string bsStyleId: ""
    property string bsImage: ""
    property string bsOriginalImage: ""
    function bsFindImage() {
        for (let i = 0; i < StyleListModel.rowCount(); ++i) {
            const p = StyleListModel.getStyle(i).backgroundImage
            if (p !== undefined && p !== "" && SettingsService.fileExists(p))
                return String(p)
        }
        // A picture every machine of that OS ships: the stock wallpapers (Windows, macOS, a common Linux one).
        const candidates = ["C:/Windows/Web/Wallpaper/Windows/img0.jpg",
                            "C:/Windows/Web/Wallpaper/Windows/img1.jpg",
                            "C:/Windows/Web/Screen/img100.jpg",
                            "/System/Library/Desktop Pictures/Solid Colors/Black.png",
                            "/Library/Desktop Pictures/Monterey Graphic.heic",
                            "/usr/share/backgrounds/warty-final-ubuntu.png",
                            "/usr/share/pixmaps/debian-logo.png"]
        for (const c of candidates)
            if (SettingsService.fileExists(c))
                return c
        return ""
    }
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

    function bsReport(tag) {
        const tile = window.bsTileItem
        if (tag === "cleared")
            console.log("[SELFTEST-STYLEBG] tile layers: " + window.bsLayerFacts().line)
        // THE TILE'S CURRENT PROPERTIES. This probe used to read `tile.styleBg` / `tile.styled`, which
        // the tile has not exposed since it started reading the roster itself — every step printed "?"
        // and the scenario failed its own premise. The style background is `styleBackground`, and
        // whether the tile PAINTS it is answered by the layer itself (found by name), which is the
        // thing under test: a tile that knows the image but does not draw it is the bug this catches.
        const bg = tile && tile.styleBackground ? tile.styleBackground : null
        const styleLayer = tile ? window.bsLayerFacts_find(tile, "selfTestTileStyleImage") : null
        const layerSource = styleLayer ? String(styleLayer.source) : ""
        const tileHasImage = styleLayer
            ? (styleLayer.visible === true && layerSource !== "")
            : (bg ? bg.hasImage === true : false)
        const ti = window.bsTileIndex
        const modelBg = ti >= 0 ? OutputListModel.styleBackground(ti) : null
        console.log("[SELFTEST-STYLEBG] " + tag
                    + " tile=" + (tile ? "yes" : "no")
                    + " tileIndex=" + ti
                    + " tileStyleId=" + (tile ? String(tile.styleId) : "?")
                    + " styleMatch=" + (tile ? String(String(tile.styleId) === window.bsStyleId) : "?")
                    + " tileHasImage=" + String(tileHasImage)
                    + " tileImage=" + JSON.stringify(layerSource.replace("file:///", ""))
                    + " tileColor=" + JSON.stringify(bg ? String(bg.backgroundColor) : "?")
                    + " tileStyled=" + (tile ? String(tile.styleBackgroundPaints === true) : "?")
                    + " modelHasImage=" + (modelBg ? String(modelBg.hasImage) : "?")
                    + " styleImage=" + JSON.stringify(String(StyleListModel.getStyle(window.bsStyleRow).backgroundImage)))
    }
    // ---- What each wall tile is drawing (VGR_TILEFACTS_TEST=1) -------------
    // The still the tile-facts run takes: VGR_TILEFACTS_MEDIA, else the folder's own test file (no
    // machine's personal path is baked in; without either, the media phase of the run is skipped).
    property string tfMedia: typeof SelfTestTileFactsMedia !== "undefined" ? String(SelfTestTileFactsMedia) : ""
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
            if (window.tfMedia === "")
                console.log("[SELFTEST-TILES] no VGR_TILEFACTS_MEDIA — the media phase is skipped")
            else
                LiveOutputService.takeMedia(window.tfMedia, window.tfMedia.replace(/^.*[\\/]/, ""))
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
            selfTestTileFactsQuit.start()
        }
    }
    Timer {
        id: selfTestTileFactsQuit
        interval: 1500
        onTriggered: SelfTest.quit()
    }

    // ---- Blank GO LIVE carrier + a live input (VGR_BLANKLIVE_TEST=1) --------
    property string blLabel: ""
    Timer {
        id: selfTestBlankLiveStart
        running: typeof SelfTestBlankLive !== "undefined" && SelfTestBlankLive
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
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
            const blTile = window.bsFindTile(SelfTest.findItem("selfTestMonitorWall"), 0)
            console.log("[SELFTEST-BLANKLIVE] tile layers: " + window.bsLayerFactsOf(blTile).line)
            console.log("[SELFTEST-BLANKLIVE] done live=" + LiveOutputService.live)
            LiveOutputService.clearInput()
            SelfTest.quit()
        }
    }
    // ---- An NDI output added WHILE LIVE (VGR_NDILATE_TEST=1) ----------------
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
    property bool boVisible: false
    property bool boPressed: false
    property bool boEnded: false
    property string boLabel: ""
    Timer {
        id: selfTestBlackoutStart
        running: typeof SelfTestBlackout !== "undefined" && SelfTestBlackout
        interval: 9000           // boot + first frame, like the other scenarios
        onTriggered: {
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
    Timer {
        id: selfTestBlackoutPress
        interval: 6000
        onTriggered: {
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
            if (LiveOutputService.inputLabel !== "")
                LiveOutputService.clearInput()
            console.log("[SELFTEST-BLACKOUT] done")
            SelfTest.quit()
        }
    }
    // ---- THE LIVE VIDEO PATH, MEASURED (VGR_VIDEOPERF_TEST=1) ----------------
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
    // ---- VIDEO RATE TEST (VGR_VIDEOFPS_TEST=1, file = VGR_VIDEOFPS_FILE) -----------------------
    // The integrated camera first, then the video file, each on air through the real path. Every two
    // seconds: the SOURCE's frames per second (the camera tap / the decoder's own count) next to what
    // the program loop composed, what the NDI feed sent and what each output received. The outputs
    // must keep up with the source — under load too (the runner script burns the CPUs meanwhile).
    property var vfGpuBefore: undefined
    property string vfPhase: ""
    property int vfSamples: 0
    property var vfPrev: ({})
    function vfSnapshot() {
        const snap = { t: Date.now(), tile: Number(LiveOutputService.frameRev), cam: Number(LiveOutputService.inputRev),
                       media: Number(LiveOutputService.mediaFramesDecoded()),
                       loop: Number(LiveOutputService.framesSentNow()),
                       ndi: Number(LiveOutputService.ndiFramesSent), out: ({}) }
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const o = OutputListModel.getOutput(i)
            const name = String(o && o.name !== undefined ? o.name : ("out" + i))
            const rt = LiveOutputService.outputRuntime[name] || null
            snap.out[name] = rt ? (Number(rt.frames) || 0) : 0
        }
        return snap
    }
    // Interface-thread responsiveness while the video runs: a 16 ms timer's real gaps. The audio meters
    // and every animation ride this thread, so a gap well above 16 ms IS the stutter.
    property real vfLastGui: 0
    property real vfGuiMax: 0
    property real vfGuiSum: 0
    property int vfGuiN: 0
    Timer {
        interval: 16
        repeat: true
        running: typeof SelfTestVideoFps !== "undefined" && SelfTestVideoFps
        onTriggered: {
            const t = Date.now()
            if (window.vfLastGui > 0) {
                const gap = t - window.vfLastGui
                window.vfGuiSum += gap
                window.vfGuiN += 1
                if (gap > window.vfGuiMax) window.vfGuiMax = gap
            }
            window.vfLastGui = t
        }
    }
    Timer {   // the baseline before anything is on air
        interval: 2000
        repeat: true
        running: typeof SelfTestVideoFps !== "undefined" && SelfTestVideoFps && window.vfPhase === ""
        onTriggered: {
            console.log("[SELFTEST-VIDEOFPS] phase=idle gui=" + (window.vfGuiN > 0 ? (window.vfGuiSum / window.vfGuiN).toFixed(1) : "-")
                        + "/" + window.vfGuiMax.toFixed(0) + "ms")
            window.vfGuiSum = 0; window.vfGuiN = 0; window.vfGuiMax = 0
        }
    }
    function vfReport() {
        const guiLine = " gui=" + (window.vfGuiN > 0 ? (window.vfGuiSum / window.vfGuiN).toFixed(1) : "-")
                        + "/" + window.vfGuiMax.toFixed(0) + "ms"
        window.vfGuiSum = 0; window.vfGuiN = 0; window.vfGuiMax = 0
        const now = window.vfSnapshot()
        const prev = window.vfPrev
        const dt = prev.t ? (now.t - prev.t) / 1000 : 0
        if (dt > 0.2) {
            const src = window.vfPhase === "camera" ? (now.cam - prev.cam) : (now.media - prev.media)
            let line = "[SELFTEST-VIDEOFPS] phase=" + window.vfPhase + " dt=" + dt.toFixed(2)
                       + " source=" + (src / dt).toFixed(1)
                       + " loop=" + ((now.loop - prev.loop) / dt).toFixed(1)
                       + " tile=" + ((now.tile - prev.tile) / dt).toFixed(1)
                       + " ndi=" + ((now.ndi - prev.ndi) / dt).toFixed(1) + guiLine
            for (const k in now.out)
                line += " out[" + k + "]=" + ((now.out[k] - (prev.out[k] || 0)) / dt).toFixed(1)
            console.log(line)
        }
        window.vfPrev = now
    }
    Timer {
        id: selfTestVideoFpsStart
        running: typeof SelfTestVideoFps !== "undefined" && SelfTestVideoFps
        interval: 9000
        onTriggered: {
            // VGR_VIDEOFPS_GPU=1: measure the GPU renderer; the user's own choice is put back at the end.
            if (typeof SelfTestVideoGpu !== "undefined" && SelfTestVideoGpu) {
                window.vfGpuBefore = SettingsService.value("preferences.hardwareAcceleration")
                SettingsService.setValue("preferences.hardwareAcceleration", true)
                console.log("[SELFTEST-VIDEOFPS] GPU renderer requested (was " + window.vfGpuBefore + ")")
            }
            let label = ""
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                const l = String(row && row.sublabel ? row.sublabel : "").trim()
                if (String(row.kind || "") === "camera" && l !== "") { label = l; break }
            }
            if (label !== "")
                LiveOutputService.takeInput(label, "camera", "")
            LiveOutputService.goLiveWithSlides("Video Rate Probe", [ {
                valid: true, title: "Video Rate Probe", text: "video rate probe", background: "",
                blocks: [ { kind: "text", text: "video rate probe", x: 60, y: 300, width: 630,
                            height: 70, meta: { fontSize: 28 } } ] } ])
            console.log("[SELFTEST-VIDEOFPS] camera=" + JSON.stringify(label) + " file="
                        + JSON.stringify(String(SelfTestVideoFpsFile)))
            window.vfPhase = "camera"
            window.vfSamples = 0
            window.vfPrev = ({})
            selfTestVideoFpsWarm.restart()
        }
    }
    Timer {
        id: selfTestVideoFpsWarm
        interval: 5000
        onTriggered: {
            window.vfPrev = window.vfSnapshot()
            selfTestVideoFpsSample.restart()
        }
    }
    Timer {
        id: selfTestVideoFpsSample
        interval: 2000
        repeat: true
        onTriggered: {
            window.vfReport()
            if (window.vfPhase === "file")
                console.log("[SELFTEST-VIDEOFPS] file declares " + Number(LiveOutputService.mediaSourceFps()).toFixed(2)
                            + " fps, state=" + LiveOutputService.mediaState)
            if (window.vfSamples === 3) {   // LOOK at what the tiles show (build/vf_shot_*.png)
                for (let t = 0; t < 2; ++t)
                    SelfTest.grab("selfTestMonitorTile" + t, "vf_shot_" + window.vfPhase + "_" + t + ".png")
            }
            if (++window.vfSamples < 6)
                return
            selfTestVideoFpsSample.stop()
            window.vfSamples = 0
            if (window.vfPhase === "camera" && String(SelfTestVideoFpsFile) !== "") {
                LiveOutputService.clearInput()
                window.vfPhase = "file"
                LiveOutputService.takeMedia(String(SelfTestVideoFpsFile), "video rate file")
                if (LiveOutputService.mediaMuted === false)
                    LiveOutputService.mediaToggleMuted()
                window.vfPrev = ({})
                selfTestVideoFpsWarm.restart()
                return
            }
            if (typeof SelfTestVideoGpu !== "undefined" && SelfTestVideoGpu && window.vfGpuBefore !== undefined)
                SettingsService.setValue("preferences.hardwareAcceleration", window.vfGpuBefore)
            // Grab what an output tile shows, to LOOK at the frame (VGR_VIDEOFPS_SHOT=<prefix>).
            console.log("[SELFTEST-VIDEOFPS] done")
            LiveOutputService.clearMedia()
            LiveOutputService.clearInput()
            SelfTest.quit()
        }
    }
    // ONE ROW PER MONITOR TILE: every term its picture depends on, plus the grab of that tile. A blank
    // tile is then a line in this table, not a guess — `gate`/`onAir`/`engine`/`frame` say which term
    // refused it.
    function selfTestTileFacts(tag) {
        for (let t = 0; ; ++t) {
            const tile = SelfTest.findItem("selfTestMonitorTile" + t)
            if (!tile)
                break
            console.log("[SELFTEST-CARD] " + tag + " tile=" + t
                        + " name=" + JSON.stringify(String(tile.name))
                        + " enabled=" + tile.isEnabled
                        + " gate=" + tile.mediaGateOpen
                        + " onAir=" + tile.mediaOnAir
                        + " engine=" + tile.engineComposed
                        + " frame=" + tile.hasFrame
                        + " slide=" + tile.hasSlidePreview
                        + " overlays=" + tile.overlaysOn)
            SelfTest.grab("selfTestMonitorTile" + t, "cp_" + tag + "_tile_" + t + ".png")
        }
    }
    // ---- MEDIA-PANE CARD PREVIEW (VGR_CARDPREVIEW_TEST=1) ---------------------------------------------
    // What a card does when it is held: the camera card's live thumbnail (previewInput -> producing +
    // inputRev) and a microphone card's L/R meter (startInputMeter -> peaks). Logged once a second.
    property string cpCam: ""
    property string cpMic: ""
    property int cpTicks: 0
    property int cpRev0: 0
    Timer {
        id: selfTestCardPreviewStart
        running: typeof SelfTestCardPreview !== "undefined" && SelfTestCardPreview
        interval: 9000
        onTriggered: {
            for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
                const row = VideoSourceListModel.getSource(i)
                if (String(row.kind || "") === "camera" && String(row.sublabel || "").trim() !== "") {
                    window.cpCam = String(row.sublabel).trim(); break
                }
            }
            for (let i = 0; i < AudioInputListModel.rowCount(); ++i) {
                const row = AudioInputListModel.getInput(i)
                if (String(row.sublabel || "").trim() !== "") { window.cpMic = String(row.sublabel).trim(); break }
            }
            console.log("[SELFTEST-CARD] camera=" + JSON.stringify(window.cpCam) + " mic=" + JSON.stringify(window.cpMic))
            window.cpRev0 = Number(LiveOutputService.inputRev)
            // The real pane: open the Media tab, show the Buses, hold each bus card like a click would.
            SelfTest.clickItem("selfTestTab_media")
            selfTestCardPreviewHold.start()
        }
    }
    Timer {
        id: selfTestCardPreviewHold
        interval: 2000
        onTriggered: {
            const pane = SelfTest.findItem("selfTestMediaPane")
            console.log("[SELFTEST-CARD] pane=" + (pane ? "found" : "MISSING"))
            if (!pane) { SelfTest.quit(); return }
            pane.selection = "inputs"
            const rows = pane.inputRows
            console.log("[SELFTEST-CARD] bus rows=" + JSON.stringify(rows))
            for (let i = 0; i < rows.length; ++i) {
                const b = BusListModel.getBus(Number(rows[i]))
                const vr = b.routedVideoSources || []
                let why = ""
                for (const r of vr) {
                    const c = VideoSourceListModel.getSource(r)
                    why += " [src " + r + " kind=" + c.kind + " label=" + JSON.stringify(c.sublabel)
                           + " healthy=" + LiveOutputService.inputHealthy(String(c.sublabel || "").trim(), String(c.kind || ""))
                           + " available=" + LiveOutputService.inputAvailable(String(c.sublabel || "").trim(), String(c.kind || "")) + "]"
                }
                console.log("[SELFTEST-CARD] bus " + rows[i] + " type=" + b.type + " muted=" + b.muted + " level=" + b.level
                            + " videoRoutes=" + vr.length + " audioRoutes=" + (b.routedAudioInputs || []).length + why)
                pane.toggleBusMonitorPreview(Number(rows[i]))
            }
            // The monitor tiles BEFORE going live: a media file taken to the output preview.
            // What a double-click on the Camera bus does: take its media source to the output preview.
            pane.toggleVideoSourcePreview(2)
            console.log("[SELFTEST-CARD] held: videoBusId=" + pane.busMonitorVideoBusId + " audioBusId=" + pane.busMonitorAudioBusId
                        + " previews=" + JSON.stringify(LiveOutputService.cardPreviews)
                        + " metered=" + JSON.stringify(pane.meteredLabels))
            selfTestCardPreviewSample.start()
        }
    }
    Timer {
        id: selfTestCardPreviewSample
        interval: 1000
        repeat: true
        onTriggered: {
            const list = EngineBridge.inputMeterList()
            let peaks = "none"
            for (let i = 0; i < list.length; ++i)
                if (String(list[i].label || "") === window.cpMic)
                    peaks = JSON.stringify(list[i].peaks) + " ch=" + list[i].channelCount
            console.log("[SELFTEST-CARD] t=" + window.cpTicks + " mediaRev=" + Number(LiveOutputService.mediaRev)
                        + " decoded=" + Number(LiveOutputService.mediaFramesDecoded())
                        + " busMeterPeaks=" + (function () {
                            const l = EngineBridge.inputMeterList(); const p = []
                            for (let i = 0; i < l.length; ++i) p.push(String(l[i].label).slice(-14) + ":" + JSON.stringify(l[i].peaks))
                            return p.join(" ") })()
                        + " gui=" + (window.vfGuiN > 0 ? (window.vfGuiSum / window.vfGuiN).toFixed(1) : "-") + "/" + window.vfGuiMax.toFixed(0)
                        + " producing=" + LiveOutputService.inputProducing(window.cpCam)
                        + " inputRev+=" + (Number(LiveOutputService.inputRev) - window.cpRev0)
                        + " meters=" + list.length + " micPeaks=" + peaks)
            if (window.cpTicks === 3) {
                const pane = SelfTest.findItem("selfTestMediaPane")
                const rows = pane ? pane.inputRows : []
                for (let i = 0; i < rows.length; ++i)
                    SelfTest.grab("selfTestBusCard_" + rows[i], "cp_card_" + rows[i] + ".png")
                window.selfTestTileFacts("pre")
                selfTestCardPreviewWindowsEarly.start()   // the real output windows, same moment
                console.log("[SELFTEST-CARD] tile state: mediaOnAir=" + LiveOutputService.mediaOnAir + " path=" + LiveOutputService.mediaPath
                            + " live=" + LiveOutputService.live + " inputLabel=" + JSON.stringify(LiveOutputService.inputLabel)
                            + " mediaIsAudio=" + LiveOutputService.mediaIsAudio + " name=" + LiveOutputService.mediaName)
            }
            if (++window.cpTicks >= 8) {
                selfTestCardPreviewSample.stop()
                // ---- GO LIVE PHASE --------------------------------------------------------------
                // THE SAME TAKE, NOW LIVE. A tile draws a media file with LiveMediaSurface on both
                // sides of GO LIVE, so the grabs taken here must show the same kind of picture as the
                // pre-live ones — a swap to the engine's frame would show up as a changed grab.
                console.log("[SELFTEST-CARD] pressing GO LIVE (live before=" + LiveOutputService.live + ")")
                LiveOutputService.goLive()
                selfTestCardPreviewLive.start()
            }
        }
    }
    // The GO LIVE phase of the card-preview scenario: grab the SAME tiles once the session is live.
    Timer {
        id: selfTestCardPreviewLive
        interval: 5000
        onTriggered: {
            console.log("[SELFTEST-CARD] live phase: live=" + LiveOutputService.live
                        + " mediaOnAir=" + LiveOutputService.mediaOnAir
                        + " name=" + LiveOutputService.mediaName
                        + " frameRev=" + Number(LiveOutputService.frameRev))
            window.selfTestTileFacts("live")
            selfTestCardPreviewStage.start()
        }
    }
    // THE OTHER TWO SURFACES THAT SHOW THE SAME FILE: the centre page (ShowCenter — what the Stage tab
    // puts on screen) and the library's own grid card. Both draw through the same shared surface as the
    // monitor tiles, so both grabs must show the file's picture, not a poster or a blank.
    Timer {
        id: selfTestCardPreviewStage
        interval: 1400
        // The folder this run put in the library for the card, and whether it was already there: it is
        // removed at the end either way, so the scenario is idempotent and leaves no trace.
        property string folder: ""
        property bool folderWasPresent: false
        onTriggered: {
            // The file needs a CARD for the grid surface to be testable at all, so its folder goes in
            // the library first (a no-op when the folder is already there).
            // The folder of the file the run was given (VGR_VIDEOFPS_FILE) — no machine's own path is
            // baked in. Without a file there is nothing to stage, so the phase ends cleanly.
            const given = typeof SelfTestVideoFpsFile !== "undefined" ? String(SelfTestVideoFpsFile) : ""
            const folder = given.replace(/[\\/][^\\/]*$/, "")
            if (folder === "" || folder === given) {
                console.log("[SELFTEST-CARD] stage phase: no VGR_VIDEOFPS_FILE — skipped")
                SelfTest.quit()
                return
            }
            const folders = MediaLibraryService.folders
            let present = false
            for (let i = 0; i < folders.length; ++i)
                if (String(folders[i].path) === folder)
                    present = true
            if (!present)
                MediaLibraryService.addFolderPath(folder)
            selfTestCardPreviewStage.folder = folder
            selfTestCardPreviewStage.folderWasPresent = present
            const pane = SelfTest.findItem("selfTestMediaPane")
            if (pane) {
                pane.selection = "all"          // the grid again (the take phase used the Buses list)
                // ASK FOR THE ROWS, never rely on onSelectionChanged: when the selection was already
                // "all" that handler does not fire, and the grid kept whatever it read before the
                // library's scan had published (it read nothing, so it drew the empty state).
                pane.reload()
            }
            console.log("[SELFTEST-CARD] stage phase: folder=" + folder + " present=" + present)
            selfTestCardPreviewStageGrab.start()
        }
    }
    Timer {
        id: selfTestCardPreviewStageGrab
        interval: 3200
        onTriggered: {
            // THE LIBRARY'S OWN PREVIEW: the card holding the file that is on air, grabbed as it is drawn
            // (no poster, no thumbnail — the shared surface's picture).
            const path = String(LiveOutputService.mediaPath)
            const card = SelfTest.findItem("selfTestMediaTile:" + path)
            SelfTest.grab("selfTestMediaPane", "cp_grid.png")
            console.log("[SELFTEST-CARD] grid phase: card=" + (card ? "found" : "MISSING")
                        + " cards=" + (SelfTest.findItem("selfTestMediaPane") ? "pane" : "-")
                        + " path=" + JSON.stringify(path))
            // THE USER'S OWN GESTURE: a click on the card, which is what puts the file on the centre page
            // (and what the splash above it waits for).
            if (card)
                SelfTest.clickItem("selfTestMediaTile:" + path)
            selfTestCardPreviewStageClick.start()
        }
    }
    Timer {
        id: selfTestCardPreviewStageClick
        interval: 2500
        onTriggered: {
            const sc = SelfTest.findItem("selfTestShowCenter")
            SelfTest.grab("selfTestMainScreen", "cp_stage.png")
            if (sc)
                SelfTest.grab("selfTestShowCenter", "cp_view.png")
            console.log("[SELFTEST-CARD] stage grabs: centre="
                        + (sc ? ("visible=" + sc.visible + " size=" + Math.round(sc.width) + "x"
                                 + Math.round(sc.height)) : "MISSING")
                        + " centerItem=" + JSON.stringify(showScreen.centerItem)
                        + " onAir=" + LiveOutputService.mediaOnAir)
            // The scenario's folder entry goes away again: it exists only to give this file a card, so the
            // library is left as it was found instead of carrying the test's folder from run to run.
            const usedFolder = selfTestCardPreviewStage.folder
            if (usedFolder !== "")
                MediaLibraryService.removeFolder(usedFolder)
            showScreen.centerItem = null
            selfTestCardPreviewWindows.start()
        }
    }
    // ---- THE OUTPUT WINDOWS BEFORE GO LIVE ----------------------------------------------------
    // Before the session is live no frame is flowing, so each window shows its OWN state — the media file
    // where its route is open, the checkerboard where nothing paints — which is what the tile grabs are
    // read against. (After GO LIVE every window draws the engine's frame, a different picture, so the
    // empty state has to be grabbed here as well as there.)
    Timer {
        id: selfTestCardPreviewWindowsEarly
        interval: 500
        onTriggered: {
            outputWindows.selfTestForceVisible = true
            selfTestCardPreviewWindowsEarlyGrab.start()
        }
    }
    Timer {
        id: selfTestCardPreviewWindowsEarlyGrab
        interval: 1000
        onTriggered: {
            const wins = SelfTest.windows()
            let grabbed = 0
            for (let i = 0; i < wins.length; ++i) {
                const name = String(wins[i].objectName ?? "")
                if (name.indexOf("outputWindow_") !== 0)
                    continue
                if (SelfTest.grabWindow(name, "cp_early_" + name + ".png"))
                    grabbed++
            }
            outputWindows.selfTestForceVisible = false
            console.log("[SELFTEST-CARD] early window phase: grabbed=" + grabbed
                        + " live=" + LiveOutputService.live
                        + " mediaOnAir=" + LiveOutputService.mediaOnAir)
        }
    }
    // ---- THE REAL OUTPUT WINDOWS, GRABBED THE SAME WAY -----------------------------------------
    // An output window is its own top-level QQuickWindow (OutputWindow.qml), so it is grabbed BY NAME
    // (SelfTest.grabWindow): the monitor tile had a pixel test while the real output it stands for had
    // none. Forced visible because a window is otherwise only up with an output live on a real screen,
    // and this machine has none — the grab is of the window's own content, which is what the tile test
    // reads too, so the empty state (checkerboard / black) can be compared surface to surface.
    Timer {
        id: selfTestCardPreviewWindows
        interval: 900
        onTriggered: {
            outputWindows.selfTestForceVisible = true
            selfTestCardPreviewWindowsGrab.start()
        }
    }
    Timer {
        id: selfTestCardPreviewWindowsGrab
        interval: 1400
        onTriggered: {
            const wins = SelfTest.windows()
            let grabbed = 0
            for (let i = 0; i < wins.length; ++i) {
                const name = String(wins[i].objectName ?? "")
                if (name.indexOf("outputWindow_") !== 0)
                    continue
                if (SelfTest.grabWindow(name, "cp_" + name + ".png"))
                    grabbed++
            }
            console.log("[SELFTEST-CARD] window phase: windows=" + wins.length
                        + " grabbed=" + grabbed
                        + " live=" + LiveOutputService.live
                        + " mediaOnAir=" + LiveOutputService.mediaOnAir
                        + " mediaName=" + LiveOutputService.mediaName)
            outputWindows.selfTestForceVisible = false
            // Give the folder removal time to reach the settings store before the process exits (a
            // 1.5 s quit left the test's folder in the user's library).
            selfTestCardPreviewQuit.start()
        }
    }
    Timer {
        id: selfTestCardPreviewQuit
        interval: 4000
        onTriggered: SelfTest.quit()
    }
    // ---- Program-feed rate for a held camera (VGR_INPUTRATE_TEST=1) ----------
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
            const probeColor = "#0f0e0d"
            const wasColor = String(StyleListModel.getStyle(window.bsStyleRow).backgroundColor)
            StyleListModel.setBackgroundColor(window.bsStyleRow, probeColor)
            const seenColor = String(OutputListModel.styleBackground(window.bsTileIndex).color)
            StyleListModel.setBackgroundColor(window.bsStyleRow, wasColor)
            console.log("[SELFTEST-STYLEBG] identity probe wrote=" + probeColor
                        + " modelSeen=" + seenColor
                        + " sameRoster=" + String(seenColor === probeColor)
                        + " rowForId=" + StyleListModel.rowForId(window.bsStyleId))
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
        const probeX = cW.x + cW.w - 23 * window.uiScale
        const probeY = cW.y + 3
        const cursor = String(SelfTest.cursorAt(probeX, probeY))
        const cursorOk = cursor.indexOf("SIZE") < 0
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
    property var metaAll: []
    property string metaClickToken: ""
    property string metaDragToken: ""
    property string metaClickKey: ""
    property string metaDragKey: ""
    property var metaKeysBefore: []
    property var metaDropAt: ({ x: 0, y: 0 })
    property var metaDragTo: ({ x: 0, y: 0 })
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
                        + " bind=" + JSON.stringify(f ? f.bind : "")
                        + " wantBind=" + JSON.stringify(String(window.metaAll[0].bind === undefined ? "" : window.metaAll[0].bind))
                        + " bindOk=" + (!!f && f.bind === String(window.metaAll[0].bind === undefined ? "" : window.metaAll[0].bind)))
            if (!f) {
                SelfTest.quit()
                return
            }
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
                        + " box=" + (f ? Math.round(f.w) + "x" + Math.round(f.h) : "?")
                        + " font=" + (f ? String(f.font) : "?")
                        + " autoSize=" + JSON.stringify(f ? f.autoSize : "")
                        + " bind=" + JSON.stringify(f ? f.bind : "")
                        + " wantBind=" + JSON.stringify(String(window.metaAll[window.metaAll.length - 1].bind === undefined ? "" : window.metaAll[window.metaAll.length - 1].bind))
                        + " bindOk=" + (!!f && f.bind === String(window.metaAll[window.metaAll.length - 1].bind === undefined ? "" : window.metaAll[window.metaAll.length - 1].bind)))
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
    property int tableFolderTick: 0
    property var tableFolderSeen: []      // one entry per notice, oldest first
    function tableFolderCollect() {
        const items = NotificationCenter.items
        for (let i = 0; i < items.length; ++i) {
            const e = items[i]
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
            const overlay = SelfTest.findItem("selfTestToast")
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