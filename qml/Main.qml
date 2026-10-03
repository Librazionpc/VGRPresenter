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

    property string settingsSection: "general"
    // The header is now two rows — the title bar (logo, menu, window buttons), a
    // separator line, then the Show / Edit / Stage tabs. The screens were designed
    // around a 48 px header, so they are pushed down by however much taller it is
    // (their own top 48 px sits underneath the header, hidden).
    readonly property int headerHeight: appHeader.height
    readonly property int headerExtra: window.headerHeight - 48
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
        else if (EngineBridge.bootError.length > 0)
            EventBus.notify(EngineBridge.bootError, "error", qsTr("Engine failed to boot"), "engine.boot")

        // Search: the Bible files are read into the engine (background thread) so verses
        // can be found.
        SearchService.loadBibles()
    }

    // Screens first (opaque, fill the window), then the shared header strip
    // and the menu layer on top — AppMenuBar must sit above AppHeader because
    // its logo/menu labels are drawn inside the title row of the same header.
    VGRPresenterMainScreen {
        id: showScreen
        anchors.fill: parent
        anchors.topMargin: window.headerExtra
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
        anchors.fill: parent
        anchors.topMargin: window.headerExtra
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
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
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
        onResultChosen: (result) => window.openSearchResult(result)
    }

    // What picking a search result does. The dialog only finds things (SearchService, backed
    // by the engine); this is the one place that knows how to go to them.
    function openSearchResult(r) {
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
            showScreen.openSermonAt(r.bookId, r.chapter, r.verse)
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
    }
    Shortcut {
        sequence: "Ctrl+K"
        onActivated: quickSearch.openSearch()
    }

    AppMenuBar {
        anchors.fill: parent
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
    WindowResizeHandles {
        anchors.fill: parent
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
        // ModalScrim's #99000000 is exactly the 0.6 black this overlay
        // always used — no visual change, just the shared behavior.
        onDismissed: settingsScrim.visible = false

        ModalShell {
            id: settingsShell
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
        OutputsScreen {}
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

    // Carries a drag from one pane to another, above everything.
    DragLayer {
        id: dragLayerItem
    }

    // GO LIVE's real destination(s) — a borderless window per Output bound
    // to a physical/HDMI screen in Settings > Outputs. Top-level Windows,
    // not part of this window's visual tree (Instantiator, not a visible
    // child item) — see OutputWindowManager.qml.
    OutputWindowManager {}

    // File > Import: every format the engine reads (FreeShow's Import screen). Above the screens and the Settings overlay, below the toasts.
    ImportDialog {
        id: importDialog
        onClosed: importDialog.open = false
    }

    // "New show" — FreeShow's own New-show popup (Name, Category, Quick
    // lyrics/Web search/Empty show). "New show" from the Show screen's CTAs
    // and the File menu both open this instead of resetting straight to a
    // blank canvas.
    NewShowDialog {
        id: newShowDialog
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
    NotificationOverlay {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.space5
        // Below the header strip, so a toast never covers the window buttons in the
        // title bar (the header is window.headerHeight tall).
        anchors.topMargin: window.headerHeight + 8
        z: 10000
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
}
