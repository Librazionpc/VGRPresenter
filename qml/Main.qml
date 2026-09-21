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
        onNewShowRequested: {
            editScreen.newShow()
            window.currentView = "edit"
        }
        // Row click in the shows library: open that .vgr (unsaved-changes
        // guarded) and go to the Edit screen.
        onOpenShowRequested: (path) => {
            editScreen.openShowPath(path)
            window.currentView = "edit"
        }
        // The Projects panel's search box / "Quick search" button: the same app-wide search.
        onSearchRequested: quickSearch.openSearch()
    }

    EditScreen {
        id: editScreen
        anchors.fill: parent
        anchors.topMargin: window.headerExtra
        visible: window.currentView === "edit"
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
            editScreen.openShowPath(r.path)
            window.currentView = "edit"
            break
        case "slide":
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
        case "bible": {
            // A verse becomes a slide in the working show.
            ShowService.ensureShow(qsTr("Untitled show"))
            const ref = r.title + (r.subtitle !== "" ? " (" + r.subtitle + ")" : "")
            const body = r.fullText !== undefined && r.fullText !== "" ? r.fullText : r.text
            const id = ShowService.addSlide({
                title: r.title,
                ref: ref,
                line1: body,
                blocks: [
                    { kind: "text", text: body, x: 24, y: 24, width: 420, height: 110 },
                    { kind: "text", text: ref, x: 24, y: 144, width: 420, height: 30 }
                ]
            })
            if (id !== "") {
                window.currentView = "edit"
                editScreen.session.refresh(id)
                EventBus.notify(qsTr("Added %1 to the show.").arg(r.title), "success", qsTr("Bible"), "search.bible.added")
            }
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
        onNewShowRequested: {
            editScreen.newShow()
            window.currentView = "edit"
        }
        // Show files: the engine reads/writes the .vgr (see ShowSession.qml).
        onOpenShowRequested: {
            window.currentView = "edit"
            editScreen.openShow()
        }
        onQuickSearchRequested: quickSearch.openSearch()
        onSaveShowRequested: editScreen.saveShow()
        onSaveShowAsRequested: editScreen.saveShowAs()
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
        if (window.closeConfirmed || !ShowService.hasShow || !ShowService.showDirty)
            return
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

    // ---- UI self-test scenario (TEMPORARY diagnostic, env-gated) ----------
    // Drives the real UI with real OS cursor moves through SelfTestDriver
    // (main.cpp only wires it when VGR_SELFTEST=1) and grabs PNGs for
    // offline pixel sampling. Covers the Show screen's live surfaces:
    // the New show CTA's hover reaction and the library dock tabs (Shows
    // table, Media rosters, Scripture/The Table engine-waiting panes,
    // coming-soon). The old Edit-screen probes (shape-canvas painting,
    // chip-row flicker) were removed once those bugs were fixed and
    // verified — the harness stays lean.
    Timer {
        id: selfTestStage1
        running: typeof SelfTest !== "undefined"
        interval: 1200
        onTriggered: {
            console.log("[SELFTEST] stage 1: show view, park cursor on New show CTA")
            window.currentView = "show"
            const cta = SelfTest.itemCenter("selfTestNewShowBtn")
            if (cta) SelfTest.move(cta.x, cta.y)
            // Seed one REAL show through the engine's own save path so the
            // library feed (categories sidebar, shows table, Media playlists)
            // proves itself non-empty — never mocks, always the engine.
            if (!ShowService.hasShow)
                ShowService.newShowDocument("Probe Sunday Service")
            ShowService.saveShowFile(ShowService.currentShow, ShowService.libraryPath + "/Probe Sunday Service.vgr")
            selfTestStage1b.restart()
        }
    }
    Timer {
        id: selfTestStage1b
        interval: 400
        onTriggered: {
            // Cursor parked on the CTA — the full-window grab shows the
            // hovered (lightened) pill.
            SelfTest.grab("", "shot_cta_hover.png")
            SelfTest.grab("selfTestShowsTable", "shot_tab_shows.png")
            SelfTest.clickItem("selfTestTab_media")
            selfTestStage13.restart()
        }
    }
    Timer {
        id: selfTestStage13
        interval: 500
        onTriggered: {
            SelfTest.grab("selfTestMediaPane", "shot_tab_media.png")
            SelfTest.clickItem("selfTestTab_scripture")
            selfTestStage14.restart()
        }
    }
    Timer {
        id: selfTestStage14
        interval: 500
        onTriggered: {
            SelfTest.grab("selfTestScripturePane", "shot_tab_scripture.png")
            SelfTest.clickItem("selfTestTab_table")
            selfTestStage15.restart()
        }
    }
    Timer {
        id: selfTestStage15
        interval: 500
        onTriggered: {
            SelfTest.grab("selfTestTablePane", "shot_tab_table.png")
            SelfTest.clickItem("selfTestTab_soon")
            selfTestStage16.restart()
        }
    }
    Timer {
        id: selfTestStage16
        interval: 500
        onTriggered: {
            SelfTest.grab("selfTestSoonPane", "shot_tab_soon.png")
            // Quit on a LATER tick: grabToImage saves asynchronously, and
            // quitting this frame killed the last save (missing soon shot).
            selfTestStage17.restart()
        }
    }
    Timer {
        id: selfTestStage17
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
        // Below the header strip, so a toast never covers the window buttons in the
        // title bar (the header is window.headerHeight tall).
        anchors.topMargin: window.headerHeight + 8
        z: 10000
    }
}
