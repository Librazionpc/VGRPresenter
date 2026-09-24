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
        // The app-wide search (the splash's "Quick search" button).
        onSearchRequested: quickSearch.openSearch()
        onImportRequested: importDialog.open = true
        // A card's Edit action in the Overlays / Templates tab: the Edit screen edits that overlay / template.
        onDesignEditRequested: (kind, id) => {
            if (editScreen.openDesign(kind, id))
                window.currentView = "edit"
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
        onImportRequested: importDialog.open = true
    }

    // Ctrl+I: File > Import.
    Shortcut {
        sequence: "Ctrl+I"
        onActivated: importDialog.open = true
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




    // ---- UI self-test scenario (TEMPORARY diagnostic, env-gated) ----------
    // Drives the REAL scripture search flow: click the box, type "est", grab
    // (commit state), type " 1:4" (auto-colon + verse), grab the popup and the
    // full window, then quit. The grabs land in the working dir as PNGs for
    // offline pixel/property sampling.
    Timer {
        id: selfTestStage1
        running: typeof SelfTest !== "undefined"
        interval: 1500
        onTriggered: {
            console.log("[SELFTEST] stage 1: open The TABLE tab, focus the search input")
            window.currentView = "show"
            const bar = SelfTest.findItem("selfTestLibraryTabBar")
            if (bar) {
                // currentPane is read-only (derived) — the tab INDEX is the writable one.
                const idx = bar.tabs.findIndex((t) => t.pane === "table")
                if (idx >= 0) bar.currentTab = idx   // property write: no OS click can miss
            }
            if (SelfTest.findItem("selfTestLibraryTabBar") === null || SelfTest.findItem("selfTestLibraryTabBar").currentPane !== "table")
                SelfTest.clickItem("selfTestTab_table")
            selfTestStage2.restart()
        }
    }
    Timer {
        id: selfTestStage2
        interval: 700
        onTriggered: {
            if (!SelfTest.focusItem("selfTestSearchInput"))
                SelfTest.clickItem("selfTestSearchBox")
            selfTestStage3.restart()
        }
    }
    Timer {
        id: selfTestStage3
        interval: 400
        onTriggered: {
        console.log("[SELFTEST] stage 3: type '47-11' (5-char rule: multiple matches -> dropdown)")
        SelfTest.type("47-11")
            selfTestStage4.restart()
        }
    }
    Timer {
        id: selfTestStage4
        interval: 700
        onTriggered: {
        console.log("[SELFTEST] stage 4: after '47-11' — popup should LIST the matches (1100X, 1102, 1123, ...)")
        const box = SelfTest.findItem("selfTestSearchBox")
        if (box) console.log("[SELFTEST] box text =", JSON.stringify(box.text))
        const sug = SelfTest.findItem("selfTestSuggestPopup")
        console.log("[SELFTEST] popup:", sug ? (sug.visible ? "VISIBLE (FAIL — Table matches live in the pane now)" : "hidden (ok)") : "never built (ok)")
        const pane0 = SelfTest.findItem("selfTestTablePane")
        if (pane0) console.log("[SELFTEST] in-pane matches =", pane0.citationMatches.length)
        console.log("[SELFTEST] pane must NOT have jumped (multiple matches):",
                    (function () { const p = SelfTest.findItem("selfTestTablePane"); return p ? (p.book ? p.book.name + " ch" + p.chapterNumber : "no book") : "no pane" })())
        SelfTest.grab("", "shot_table_multimatch.png")
            selfTestStage4b.restart()
        }
    }
    // Hover-peek probe: the REAL cursor onto the popup's second row — the verses
    // column must swap to that sermon's paragraphs while the hover lasts, and
    // return to the open chapter when the cursor leaves the popup.
    Timer {
        id: selfTestStage4b
        interval: 400
        onTriggered: {
            console.log("[SELFTEST] stage 4b: hover popup row 1 — peek ON?")
            // Hover the SECOND in-pane match row by position truth (what the
            // catcher feeds on a real user move).
            const row = SelfTest.findItem("selfTestMatchRow_1")
            if (row) {
                const c = row.mapToItem(null, row.width / 2, row.height / 2)
                SelfTest.move(c.x, c.y)
                AppCursor.setPointerPos(Qt.point(c.x, c.y))
                console.log("[SELFTEST] hovering match row 1 at", JSON.stringify(c))
            } else console.log("[SELFTEST] NO match rows to hover")
            selfTestStage4c.restart()
        }
    }
    Timer {
        id: selfTestStage4c
        interval: 500
        onTriggered: {
            const pane = SelfTest.findItem("selfTestTablePane")
            if (pane) console.log("[SELFTEST] peek index =", pane.peekIndex,
                                  "peek verses =", pane.peekVerses.length,
                                  "open verses =", pane.chapterVerses.length,
                                  "content swapped =",
                                  pane.peekVerses.length > 0 && pane.peekVerses[0].text !== pane.chapterVerses[0].text)
            const row = SelfTest.findItem("selfTestMatchRow_1")
            console.log("[SELFTEST] AppCursor point =", JSON.stringify(AppCursor._point),
                        "point-in-row =", row ? AppCursor.hovered(row) : false)
            SelfTest.grab("", "shot_table_hoverpeek.png")
            SelfTest.move(600, 500)   // off the rows, over the verses area
            AppCursor.setPointerPos(Qt.point(600, 500))
            selfTestStage4d.restart()
        }
    }
    Timer {
        id: selfTestStage4d
        interval: 500
        onTriggered: {
            const pane = SelfTest.findItem("selfTestTablePane")
            if (pane) console.log("[SELFTEST] after leave — peek index =", pane.peekIndex, "(must be -1)")
            selfTestStage5.restart()
        }
    }
    Timer {
        id: selfTestStage5
        interval: 500
        onTriggered: {
            console.log("[SELFTEST] stage 5: type '02' — '47-1102' is UNIQUE: inline fill + jump, popup closes")
            SelfTest.type("02")
            selfTestStage6.restart()
        }
    }
    Timer {
        id: selfTestStage6
        interval: 700
        onTriggered: {
            console.log("[SELFTEST] stage 6: after '47-1102'")
            const box = SelfTest.findItem("selfTestSearchBox")
            if (box) console.log("[SELFTEST] box text =", JSON.stringify(box.text), "(should carry the filled citation)")
            const sug = SelfTest.findItem("selfTestSuggestPopup")
            console.log("[SELFTEST] popup:", sug ? (sug.visible ? "VISIBLE (FAIL)" : "hidden (ok)") : "never built (ok)")
            const pane = SelfTest.findItem("selfTestTablePane")
            if (pane) console.log("[SELFTEST] pane jumped to:", pane.book ? pane.book.name + " sermon " + pane.chapterNumber : "none (FAIL)")
            SelfTest.grab("", "shot_table_faith.png")
            selfTestStage7.restart()
        }
    }
    Timer {
        id: selfTestStage7
        interval: 500
        onTriggered: {
            console.log("[SELFTEST] stage 7: backspace sweep (the filled citation is ~32 chars)")
            SelfTest.type("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b")
            selfTestStage8.restart()
        }
    }
    Timer {
        id: selfTestStage8
        interval: 700
        onTriggered: {
            console.log("[SELFTEST] stage 8: after sweep — box must be empty")
            const box = SelfTest.findItem("selfTestSearchBox")
            if (box) console.log("[SELFTEST] box text =", JSON.stringify(box.text), "(empty = PASS)")
            SelfTest.grab("", "shot_table_cleared.png")
            selfTestStage9.restart()
        }
    }
    Timer {
        id: selfTestStage9
        interval: 600
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
            objectName: "selfTestSettingsShell"
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

    // Carries a drag from one pane to another, above everything.
    DragLayer {
        id: dragLayerItem
    }

    // File > Import: every format the engine reads (FreeShow's Import screen). Above the screens and the Settings overlay, below the toasts.
    ImportDialog {
        id: importDialog
        onClosed: importDialog.open = false
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
