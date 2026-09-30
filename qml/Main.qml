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
    property real selfTestT6: 0   // (self-test probe timing scratch)
    property real selfTestPillT: 0   // (stage 6c pill-search timing scratch)
    property int selfTestStyleRow: -1   // (stage 8s test style row — restored after the grab)
    // Singleton warm-up: QML singletons are created LAZILY — one no binding
    // has touched yet simply doesn't exist, and the C++ side reads
    // StyleListModel::instance() (the outputs model's roster relay, the
    // boot-time engine style push, every styleBackground() role read) and
    // gets null. Nothing in the boot scene touches the Styles screen, so
    // this stayed null for a whole session: the output's assigned style
    // resolved as "no style" everywhere — the monitor tile showed the
    // transparency checkerboard over the style's colour/image and the
    // engine rendered unstyled. Referencing both here forces them to exist
    // while the root's properties bind — before any child (the monitor
    // wall) is created — and OutputListModel's singleShot(0) retry then
    // wires the relay against the now-real instance.
    readonly property var _singletonWarmup: [StyleListModel.rowCount(), OutputListModel.rowCount()]
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




    // ---- UI self-test scenario (TEMPORARY diagnostic, env-gated) ----------
    // CRASH REPRO: the user typed "revel" in Quick search, picked the
    // "Revelation 1:1" reference row, and the app died with an unhandled
    // exception (crash.log signal=0 terminate). This scenario drives the
    // exact flow — open dialog, type, log results, invoke the SAME choose()
    // handler a row click runs — with a log line before and after each step,
    // so the crashing step names itself in the output.
    Timer {
        id: selfTestStage1
        running: false   // TEMP: yielded to selfTestTextDiag below
        interval: 1500
        onTriggered: {
            console.log("[SELFTEST] stage 1: open Quick search (Ctrl+K flow)")
            window.currentView = "show"
            quickSearch.openSearch()
            selfTestStage2.restart()
        }
    }
    Timer {
        id: selfTestStage2
        interval: 700
        onTriggered: {
            console.log("[SELFTEST] stage 2: type 'revelation 1:1'")
            SelfTest.type("revelation 1:1")
            selfTestStage3.restart()
        }
    }
    Timer {
        id: selfTestStage3
        interval: 900
        onTriggered: {
            console.log("[SELFTEST] stage 3: results after debounce —", JSON.stringify(quickSearch.results))
            SelfTest.grab("", "shot_quick_revel.png")
            selfTestStage4.restart()
        }
    }
    Timer {
        id: selfTestStage4
        interval: 400
        onTriggered: {
            // The user's crash: picking the REFERENCE row (Revelation 1:1), which
            // carries no bookId — openVerseAt resolves the title through the pane's
            // adapter. Find that row (kind=bible, bookId empty/undefined).
            let pick = quickSearch.results.find(
                (r) => r.kind === "bible" && (r.bookId === undefined || r.bookId === ""))
            if (!pick) pick = quickSearch.results[0]
            console.log("[SELFTEST] stage 4: PICK", pick ? pick.kind + "/" + pick.title : "nothing",
                        "(crash step)")
            if (pick) quickSearch.choose(pick)
            console.log("[SELFTEST] stage 4: choose() returned without crashing")
            selfTestStage4b.restart()
        }
    }
    // Hover-peek probe: the REAL cursor onto the popup's second row — the verses
    // column must swap to that sermon's paragraphs while the hover lasts, and
    // return to the open chapter when the cursor leaves the popup.
    Timer {
        id: selfTestStage4b
        interval: 800
        onTriggered: {
            console.log("[SELFTEST] stage 4b: after the pick — view =", window.currentView,
                        "scripture pane book =",
                        (function () { const p = SelfTest.findItem("selfTestScripturePane");
                                       return p ? (p.book ? p.book.name + " ch" + p.chapterNumber : "no book") : "no pane" })())
            SelfTest.grab("", "shot_quick_picked.png")
            selfTestStage4c.restart()
        }
    }
    Timer {
        id: selfTestStage4c
        interval: 400
        onTriggered: {
            console.log("[SELFTEST] stage 4c: pick survived — now the code-query check")
            quickSearch.openSearch()
            selfTestStage5.restart()
        }
    }
    // A repeating heartbeat through the SAME event loop the app runs on: if the
    // loop is alive these tick; the last tick before a terminate/hang names the
    // window where it died. (Stage 2's log vanished while the process stayed
    // alive — this distinguishes "event loop dead" from "stage never fired".)
    Timer {
        id: selfTestHeartbeat
        running: typeof SelfTest !== "undefined"
        interval: 500
        repeat: true
        // (If this fires after quit() — into the dying window — it kills the
        // teardown with an exception; the visible-guard keeps it inert there.)
        onTriggered: if (window.visible) console.log("[SELFTEST] heartbeat")
    }
    Timer {
        id: selfTestStage5
        interval: 1200   // (async search: request + worker round-trip needs a beat)
        onTriggered: {
            console.log("[SELFTEST] stage 5: type '47-'")
            SelfTest.type("47-")
            selfTestStage6.restart()
        }
    }
    Timer {
        id: selfTestStage6
        interval: 1600   // (type '47-' + debounce 140ms + async worker round-trip)
        onTriggered: {
            const kinds = {}
            for (const r of quickSearch.results)
                kinds[r.kind] = (kinds[r.kind] || 0) + 1
            console.log("[SELFTEST] stage 6: '47-' results —", JSON.stringify(kinds),
                        "first:", quickSearch.results.length ? quickSearch.results[0].kind + "/" + quickSearch.results[0].title : "none")
            // GLOW + LAG probe, async-shaped: request, WAIT for the rows to
            // arrive (stage 6b), then verify — the old probe read the dialog
            // synchronously and measured the PREVIOUS query's stale rows.
            window.selfTestT6 = Date.now()
            quickSearch.query = "then friend"
            quickSearch.refresh()
            selfTestStage6b.restart()
        }
    }
    Timer {
        id: selfTestStage6b
        interval: 1500
        onTriggered: {
            // GROUP AUDIT: user call — "friend shows only Bible, not sermon".
            // Log the full kind breakdown + each row's first 60 chars so the
            // missing group is identifiable from the log alone.
            {
                const kinds = {}
                for (const r of quickSearch.results)
                    kinds[r.kind] = (kinds[r.kind] || 0) + 1
                console.log("[SELFTEST] stage 6b-groups:", JSON.stringify(kinds))
                for (const r of quickSearch.results)
                    console.log("[SELFTEST]   row:", r.kind, "|", String(r.title).slice(0, 60))
            }
            // THE NO-HOVER TEST: the yellow glow must land inside the VISIBLE
            // snippet of EVERY row. The engine anchors snippets so the resolved
            // words sit within the 120-char cap; a row whose match words are all
            // past the cap is the "swallowed highlight" bug.
            const words = String(quickSearch.highlightSource || "then friends")
                              .toLowerCase().split(/\s+/).filter((w) => w.length >= 2)
            let glowOk = 0, glowBad = [], detail = []
            for (const r of quickSearch.results) {
                if (r.kind !== "table" && r.kind !== "bible") continue
                const low = String(r.text || "").toLowerCase()
                const at = words.map((w) => low.indexOf(w)).filter((p) => p >= 0)
                if (at.length > 0) {
                    glowOk++
                    detail.push(r.title.split(" · ")[0] + "@" + Math.min.apply(null, at))
                } else {
                    glowBad.push(r.title)
                }
            }
            console.log("[SELFTEST] stage 6b: 'then friend' arrived in",
                        (Date.now() - window.selfTestT6) + "ms", "rows:", quickSearch.results.length,
                        "— GLOW (no hover)", glowOk + "/" + (glowOk + glowBad.length), "visible",
                        glowBad.length ? "SWALLOWED: " + glowBad.slice(0, 3).join(" | ")
                                       : "(word positions: " + detail.slice(0, 4).join(", ") + ")")
            SelfTest.grab("", "shot_glow_then_friends.png")
            selfTestStage6c.restart()
        }
    }
    // Stage 6c: The Table PILL search, now ASYNC (the Quick-search pattern).
    // runSearch must return in ~0 GUI time (the heavy scan is on a worker
    // thread) and the rows must arrive via the token-guarded signal.
    Timer {
        id: selfTestStage6c
        interval: 1800
        onTriggered: {
            console.log("[SELFTEST] stage 6c: The Table pill search (async) — 'then friend'")
            const pane = SelfTest.findItem("selfTestTablePane")
            if (!pane) { console.log("[SELFTEST] stage 6c: pane NOT FOUND"); selfTestStage7.restart(); return }
            window.selfTestPillT = Date.now()
            const disp0 = Date.now()
            pane.runSearch("then friend")
            console.log("[SELFTEST] stage 6c: runSearch dispatched in", (Date.now() - disp0) + "ms (GUI thread — must be ~0)")
            selfTestPillCheck.restart()
        }
    }
    Timer {
        id: selfTestPillCheck
        interval: 1500
        onTriggered: {
            const pane = SelfTest.findItem("selfTestTablePane")
            if (!pane) { selfTestStage7.restart(); return }
            const n = pane.searchResults ? pane.searchResults.length : -1
            const first = n > 0 ? pane.searchResults[0].reference : "none"
            console.log("[SELFTEST] stage 6c-results:", n, "rows, first:", first,
                        "— arrived", (Date.now() - window.selfTestPillT) + "ms after dispatch",
                        n > 0 && String(pane.searchResults[0].snippet || "").indexOf("Then, friends") >= 0 ? "(verbatim lead)" : "")
            selfTestStage7.restart()
        }
    }
    Timer {
        id: selfTestStage7
        interval: 9000
        onTriggered: {
            quickSearch.query = "god"
            const s0 = Date.now()
            quickSearch.refresh()
            const s1 = Date.now()
            quickSearch.query = "then friend"
            quickSearch.refresh()
            const s2 = Date.now()
            quickSearch.query = "the"
            quickSearch.refresh()
            const s3 = Date.now()
            console.log("[SELFTEST] stage 7 STEADY (GUI-thread dispatch — what typing feels): 'god' =", (s1 - s0) + "ms",
                        "'then friend' =", (s2 - s1) + "ms", "'the' =", (s3 - s2) + "ms")
            selfTestStage8.restart()
        }
    }
    // Monitor-wall rendering audit (style backgrounds vs checkerboards —
    // the output-preview regression): grabs the wall's pixels + logs every
    // output's style/color/image state so the PNG can be interpreted.
    Timer {
        id: selfTestStage8
        interval: 300
        onTriggered: {
            const roster = OutputListModel.rowCount()
            const ai = OutputListModel.activeIndex()
            // (typeof only — no method call: calling rowCount() here would
            // force-instantiate the singleton and mask the warm-up fix.)
            console.log("[SELFTEST] stage 8: roster =", roster, "active =", ai,
                        "styles singleton:", (typeof StyleListModel !== "undefined") ? "defined" : "NOT DEFINED")
            const state = []
            for (let i = 0; i < roster; ++i) {
                const o = OutputListModel.getOutput(i)
                const bg = OutputListModel.styleBackground(i)
                state.push({ name: o.name, styleId: o.styleId, active: o.active,
                             color: bg.color, image: bg.image, hasImage: bg.hasImage })
            }
            console.log("[SELFTEST] stage 8a state:", JSON.stringify(state))
            const wall = SelfTest.findItem("selfTestMonitorWall")
            console.log("[SELFTEST] stage 8a wall:", wall ? "found" : "NOT FOUND")
            SelfTest.grab("selfTestMonitorWall", "shot_monitor_wall.png")
            // (quit() DESTROYS the window while grabToImage's async render is
            // still in flight — quit one beat later, after the save lands.)
            selfTestStage8s.restart()
        }
    }
    // Stage 8s: PROVE the styled path — a real style with a solid bg colour
    // assigned to the active output. The dataChanged relay must repaint the
    // wall: checkerboard gone, the colour showing.
    Timer {
        id: selfTestStage8s
        interval: 300
        onTriggered: {
            try {
                const row = StyleListModel.rowCount()
                window.selfTestStyleRow = row
                StyleListModel.addStyle()
                StyleListModel.setBackgroundColor(row, "#1064b0")
                OutputListModel.setStyle(0, StyleListModel.getStyle(row).id)
                console.log("[SELFTEST] stage 8s: style", StyleListModel.getStyle(row).id,
                            "bg #1064b0 assigned to output 0")
            } catch (e) { console.log("[SELFTEST] stage 8s THREW:", e) }
            selfTestStage8c.restart()
        }
    }
    Timer {
        id: selfTestStage8c
        interval: 700
        onTriggered: {
            try {
                const ai = OutputListModel.activeIndex()
                const bg = OutputListModel.styleBackground(ai)
                console.log("[SELFTEST] stage 8c styled state:", JSON.stringify(bg))
                SelfTest.grab("selfTestMonitorWall", "shot_monitor_wall_styled.png")
            } catch (e) { console.log("[SELFTEST] stage 8c THREW:", e) }
            selfTestStage8b.restart()
        }
    }
    Timer {
        id: selfTestStage8b
        interval: 1200
        onTriggered: {
            // Restore exactly what the stage touched (no test residue left in
            // the user's saved roster/styles).
            try {
                if (window.selfTestStyleRow >= 0) {
                    OutputListModel.setStyle(0, "")
                    StyleListModel.removeStyle(window.selfTestStyleRow)
                    window.selfTestStyleRow = -1
                }
            } catch (e) { console.log("[SELFTEST] cleanup failed:", e) }
            selfTestHeartbeat.running = false
            SelfTest.quit()
        }
    }
    Timer {
        id: selfTestStage4d
        running: false
        interval: 9999999
    }

    // ---- TEMP: reproduce "text missing on the real output window, bg image
    // shows fine" — go live on a real text slide and let a few real frames
    // render so DrawTextObject's TEMPDIAG logging fires, then quit.
    Timer {
        id: selfTestTextDiag
        running: typeof SelfTest !== "undefined"
        interval: 1500
        onTriggered: {
            const peeked = ShowService.peekShow("C:/Users/znwaj/OneDrive/Documents/VGR Presenter/Shows/FreeShow/1088-WE ARE TRAVELLING ON THE RIGHT ROAD.vgr")
            console.log("[SELFTEST-TEXTDIAG] peek ok=" + peeked.ok)
            const slide0 = peeked.show && peeked.show.slides && peeked.show.slides.length > 0 ? peeked.show.slides[0] : null
            if (slide0)
                LiveOutputService.goLiveWithSlides("1088-WE ARE TRAVELLING ON THE RIGHT ROAD", [slide0])
            selfTestTextDiag2.restart()
        }
    }
    Timer {
        id: selfTestTextDiag2
        interval: 2500
        onTriggered: {
            console.log("[SELFTEST-TEXTDIAG] done waiting for frames")
            SelfTest.quit()
        }
    }





    // ---- SELFTEST: video preview pane scenario (env-gated by the same
    // VGR_SELFTEST=1 the driver uses; inert in normal runs). Drives the REAL
    // UI into the Add Source dialog's video preview and grabs the pane —
    // pixel truth for "is the live camera actually rendering".
    Timer {
        id: selfTestPreviewStage1
        running: typeof SelfTest !== "undefined"
        interval: 1500
        onTriggered: {
            console.log("[SELFTEST-PREVIEW] stage 1: open Settings > Audio & Video")
            window.openSettings("av")
            selfTestPreviewStage2.restart()
        }
    }
    Timer {
        id: selfTestPreviewStage2
        interval: 800
        onTriggered: {
            console.log("[SELFTEST-PREVIEW] stage 2: open Add Source dialog")
            const screen = SelfTest.findItem("audioVideoScreen")
            if (screen && screen.openAddSource) {
                screen.openAddSource("video")
                console.log("[SELFTEST-PREVIEW] openAddSource(video) invoked")
            } else {
                console.log("[SELFTEST-PREVIEW] FAIL: audioVideoScreen not found")
            }
            selfTestPreviewStage3.restart()
        }
    }
    // ALL-SOURCES sweep: every camera, then every display, then every open
    // window — each picked through the REAL dialog path, warmed up, logged
    // with the pane's live status, and pixel-grabbed. Per-source ground
    // truth for the whole preview chain (cams/boards/windows share it).
    property int selfTestCamIndex: -1
    property var selfTestCamLabels: []      // [{label, kind}] — kind: camera|screen
    property bool selfTestSweepDone: false
    function selfTestPickNextCam() {
        const screen = SelfTest.findItem("audioVideoScreen")
        selfTestCamIndex++
        if (!screen || !screen.selfTestPickVideoSource
                || selfTestCamIndex >= selfTestCamLabels.length) {
            console.log("[SELFTEST-PREVIEW] sweep done over", selfTestCamLabels.length, "source(s)")
            selfTestSweepDone = true
            return
        }
        const entry = selfTestCamLabels[selfTestCamIndex]
        console.log("[SELFTEST-PREVIEW] picking", entry.kind, selfTestCamIndex, ":", entry.label)
        screen.selfTestPickVideoSource(entry.label, entry.kind)
        selfTestPreviewStage4.restart()
    }
    Timer {
        id: selfTestPreviewStage3
        interval: 500
        onTriggered: {
            const entries = []
            const cams = EngineBridge.videoDevices
            for (let i = 0; i < cams.length; ++i) {
                const l = String(cams[i].label || "")
                // NDI virtual cams crash their driver when opened as MF
                // capture devices — they are served by the NDI pipeline,
                // not this tap; the sweep must not exercise them.
                if (l !== "" && !/ndi/i.test(l)) entries.push({label: l, kind: "camera"})
            }
            // Screens: displays + open windows, same pane chain.
            const scr = EngineBridge.screenDevices
            for (let i = 0; i < scr.length; ++i) {
                const l = String(scr[i].label || "")
                if (l !== "") entries.push({label: l, kind: "screen"})
            }
            window.selfTestCamLabels = entries
            window.selfTestCamIndex = -1
            console.log("[SELFTEST-PREVIEW] stage 3: sweeping", entries.length,
                        "source(s):", JSON.stringify(entries))
            window.selfTestPickNextCam()
        }
    }
    // Second window grab 2.5 s after cam 0's — if the live frame MOVES
    // between the two window shots, rendering + streaming both work.
    Timer {
        id: selfTestPreviewStage4b
        interval: 2500
        running: false
        onTriggered: {
            console.log("[SELFTEST-PREVIEW] stage 4b: motion witness grab")
            const paneItem = SelfTest.findItem("selfTestPreviewPane")
            if (paneItem) {
                // Window-space rect of the pane — the pixel-diff target
                // between shot_window_0.png and this grab.
                const r = paneItem.mapToItem(null, 0, 0, paneItem.width, paneItem.height)
                console.log("[SELFTEST-PREVIEW] pane rect: x=" + Math.round(r.x) + " y=" + Math.round(r.y)
                            + " w=" + Math.round(r.width) + " h=" + Math.round(r.height))
            }
            SelfTest.grab("", "shot_window_0_late.png")
        }
    }
    Timer {
        id: selfTestPreviewStage4
        interval: 4000   // tap warm-up + several pump ticks per camera
        running: false
        onTriggered: {
            if (window.selfTestSweepDone)
                return
            const entry = window.selfTestCamIndex < window.selfTestCamLabels.length
                          ? window.selfTestCamLabels[window.selfTestCamIndex] : null
            const label = entry ? entry.label : "?"
            const shot = "shot_src_" + window.selfTestCamIndex + "_"
                         + (entry ? entry.kind : "?") + ".png"
            const paneItem = SelfTest.findItem("selfTestPreviewPane")
            const frameImg = SelfTest.findItem("selfTestPreviewPaneImage")
            console.log("[SELFTEST-PREVIEW] stage 4: grabbing", shot, "for", label,
                        "pane:", (paneItem ? "alive" : "MISSING"),
                        "image:", frameImg
                            ? ("status=" + frameImg.status + " size=" + frameImg.sourceSize.width + "x" + frameImg.sourceSize.height
                               + " src=" + String(frameImg.source).slice(0, 48) + " vis=" + frameImg.visible) : "MISSING")
            // Pixel truth at three levels: the Image element alone, the
            // pane subtree, and the WHOLE WINDOW (what a user actually
            // sees). If the window shot shows the camera but the pane shot
            // is dark, the defect is grab compositing, not the app.
            SelfTest.grab("selfTestPreviewPaneImage",
                          "shot_img_" + window.selfTestCamIndex + "_"
                          + (entry ? entry.kind : "?") + ".png")
            SelfTest.grab("selfTestPreviewPane", shot)
            SelfTest.grab("", "shot_window_" + window.selfTestCamIndex + ".png")
            if (window.selfTestCamIndex === 0)
                selfTestPreviewStage4b.restart()   // motion witness for cam 0
            window.selfTestPickNextCam()
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
}
