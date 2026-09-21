import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

// The Edit screen: the slide canvas editor shown when the "Edit" tab is
// active. Faithfully matches VGRPresenter_Main_Screen_Edit.qml (the
// Figma-to-Qt ground truth), with the shared header chrome swapped for the
// AppMenuBar/ViewTabs/HeaderStatus components already used by the Show
// screen, and the repeated slide-list / output-monitor rows built from data
// (same content, less duplication) instead of copy-pasted blocks.
//
// Literal colors throughout, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml/DropdownPanel.qml at this nesting depth.
Rectangle {
    id: root

    clip: true
    color: "#12131a"

    // Mock CRUD backend (qml/models/SlideListModel.cpp) — stands in for the
    // real show/slide data source. Seeded with the same ground-truth
    // content the static array used to hold, but "Add slide" and selecting
    // a row now mutate real model state instead of pointing at fixed data.
    SlideListModel {
        id: slideModel
    }

    // Per-slide canvas state (items + background), keyed by the model's
    // stable slide id — switching slides swaps the canvas, duplicating a
    // slide clones its canvas, deleting one drops its archive.
    SlideCanvasStore {
        id: slideStore
    }

    // The seam to the ENGINE, which owns the show (slides, items, duplicate/delete,
    // ids, the .vgr file). Slide and item actions below ask it first; the list and
    // canvas are its projection plus the live gesture state it is synced from. See
    // ShowSession.qml.
    ShowSession {
        id: showSession
        slideModel: slideModel
        slideStore: slideStore
        canvasHistory: canvasHistory
    }
    readonly property var session: showSession

    // Every settled edit (and every undo/redo) is sent on to the engine.
    Connections {
        target: canvasHistory
        function onCommitted() { showSession.scheduleFlush() }
    }

    // Switching slides swaps the canvas: the store re-archives the outgoing
    // slide's working set and installs the incoming one. addSlide()
    // auto-selects, so a fresh slide lands here with an empty canvas too.
    Connections {
        target: slideModel
        function onActiveSlideChanged() {
            // The undo stack's snapshots are of the ACTIVE slide's canvas only
            // (no slide id), so replaying one after the slide changed would
            // paint the old slide's items over the new one. Drop the history
            // (and any open gesture/typing bracket) whenever the active slide
            // changes — that also covers "New show", which clears the deck and
            // adds a fresh slide. Session flags reset with it: the delegates
            // die with the swap, and a destroyed TextEdit never reports
            // editingChanged, which would leave them stale.
            // The outgoing slide's on-screen state goes to the engine first (a no-op
            // when it has not changed, and skipped while the UI is being rebuilt).
            showSession.flushSlide(slideStore.activeSlideId)
            canvasHistory.clear()
            root.textUndoKey = ""
            root.anyTextEditing = false
            slideStore.load(slideModel.activeSlideId)
        }
    }

    // Same tie-to-parent rule for the canvas context menu: if the item it
    // was opened for disappears while the menu is up (Delete key with the
    // canvas focused, an undo that restores an earlier item set, ...), the
    // menu closes instead of offering Duplicate/Delete on a ghost.
    Connections {
        target: slideStore.current
        function onItemsChanged() {
            if (root.canvasContextTarget !== "" && !root.canvasObjectByKey(root.canvasContextTarget)) {
                canvasContextMenu.visible = false
                root.canvasContextTarget = ""
            }
        }
    }

    // True when a slide is active — the roster starts EMPTY (no hardcoded
    // slides), so this is false on a fresh launch until "Add slide" is used.
    // Drives the empty state: canvas and right panel grey out (nothing to
    // edit), while the slide list stays fully live (adding a slide is the
    // way out).
    readonly property bool hasActiveSlide: slideModel.activeSlideId > 0

    // "New show": reset the deck to a fresh single slide. One call drops
    // every canvas archive first, then every slide — order matters, since
    // a removed ACTIVE slide fires onActiveSlideChanged and would load a
    // stale archive for the next auto-selected slide if its canvas still
    // existed. The final addSlide() gives the user the empty slide New
    // Show means (matching the roster's empty-at-launch posture: never a
    // hardcoded slide, never a bare roster).
    function newShow() {
        // The engine starts the new document; the UI resets to its single empty slide
        // (asking first if the current show has unsaved changes).
        showSession.newShow()
    }
    // Show file actions (menu bar): the engine reads/writes the .vgr; these keep the
    // UI in step and prompt about unsaved changes.
    function openShow() { showSession.openShow() }
    // Opens a library path directly (shows-table row click, Main.qml routes it here).
    function openShowPath(path) { showSession.openShowPath(path) }
    function saveShow() { return showSession.saveShow() }
    function saveShowAs() { return showSession.saveShowAs() }
    // UI self-test helper (Main.qml scenario): resolve a named item into
    // WINDOW coordinates by recursively walking this screen's item tree.
    // C++ findChild can't reach some QML-created items (Repeater delegates
    // exist — the row dump proves it — but are invisible to QObject-name
    // search), so the QML side does the walking. Invisible branches are
    // skipped so a closed menu/modal resolves to null, not a stale point.

    // TEMP DIAGNOSTIC — reproduce "type text, exit, re-enter and type more,
    // undo WHILE still in that second edit session" exactly. Flip to true
    // ONLY when debugging an undo regression: it seeds a slide + text item
    // on every launch (so the app never starts in the empty, greyed-out
    // "no active slide" state while it's on) and drives the scripted
    // type/undo sequence, logging [DIAG] lines to stderr. Default off —
    // the app must start empty, which is what the regression it found (the
    // mid-edit junk-command push) is now verified fixed.
    property bool diagUndoHarness: false
    Component.onCompleted: {
        if (!root.diagUndoHarness)
            return
        slideModel.addSlide()
        const freshItem = root.addCanvasItem("text")
        root.handleCanvasSelect(freshItem.key, 0)
        function afterMs(ms, fn) {
            const t = Qt.createQmlObject('import QtQuick; Timer { interval: ' + ms + '; running: true; repeat: false }', root, "diagTimer")
            t.triggered.connect(fn)
        }
        afterMs(200, function () {
            // Fresh reference — addCanvasItem's own deferred "after" commit
            // (Qt.callLater) already fired by now and destroyed+recreated
            // every item as a side effect (restoreItems' redundant re-
            // apply), so the ORIGINAL `item` capture is stale/destroyed.
            const item = slideStore.current.items[0]
            console.log("[DIAG] fresh item key=", item.key)
            // Session 1: type "AAA", then commit (leave edit mode).
            root.beginTextEdit(item.key)
            item.text = "AAA"
            root.endTextEdit()
            afterMs(200, function () {
                console.log("[DIAG] after session 1 commit: text=", slideStore.current.items[0].text, "canUndo=", canvasHistory.canUndo)
                // Session 2: re-enter edit on the SAME item, type more —
                // but do NOT end the session yet.
                const item2 = slideStore.current.items[0]
                root.beginTextEdit(item2.key)
                item2.text = "AAABBB"
                console.log("[DIAG] mid session 2 (still editing): text=", item2.text, "anyTextEditing=", root.anyTextEditing)
                // Undo WHILE still mid-session-2 — the exact repro.
                root.undo()
                console.log("[DIAG] after undo WHILE mid-session-2: text=", slideStore.current.items[0].text,
                             "canUndo=", canvasHistory.canUndo, "canRedo=", canvasHistory.canRedo,
                             "anyTextEditing=", root.anyTextEditing)
                // One tick later: the old endTextEdit->commit chain (fired
                // by the undo's apply() recreating the delegate) used to
                // push a JUNK command here ("do: restore post-undo state",
                // undo: re-apply "AAA"), clearing canRedo with it — so the
                // NEXT Ctrl+Z resurrected the text that was just undone
                // ("undo undoes the undo"). With abandonPending in place
                // canRedo must STILL be true.
                afterMs(50, function () {
                    console.log("[DIAG] commit-tick: text=", slideStore.current.items[0].text,
                                 "canUndo=", canvasHistory.canUndo, "canRedo=", canvasHistory.canRedo,
                                 "(canRedo MUST still be true — no junk step)")
                    // Drains the last real command (session 1's edit, then
                    // the add itself — whose "before" is an EMPTY canvas, so
                    // items[0] legitimately no longer exists afterwards).
                    // The old junk command would instead have re-applied
                    // "AAA" here and left an extra entry on the stack.
                    root.undo()
                    const drained = slideStore.current.items.length === 0
                    console.log("[DIAG] second undo: items empty=", drained,
                                 "(expect true — the add command's before-state)",
                                 "text=", drained ? "(no items)" : slideStore.current.items[0].text,
                                 "canUndo=", canvasHistory.canUndo, "(expect false — stack drained)",
                                 "canRedo=", canvasHistory.canRedo,
                                 "(expect true — redo of session 1 intact)")
                })
            })
        })
    }

    // Output roster comes from the OutputListModel singleton (qml/models/OutputListModel.cpp)
    // — the SAME model Settings · Outputs (OutputsScreen.qml) edits, so adding
    // or renaming an output there shows up here too instead of two arrays
    // drifting apart.

    // Right-click context menu for slide rows (Edit / Duplicate / Delete) —
    // see slideContextMenu near the end of this file. Tracks which row it
    // was opened for, since only one shared DropdownPanel instance exists.
    property int contextMenuSlideIndex: -1

    // Right panel's ITEMS/TEXT/SLIDE tab indicator — see that Row in the
    // right panel below. Purely a visual toggle for now: the ground truth
    // export never captured what TEXT/SLIDE should actually show, so the
    // panel content underneath doesn't vary by tab yet.
    property string rightPanelTab: "items"
    // Leaving the TEXT tab closes its font-weight dropdown — an open menu
    // belonging to a tab you've left is exactly the "not tied to its
    // parent" class of bug.
    onRightPanelTabChanged: textFontMenu.visible = false

    // Background-color picker (see bgColorModal near the end of this file)
    // and the current selection it applies to the "Background" row's swatch.
    // bgModalTarget says which row opened it ("background", "itemBackground"
    // or "border" — see SizeStyleCard's Border row), so one shared picker
    // instance can serve both instead of duplicating it.
    property bool bgModalOpen: false
    property string bgModalTarget: "background"

    // Which canvas object(s) are currently selected — drives which ones show
    // the border+handle "selected" chrome (see DraggableCanvasText). Empty
    // means nothing selected (clicking empty canvas clears it, and a slide
    // starts with none). A plain or Ctrl+click replaces the selection with
    // just that one; Shift+click toggles it in/out of a multi-selection.
    property var selectedCanvasObjects: []

    // Canvas items live in slideStore (per-slide, archived on slide switch)
    // — no screen-level array anymore.

    // Canvas zoom — a plain visual scale (see mCanvas's transformOrigin/
    // scale below), clamped to a modest range since the canvas's own
    // clip:true still clips to its unscaled bounds (zooming in crops the
    // outer edges rather than revealing more canvas via panning — panning
    // is future work, not built here).
    property real canvasZoom: 1

    // ---- Keyboard shortcuts --------------------------------------------
    // One data map, not scattered hardcoded Keys.onPressed conditionals —
    // every Shortcut element below reads its sequence from here, so a
    // future rebind-shortcuts settings screen only has to change this map,
    // never touch the Shortcut elements or add new key-handling code.
    // Delete/Backspace/Escape/arrow-nudge stay on mCanvas's own
    // Keys.onPressed (below) rather than moving here — those are near-
    // universal editing conventions, not the app-specific actions this was
    // asked for (undo/redo/copy/paste/add-content).
    property var shortcutBindings: ({
        undo: "Ctrl+Z",
        redo: "Ctrl+Shift+Z",
        // The second-standard redo chord — see the Ctrl+Y Shortcut below.
        redoAlt: "Ctrl+Y",
        copy: "Ctrl+C",
        paste: "Ctrl+V",
        duplicateSelected: "Ctrl+D",
        addText: "T",
        addCamera: "Shift+C"
    })

    // ---- Undo/redo -------------------------------------------------------
    // A reusable UndoHistory (see UndoHistory.qml) — now itself backed by
    // the real engine's UndoRedoManager rather than a local stack, so this
    // screen just teaches it how to capture/apply the whole per-slide
    // canvas state ({ items, background }); the ordering/depth-cap
    // bookkeeping lives in bps::project::UndoRedoManager. Each surface that
    // wants undo instantiates its own UndoHistory (see its header comment
    // for the full contract — push(label)/push(label,false)+commit()/
    // push()).
    //
    // One entry per whole gesture, never per intermediate value: drags
    // collapse via dragSnapshotTaken (below), a typing session via
    // beginTextEdit (below), slider gestures via SizeStyleCard/
    // TextItemPanel's undoHook (wired straight to canvasHistory.push below
    // — its own no-arg settle-debounce handles those). Slide-level
    // operations (add/remove/duplicate slide) are NOT yet covered — see
    // KNOWN_ISSUES.md if that scope grows later.
    UndoHistory {
        id: canvasHistory
        capture: function () {
            return { items: slideStore.snapshotItems(), background: slideStore.current.background }
        }
        apply: function (snap) {
            slideStore.restoreItems(snap.items)
            slideStore.current.background = snap.background
        }
        // Restored items are fresh objects; keys survive restore, object
        // references don't — the selection is transient state referencing
        // exactly those, so it resets after every undo/redo.
        afterRestore: function () { root.selectedCanvasObjects = [] }
    }

    // undoHook target for SizeStyleCard.qml/TextItemPanel.qml — a plain
    // reference to canvasHistory.push, called with no arguments, which is
    // exactly UndoHistory's own settle-debounced mode (right for a
    // component that only fires "drag started" with no matching "drag
    // finished" signal back to this screen).
    function pushUndoSnapshot() {
        canvasHistory.push()
    }

    // Undo/redo also pull keyboard focus back to the canvas: a chord
    // pressed right after clicking a panel control otherwise fires while
    // the panel's TextInput still owns focus, which native-edit behavior
    // would route into that field instead of the canvas history.
    //
    // Safe to fire mid-text-edit too (the Shortcuts below explicitly stay
    // enabled while root.anyTextEditing): canvasHistory.undo() ABANDONS the
    // open session's pending push before touching the engine stack (see
    // UndoHistory.abandonPending), so no junk command can be committed from
    // the post-undo state when the interrupted session ends.
    //
    // Also resets the session bookkeeping explicitly: undo()'s apply()
    // destroys and recreates the delegates, and a TextEdit destroyed
    // mid-edit never emits onEditingChanged — so textUndoKey/anyTextEditing
    // would stay stale (anyTextEditing stuck true; worse, a beginTextEdit on
    // the SAME restored key early-returns and its typing would never be
    // undoable). The pending snapshot was already dropped inside
    // canvasHistory.undo(); this only clears the flags.
    function undo() {
        canvasHistory.undo()
        root.textUndoKey = ""
        root.anyTextEditing = false
        mCanvas.forceActiveFocus()
    }
    function redo() {
        canvasHistory.redo()
        root.textUndoKey = ""
        root.anyTextEditing = false
        mCanvas.forceActiveFocus()
    }

    // Set on the first applyCanvasMove/applyCanvasResize call of a drag
    // gesture, cleared in endCanvasDrag() — collapses a whole drag into one
    // undo step instead of one per mouse-move event.
    property bool dragSnapshotTaken: false

    // Brackets one typing session (double-click edit → commit) as a single
    // undo step. Text edits previously weren't undoable at all: every other
    // edit path pushes BEFORE mutating, but keystrokes arrive after the
    // first change has already happened — so the push fires on the first
    // edited() keystroke of a session, and only once per session (the key
    // resets on commit/Escape via endTextEdit). Waiting one keystroke means
    // the pushed snapshot still holds the pre-typing text, because the
    // model write happens right after the push in the same handler.
    property string textUndoKey: ""
    // True the instant ANY item's edit mode actually starts (set by the
    // Repeater delegate's onEditingChanged below) — unlike textUndoKey,
    // which stays "" until the session's first keystroke. Undo/redo need
    // THIS one: pressing Ctrl+Z right after double-clicking into edit mode,
    // before typing anything, has to work too.
    property bool anyTextEditing: false
    function beginTextEdit(key) {
        if (root.textUndoKey === key)
            return
        root.textUndoKey = key
        canvasHistory.push(qsTr("Edit text"), false)
    }
    function endTextEdit() {
        root.textUndoKey = ""
        canvasHistory.commit()
    }

    // ---- Copy/paste --------------------------------------------------
    property var clipboardItems: []

    function copySelectedCanvasObjects() {
        const all = slideStore.snapshotItems()
        root.clipboardItems = all.filter((d) => root.selectedCanvasObjects.indexOf(d.key) >= 0)
    }

    function pasteClipboardItems() {
        if (root.clipboardItems.length === 0)
            return
        const newKeys = []
        root.clipboardItems.forEach((d) => {
            const item = root.addCanvasItem(d.kind)
            item.text = d.text
            item.x = d.x + 16
            item.y = d.y + 16
            item.width = d.width
            item.height = d.height
            item.meta = d.meta
            item.style.padding = d.style.padding
            item.style.backgroundColor = d.style.backgroundColor
            item.style.cornerRadius = d.style.cornerRadius
            item.style.borderEnabled = d.style.borderEnabled
            item.style.borderWidth = d.style.borderWidth
            item.style.borderStyle = d.style.borderStyle
            item.style.borderColor = d.style.borderColor
            newKeys.push(item.key)
        })
        root.selectedCanvasObjects = newKeys
        // UI-originated notification — the STANDARD way: through EventBus's
        // notify() (stamps level/title/message/origin=ui and namespaces the
        // topic under ui.*), so NotificationCenter toasts it and any other
        // subscriber sees it exactly like engine traffic. Screens never call
        // NotificationCenter directly and never hand-roll payload maps.
        EventBus.notify(qsTr("%1 item(s) pasted").arg(newKeys.length),
                        "success", "Edit", "ui.edit.itemsPasted")
    }

    // Right-click context menu for canvas text objects (Edit / Duplicate /
    // Delete) — see canvasContextMenu near the end of this file. Tracks
    // which object it was opened for, since only one shared DropdownPanel
    // instance exists (same pattern as slideContextMenu above).
    property string canvasContextTarget: ""

    function isCanvasObjectSelected(key) {
        return root.selectedCanvasObjects.indexOf(key) >= 0
    }
    function handleCanvasSelect(key, modifiers) {
        if (modifiers & Qt.ShiftModifier) {
            const idx = root.selectedCanvasObjects.indexOf(key)
            if (idx >= 0) {
                const copy = root.selectedCanvasObjects.slice()
                copy.splice(idx, 1)
                root.selectedCanvasObjects = copy
            } else {
                root.selectedCanvasObjects = root.selectedCanvasObjects.concat([key])
            }
        } else {
            // Plain click and Ctrl+click both just select this one object.
            root.selectedCanvasObjects = [key]
        }
        // So arrow-key nudge / Delete (see mCanvas's Keys.onPressed below)
        // work immediately after a select click, without the user having
        // to click empty canvas first to move focus off whatever was
        // mid-edit.
        mCanvas.forceActiveFocus()
    }

    // Every item's x/y/width/height *are* the data a Repeater delegate binds
    // to, so writes through this function (from applyCanvasMove/
    // applyCanvasResize below) propagate visually automatically.
    function canvasObjectByKey(key) {
        const items = slideStore.current.items
        for (let i = 0; i < items.length; ++i) {
            if (items[i].key === key)
                return items[i]
        }
        return null
    }

    // Drives the top-of-panel "Background" row further down: it edits
    // whichever item is primary-selected instead of the slide's own
    // background when something is selected, rather than Size & Style
    // duplicating the same swatch+chip control a second time.
    readonly property CanvasItemStyle primarySelectedItemStyle: root.selectedCanvasObjects.length > 0
        ? (root.canvasObjectByKey(root.selectedCanvasObjects[0])?.style ?? null)
        : null

    // The raw CanvasItem behind primarySelectedItemStyle — TextItemPanel
    // needs the whole item (meta, x/y/width/height), not just its style.
    readonly property var primarySelectedItem: root.selectedCanvasObjects.length > 0
        ? root.canvasObjectByKey(root.selectedCanvasObjects[0])
        : null

    // The font-weight dropdown (textFontMenu) is opened FOR this item, so
    // it must not outlive it: deleting the item, clicking empty canvas or
    // switching slides all drop this to null, and the menu closes with it
    // instead of floating over the panel with nothing left to apply to.
    onPrimarySelectedItemChanged: {
        if (!root.primarySelectedItem)
            textFontMenu.visible = false
    }

    // Background/Border only have a visible effect on a "text", "shape",
    // "clock" or "timer" item — camera's live-preview Shape and the generic
    // media/audio placeholder still fill their box edge-to-edge with their
    // own opaque visual, completely covering whatever the style background/
    // border would draw underneath, so those stay greyed out. clock/timer
    // (like shape) draw their own background Rectangle straight from the
    // item's style now (see clockContent/timerContent below), so these
    // controls are genuinely live for them, not just for text/shape.
    readonly property bool primarySelectedSupportsFill: root.primarySelectedItemStyle === null
        || ["text", "shape", "clock", "timer"].includes(root.primarySelectedItemStyle.kind)

    // ---- Alignment-guide snapping helpers ----
    // Pure geometry math (modeled after FreeShow's src/frontend/components/
    // system/textbox.ts snapBox/checkMatch), kept as plain functions here
    // rather than a separate .js module: this file is their only consumer,
    // and a standalone .js sits outside this project's qml/*.qml glob in
    // CMakeLists.txt, so it silently isn't bundled unless the build is
    // taught about a second file type for one file's sake — not worth it.
    readonly property int snapDistance: 8

    // canvasWidth/Height plus every "other" object's {x,y,width,height}
    // become candidate lines: canvas edges + center, and each other
    // object's edges + center, on each axis.
    function snapCandidates(canvasWidth, canvasHeight, others) {
        const xs = [0, canvasWidth / 2, canvasWidth]
        const ys = [0, canvasHeight / 2, canvasHeight]
        others.forEach((o) => {
            xs.push(o.x, o.x + o.width / 2, o.x + o.width)
            ys.push(o.y, o.y + o.height / 2, o.y + o.height)
        })
        return { xs: xs, ys: ys }
    }

    function snapClosest(value, candidates) {
        let best = null
        let bestDist = root.snapDistance
        candidates.forEach((c) => {
            const d = Math.abs(value - c)
            if (d < bestDist) {
                bestDist = d
                best = c
            }
        })
        return best
    }

    // Snap a translate (move): checks the box's left/center/right against x
    // candidates and top/center/bottom against y candidates, snapping
    // whichever edge/center is closest (center checked first since it's
    // usually the more meaningful alignment).
    function snapMove(x, y, width, height, canvasWidth, canvasHeight, others) {
        const cand = root.snapCandidates(canvasWidth, canvasHeight, others)
        const guides = []

        let snappedX = x
        for (const ex of [x + width / 2, x, x + width]) {
            const mx = root.snapClosest(ex, cand.xs)
            if (mx !== null) {
                snappedX = x + (mx - ex)
                guides.push({ axis: "x", pos: mx })
                break
            }
        }

        let snappedY = y
        for (const ey of [y + height / 2, y, y + height]) {
            const my = root.snapClosest(ey, cand.ys)
            if (my !== null) {
                snappedY = y + (my - ey)
                guides.push({ axis: "y", pos: my })
                break
            }
        }

        return { x: snappedX, y: snappedY, guides: guides }
    }

    // Snap a resize: only the edge(s) actually being dragged (per the
    // left/right/top/bottom flags) are candidates for snapping, and only
    // that edge moves — the opposite edge stays anchored, same as the
    // underlying resize math already does.
    function snapResize(x, y, width, height, canvasWidth, canvasHeight, others, activeLeft, activeRight, activeTop, activeBottom) {
        const cand = root.snapCandidates(canvasWidth, canvasHeight, others)
        const guides = []

        let rx = x, rw = width
        if (activeRight) {
            const mr = root.snapClosest(x + width, cand.xs)
            if (mr !== null) {
                rw = Math.max(1, mr - x)
                guides.push({ axis: "x", pos: mr })
            }
        } else if (activeLeft) {
            const ml = root.snapClosest(x, cand.xs)
            if (ml !== null) {
                rw = Math.max(1, width + (x - ml))
                rx = x + width - rw
                guides.push({ axis: "x", pos: ml })
            }
        }

        let ry = y, rh = height
        if (activeBottom) {
            const mb = root.snapClosest(y + height, cand.ys)
            if (mb !== null) {
                rh = Math.max(1, mb - y)
                guides.push({ axis: "y", pos: mb })
            }
        } else if (activeTop) {
            const mt = root.snapClosest(y, cand.ys)
            if (mt !== null) {
                rh = Math.max(1, height + (y - mt))
                ry = y + height - rh
                guides.push({ axis: "y", pos: mt })
            }
        }

        return { x: rx, y: ry, width: rw, height: rh, guides: guides }
    }

    // Group-drag + snap state for a body-drag in progress (see
    // applyCanvasMove, wired to every EditableCanvasLabel's moveRequested).
    // Keyed so a second object's drag starting mid-gesture (shouldn't
    // happen, but defensively) resets cleanly instead of mixing deltas.
    property var canvasMoveDrag: null

    function applyCanvasMove(key, dx, dy, snapDisabled) {
        if (!root.canvasMoveDrag || root.canvasMoveDrag.key !== key) {
            canvasHistory.push(qsTr("Move"), false)
            const positions = {}
            root.selectedCanvasObjects.forEach((k) => {
                const o = root.canvasObjectByKey(k)
                if (o) positions[k] = { x: o.x, y: o.y }
            })
            root.canvasMoveDrag = { key: key, startPositions: positions, accumDx: 0, accumDy: 0 }
        }
        root.canvasMoveDrag.accumDx += dx
        root.canvasMoveDrag.accumDy += dy

        const primaryStart = root.canvasMoveDrag.startPositions[key]
        const primaryObj = root.canvasObjectByKey(key)
        let finalDx = root.canvasMoveDrag.accumDx
        let finalDy = root.canvasMoveDrag.accumDy

        if (!snapDisabled && primaryStart && primaryObj) {
            const others = []
            slideStore.current.items.forEach((it) => {
                if (root.selectedCanvasObjects.indexOf(it.key) >= 0) return
                others.push({ x: it.x, y: it.y, width: it.width, height: it.height })
            })
            const snap = root.snapMove(primaryStart.x + finalDx, primaryStart.y + finalDy, primaryObj.width, primaryObj.height, mCanvas.width, mCanvas.height, others)
            finalDx = snap.x - primaryStart.x
            finalDy = snap.y - primaryStart.y
            canvasSnapGuides.guides = snap.guides
        } else {
            canvasSnapGuides.guides = []
        }

        root.selectedCanvasObjects.forEach((k) => {
            const start = root.canvasMoveDrag.startPositions[k]
            const o = root.canvasObjectByKey(k)
            if (o && start) {
                o.x = start.x + finalDx
                o.y = start.y + finalDy
            }
        })
    }

    function applyCanvasResize(key, geom) {
        const primaryObj = root.canvasObjectByKey(key)
        if (!primaryObj)
            return

        if (geom.snapDisabled) {
            canvasSnapGuides.guides = []
            return
        }

        // One snapshot per whole resize gesture, not per mouse-move event
        // (applyCanvasResize fires continuously while dragging a handle) —
        // see dragSnapshotTaken's header comment and endCanvasDrag below.
        if (!root.dragSnapshotTaken) {
            canvasHistory.push(qsTr("Resize"), false)
            root.dragSnapshotTaken = true
        }

        const others = []
        slideStore.current.items.forEach((it) => {
            if (it.key === key) return
            others.push({ x: it.x, y: it.y, width: it.width, height: it.height })
        })
        const snap = root.snapResize(geom.x, geom.y, geom.width, geom.height, mCanvas.width, mCanvas.height, others, geom.left, geom.right, geom.top, geom.bottom)
        primaryObj.x = snap.x
        primaryObj.y = snap.y
        primaryObj.width = snap.width
        primaryObj.height = snap.height
        canvasSnapGuides.guides = snap.guides
    }

    function endCanvasDrag() {
        root.canvasMoveDrag = null
        root.dragSnapshotTaken = false
        canvasSnapGuides.guides = []
        canvasHistory.commit()
    }

    function nudgeSelectedCanvasObjects(dx, dy) {
        canvasHistory.push(qsTr("Nudge"))
        root.selectedCanvasObjects.forEach((k) => {
            const o = root.canvasObjectByKey(k)
            if (o) {
                o.x += dx
                o.y += dy
            }
        })
    }

    // Delete actually removes an item from the canvas entirely — nothing is
    // a permanent slot anymore. Shared by deleteSelectedCanvasObjects below
    // and canvasContextMenu's own "Delete" case, so the two delete paths
    // can't drift apart.
    function removeCanvasItems(keys) {
        canvasHistory.push(qsTr("Delete"))
        showSession.removeItems(keys)   // the engine deletes them first
        slideStore.removeItems(keys)
        root.selectedCanvasObjects = root.selectedCanvasObjects.filter((k) => keys.indexOf(k) < 0)
    }

    function deleteSelectedCanvasObjects() {
        root.removeCanvasItems(root.selectedCanvasObjects)
        root.selectedCanvasObjects = []
    }

    // Entry point for the "+" Add Content menu below — creates a new item
    // of the clicked kind, drops it onto the canvas with a small cascading
    // offset per existing item (so repeated adds don't stack exactly on top
    // of each other), and selects it.
    // Creates a new item of the given kind ("+" Add Content menu) — or, when
    // `copyFrom` is given (the context menu's Duplicate), clones an existing
    // item's geometry/text/style and lands the copy one tile down-right.
    // The item is created and owned by the slide store so it archives with
    // its slide. Returns the new CanvasItem.
    function addCanvasItem(kind, copyFrom) {
        // Deferred: several callers (CameraSourceModal.onApplied etc.) set
        // item.text/item.meta right after this returns, in the same tick —
        // the deferred commit fires after those too, so the undo step
        // captures the fully-configured item, not a half-built one.
        canvasHistory.push(copyFrom ? qsTr("Duplicate item") : qsTr("Add item"))
        const n = slideStore.current.items.length
        const isCamera = kind === "camera"
        // The ENGINE creates the item (or duplicates the source) and issues its id;
        // the item — text, geometry, style, meta and all — comes back as a live
        // CanvasItem already on the slide. showSession.addItem returns null if the
        // engine refused (no show / no slide), and nothing is added then.
        showSession.ensureDocument()
        const item = showSession.addItem(
            kind,
            40 + (n % 6) * 20,
            40 + (n % 6) * 20,
            isCamera ? 112 : 220,
            isCamera ? 84 : 44,
            copyFrom)
        if (!item)
            return null
        root.handleCanvasSelect(item.key, 0)
        return item
    }

    // Finds the live Repeater delegate for a given item key — items are
    // dynamically created (no fixed/named id the way the old title/verse/
    // ref/date objects had), so reaching one from outside the Repeater means
    // searching its instantiated delegates by their modelData.key.
    function delegateForKey(key) {
        for (let i = 0; i < canvasItemsRepeater.count; ++i) {
            const d = canvasItemsRepeater.itemAt(i)
            if (d && d.modelData && d.modelData.key === key)
                return d
        }
        return null
    }

    // sourceItem is whichever DraggableCanvasText fired contextMenuRequested
    // — mapToItem from it (not from root directly) since (mx, my) are in
    // that object's own local space. Clamped to root's current bounds, same
    // reasoning as slideContextMenu's positioning below.
    function openCanvasContextMenu(sourceItem, mx, my, key) {
        // Shared map+clamp helper (see slideContextMenu's usage above).
        canvasContextMenu.openAt(sourceItem, mx, my, root)
        root.canvasContextTarget = key
    }

    // Add-content chip + its popover menu (bottom of the canvas).
    property bool addMenuOpen: false
    // Opened instead of adding a camera/media/timer/clock/shape item
    // immediately — see cameraSourceModal/mediaSourceModal/timerSourceModal/
    // clockSourceModal/shapeSourceModal below and the matching branches in
    // the chip row's onClicked above.
    property bool cameraModalOpen: false
    property bool mediaModalOpen: false
    property bool timerModalOpen: false
    property bool clockModalOpen: false
    property bool shapeModalOpen: false
    readonly property var contentTypes: [
        // `icon` is an IconGlyph name (qml/components/IconGlyph.qml — Lucide
        // path data), not a Text glyph: the old "⏱"/"◷"/"◎" unicode
        // strings had no glyph coverage in Inter and rendered as missing-
        // glyph tofu boxes on Windows. Kind is the fallback for "text" —
        // plain letters need no icon.
        { kind: "text",   icon: "text",   label: "Text" },
        { kind: "camera", icon: "camera",  label: "Camera" },
        { kind: "media",  icon: "play",    label: "Media" },
        { kind: "audio",  icon: "music",   label: "Audio" },
        { kind: "shape",  icon: "shape",   label: "Shape" },
        { kind: "timer",  icon: "timer",   label: "Timer" },
        { kind: "clock",  icon: "clock",   label: "Clock" }
    ]

    // ---- Middle toolbar ----
    Rectangle {
        id: mHdr
        x: 280
        y: 48
        height: 48
        // Everything between the two side panels (the window is no longer a fixed 1440 wide).
        width: root.width - 680
        color: "#15161d"

        Rectangle {
            id: mBack
            x: 12
            y: 10
            height: 28
            width: 28
            color: "#1e1f29"
            radius: 7

            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -2
                color: "#eef0f6"
                font.family: "Inter"
                font.pixelSize: 13
                text: "‹"
            }
        }
        Text {
            x: 50
            y: 14
            color: "#eef0f6"
            font.family: "Inter"
            font.pixelSize: 13
            font.weight: Font.Medium
            text: qsTr("Sunday Service")
        }
        Rectangle {
            x: 156
            y: 12
            height: 24
            width: 104
            border.color: "#3a2230"
            border.width: 1
            color: "#2a1c24"
            radius: 6

            Text {
                anchors.centerIn: parent
                color: "#ff4d3d"
                font.family: "Inter"
                font.pixelSize: 9
                font.weight: Font.Medium
                text: qsTr("Template · Worship")
            }
        }
        // KERNEL-DRIVEN, not baked text: reflects real project.saved events
        // relayed from the engine (autosave flag included). Before the first
        // save it says so honestly instead of claiming an autosave that never
        // happened. Backfilled from the relay ring for events that fired
        // before this screen existed.
        Text {
            id: saveLabelText
            x: parent.width - 204   // right-aligned cluster: stays put at the bar's right end
            y: 17
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Not saved yet")
            function applySave(autosave) { text = autosave ? qsTr("Autosaved ✓") : qsTr("Saved ✓") }
            Component.onCompleted: {
                const recent = EngineBridge.recentEngineEvents(200)
                for (let i = recent.length - 1; i >= 0; --i)
                    if (recent[i].topic === "project.saved") {
                        applySave(recent[i].autosave)
                        break
                    }
            }
        }
        Connections {
            target: EngineBridge
            function onEngineEvent(topic, payload) {
                if (topic === "project.saved")
                    saveLabelText.applySave(payload && payload.autosave === true)
            }
        }
        Text {
            x: parent.width - 122
            y: 17
            color: fitArea.containsMouse ? "#c8cdd9" : "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Fit")
            Behavior on color { ColorAnimation { duration: 100 } }

            MouseArea {
                id: fitArea
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.canvasZoom = 1
            }
        }
        Row {
            x: parent.width - 98
            y: 10
            spacing: 4

            Rectangle {
                height: 28
                width: 28
                radius: 7
                enabled: canvasHistory.canUndo
                opacity: enabled ? 1 : 0.35
                color: undoArea.containsMouse ? "#20242f" : "#1a1c26"
                Behavior on color { ColorAnimation { duration: 100 } }

                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↺" }

                MouseArea {
                    id: undoArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.undo()
                }
            }
            Rectangle {
                height: 28
                width: 28
                radius: 7
                enabled: canvasHistory.canRedo
                opacity: enabled ? 1 : 0.35
                color: redoArea.containsMouse ? "#20242f" : "#1a1c26"
                Behavior on color { ColorAnimation { duration: 100 } }

                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↻" }

                MouseArea {
                    id: redoArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.redo()
                }
            }
            Rectangle {
                height: 28
                width: 34
                radius: 7
                color: zoomResetArea.containsMouse ? "#20242f" : "#1a1c26"
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    color: "#8a94a6"
                    font.family: "Inter"
                    font.pixelSize: 10
                    text: Math.round(root.canvasZoom * 100) + "%"
                }

                MouseArea {
                    id: zoomResetArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = 1
                }
            }
        }
    }
    Rectangle {
        x: 280
        y: 96
        height: 1
        width: root.width - 680
        color: "#232530"
    }

    // ---- Canvas ----
    // width: 754, not 760 — 286 + 760 = 1046 overlapped 6px into rightPanel
    // (x: 1040). 754 lands exactly on rightPanel's left edge.
    Rectangle {
        id: mCanvas
        // The design's slot at 1440 x 900, kept centred in the middle region as the window grows.
        x: 286 + (root.width - 1440) / 2
        y: 308 + (root.height - 900) / 2
        height: 428
        width: 754
        clip: true
        // Visual zoom (see root.canvasZoom's header comment) — scales from
        // the top-left corner so the canvas stays pinned to its own x/y
        // slot in the surrounding layout instead of drifting from the
        // default center-origin scale.
        transformOrigin: Item.TopLeft
        scale: root.canvasZoom
        // No active slide = nothing to edit: every child MouseArea (empty-
        // canvas click, drag areas, resize handles) and the keyboard nudge
        // go inert. The empty-state veil below lives OUTSIDE this item at
        // root level precisely so its own "Add slide" chip stays clickable.
        enabled: root.hasActiveSlide
        // Fill only — border is a separate top-layer Rectangle at the end
        // of this file's children (see mCanvasBorder below), not a border
        // set directly here: the checkerboard/background/content children
        // fill edge-to-edge with anchors.fill:parent and draw on top of
        // this Rectangle's own paint, which would hide a border set here
        // completely (the exact bug camPanel's border had earlier).
        color: "#141519"

        // Transparency checkerboard (FreeShow's Zoomed.svelte convention) —
        // sits behind slideBgLayer so it reads through wherever the slide
        // background is transparent or less than fully opaque, and gets
        // fully covered once a solid, fully-opaque background is chosen.
        //
        // Fixed whole-pixel tile size (16px), not width/columns — a
        // fractional size (e.g. 760/48 ≈ 15.83px) still looks uneven even
        // with exact index-based positioning, because the renderer snaps
        // each tile's fractional edges to the physical pixel grid
        // independently, and adjacent tiles can round differently. A whole-
        // pixel size rounds identically every time, so every tile is
        // pixel-for-pixel uniform. columns/rows overshoot the canvas
        // slightly (ceil, not exact division) and mCanvas's clip:true crops
        // the small remainder — invisible, and no worse than a partial tile
        // would look anyway.
        Item {
            id: checkerGrid
            anchors.fill: parent
            readonly property int tileSize: 16
            readonly property int columns: Math.ceil(width / tileSize)
            readonly property int rows: Math.ceil(height / tileSize)

            Repeater {
                model: checkerGrid.columns * checkerGrid.rows
                delegate: Rectangle {
                    required property int index
                    readonly property int col: index % checkerGrid.columns
                    readonly property int row: Math.floor(index / checkerGrid.columns)
                    x: col * checkerGrid.tileSize
                    y: row * checkerGrid.tileSize
                    width: checkerGrid.tileSize
                    height: checkerGrid.tileSize
                    color: (row + col) % 2 === 0 ? "#2a2c38" : "#15161d"
                }
            }
        }

        // Decorative alignment-grid dots — declared here, before
        // slideBgLayer, so a chosen background actually covers them like it
        // covers the checkerboard above. They used to sit after slideBgLayer
        // in the file and so always rendered on top of it regardless of
        // background choice, showing as faint dot-rows near the top no
        // matter what color was picked.
        Repeater {
            model: 13 * 8
            delegate: Rectangle {
                required property int index
                x: (index % 13) * 56 + 2
                y: Math.floor(index / 13) * 56 + 14
                width: 2
                height: 2
                radius: 1
                color: "#232530"
            }
        }

        // The actual slide background — driven by slideStore.current.background (see
        // the "Background" row's Change button in the right panel). A
        // dedicated layer, not mCanvas's own color: the checkerboard shows
        // through when the background is deliberately set to Transparent
        // (a real swatch in the picker, not just an unset default).
        Rectangle {
            id: slideBgLayer
            anchors.fill: parent
            color: slideStore.current.background
        }

        // Arrow-key nudge / Delete / Escape for the selected canvas
        // object(s) — guarded so it never fires while a label is actually
        // being typed into (mCanvas only holds focus when nothing is
        // mid-edit; see handleCanvasSelect and every forceActiveFocus call
        // that steals focus back to mCanvas on selection/deselection).
        focus: true
        Keys.onPressed: (event) => {
            if (root.selectedCanvasObjects.length === 0)
                return

            if (event.key === Qt.Key_Escape) {
                root.selectedCanvasObjects = []
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                root.deleteSelectedCanvasObjects()
                event.accepted = true
                return
            }

            const step = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) ? 10 : 1
            let dx = 0, dy = 0
            if (event.key === Qt.Key_Left) dx = -step
            else if (event.key === Qt.Key_Right) dx = step
            else if (event.key === Qt.Key_Up) dy = -step
            else if (event.key === Qt.Key_Down) dy = step
            else return

            root.nudgeSelectedCanvasObjects(dx, dy)
            event.accepted = true
        }

        // Clicking empty canvas clears the whole selection. z: -1 keeps it
        // behind every object's own MouseArea (so clicking an object still
        // selects/drags it) regardless of declaration order — only clicks
        // that land on truly empty canvas space reach this one. Each
        // object's own ring is now enabled only while selected (see
        // DraggableCanvasText), so there's genuinely empty space around an
        // unselected object for this to catch.
        MouseArea {
            id: canvasBackgroundArea
            anchors.fill: parent
            z: -1
            hoverEnabled: true
            // Cursor via the AppCursor override stack (see EditableCanvasLabel's
            // dragArea for the full why — scale-transformed canvas kills plain
            // cursorShape). Without this, truly empty canvas space never
            // pushed anything: AppCursorCatcher's "no active override = leave
            // the OS cursor alone" default then left whatever was last pushed
            // (a resize handle's size cursor, a label's I-beam...) stuck
            // showing over empty canvas indefinitely, since nothing here ever
            // asserted Arrow to fall back to. z: -1 already keeps this behind
            // every object's own MouseArea, so it only ever wins hover (and
            // only ever pushes) over genuinely uncovered space.
            cursorShape: Qt.ArrowCursor
            function syncCursor() {
                // Position truth only: containsMouse latches in this build
                // (hover-exit never delivers — KNOWN_ISSUES.md); the pointer-
                // position stream tracks both directions reliably.
                if (visible && enabled && AppCursor.hovered(canvasBackgroundArea))
                    AppCursor.push(Qt.ArrowCursor, canvasBackgroundArea)
                else
                    AppCursor.pop(canvasBackgroundArea)
            }
            // Recompute from the pointer-position stream (every move) instead
            // of onContainsMouseChanged — same mechanism as PositionHoverArea.
            Connections {
                target: AppCursor
                function onPointerMoved() { canvasBackgroundArea.syncCursor() }
            }
            onEnabledChanged: syncCursor()
            Component.onDestruction: AppCursor.pop(canvasBackgroundArea)
            onClicked: {
                // Steals focus from any object's TextInput mid-edit, so
                // clicking empty canvas both exits edit mode and clears
                // selection instead of leaving the old edit stuck open.
                mCanvas.forceActiveFocus()
                root.selectedCanvasObjects = []
            }
        }

        // Alignment-guide lines shown while dragging/resizing a canvas
        // object (see applyCanvasMove/applyCanvasResize) — z: 1000 (set
        // inside SnapGuideLines.qml) keeps it above every object regardless
        // of where it's declared.
        SnapGuideLines {
            id: canvasSnapGuides
        }

        // Every item on the canvas, created via the "+" Add Content menu
        // (see addCanvasItem) — "text" gets a real editable label, "camera"
        // gets the full live-preview visual, everything else gets a
        // bordered placeholder showing its own icon/label from
        // root.contentTypes (not just its bare kind name) so each kind at
        // least reads as visually distinct until real per-kind content
        // (a real media player frame, audio waveform, etc.) gets built out.
        Repeater {
            id: canvasItemsRepeater
            model: slideStore.current.items
            delegate: DraggableCanvasText {
                id: canvasItemObject
                required property var modelData
                // Exposed so canvasContextMenu's "Edit" case (below) can
                // reach into whichever delegate matches the clicked item and
                // enter edit mode directly, without a fixed/named id to
                // reference the way the old title/verse/ref/date objects had.
                property alias textLabel: itemTextLabel
                x: modelData.x
                y: modelData.y
                width: modelData.width
                height: modelData.height
                style: modelData.style
                selected: root.isCanvasObjectSelected(modelData.key)
                // Sourced from whichever content MouseArea is actually
                // present for this kind — not a second MouseArea of this
                // item's own (see DraggableCanvasText's contentHovered doc).
                contentHovered: modelData.kind === "text" ? itemTextLabel.hovered
                    : modelData.kind === "camera" ? camDragArea.containsMouse
                    : modelData.kind === "clock" ? clockDragArea.containsMouse
                    : modelData.kind === "timer" ? timerDragArea.containsMouse
                    : modelData.kind === "shape" ? shapeDragArea.containsMouse
                    : placeholderDragArea.containsMouse
                onSelectedRequested: (mods) => root.handleCanvasSelect(modelData.key, mods)
                onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(canvasItemObject, mx, my, modelData.key)
                onResizing: (geom) => root.applyCanvasResize(modelData.key, geom)
                onMoving: (dx, dy, snapDisabled) => root.applyCanvasMove(modelData.key, dx, dy, snapDisabled)
                onDragEnded: root.endCanvasDrag()

                EditableCanvasLabel {
                    id: itemTextLabel
                    readonly property var tmeta: canvasItemObject.modelData.meta
                    // Auto-size mode, FreeShow's textFit semantics (see
                    // InspirationOrResources/FreeShow-main/.../autosize.ts):
                    // the BOX is always the master — the user sizes it
                    // freely and the FONT adapts, never the other way
                    // around. "shrinkToFit" renders at the set font size and
                    // only shrinks on overflow; "growToFit" scales the text
                    // to fill the box; "none" is fully fixed. Legacy values
                    // from items saved before the rename map across.
                    readonly property string autoSizeMode: {
                        const v = itemTextLabel.tmeta.autoSize
                        return v === "shrink" ? "shrinkToFit"
                            : v === "grow" ? "growToFit"
                            : v === "shrinkToFit" || v === "growToFit" || v === "none" ? v
                            : "none"
                    }

                    // Shrink-fit measurement: an invisible twin of the
                    // display text rendering at the BASE font size so the
                    // needed scale can be computed without feedback (the
                    // displayed text's own content size depends on its
                    // already-scaled size, so measuring it would oscillate).
                    // Same width as the label so wrapped text measures its
                    // wrapped height; NoWrap text measures its natural
                    // single-line width. Mirrors the display font's
                    // properties explicitly — keep in sync with the
                    // displayText bindings below.
                    Text {
                        id: shrinkMeasure
                        visible: false
                        width: itemTextLabel.width
                        text: itemTextLabel.text
                        font.family: itemTextLabel.tmeta.fontFamily ?? "Inter"
                        font.pixelSize: itemTextLabel.tmeta.fontSize ?? 16
                        font.weight: itemTextLabel.tmeta.bold ? Font.Bold
                            : itemTextLabel.tmeta.fontWeight === "Regular" ? Font.Normal
                            : itemTextLabel.tmeta.fontWeight === "SemiBold" ? Font.DemiBold
                            : itemTextLabel.tmeta.fontWeight === "Bold" ? Font.Bold
                            : Font.Medium
                        font.italic: itemTextLabel.tmeta.italic === true
                        font.letterSpacing: itemTextLabel.tmeta.letterSpacing ?? 0
                        lineHeight: itemTextLabel.tmeta.lineHeight ?? 1.2
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: itemTextLabel.wrapMode
                    }

                    // The font size actually rendered, computed entirely
                    // from the measurement twin — deliberately NOT via Qt's
                    // fontSizeMode: Fit, because Fit treats font.pixelSize
                    // as a CEILING (it scales down from it but never up),
                    // so a Fit-based "grow" could never grow. One formula:
                    // the largest font size whose text fits the box's inner
                    // area, assuming size scales linearly from the measured
                    // base render. Floored at 8px (FreeShow's MIN_FONT_SIZE).
                    readonly property real baseFontSize: itemTextLabel.tmeta.fontSize ?? 16
                    readonly property real fitsBoxSize: {
                        const pad = canvasItemObject.modelData.style ? canvasItemObject.modelData.style.padding : 0
                        const bw = canvasItemObject.modelData.width - pad * 2
                        const bh = canvasItemObject.modelData.height - pad * 2
                        const cw = shrinkMeasure.contentWidth
                        const ch = shrinkMeasure.contentHeight
                        if (bw <= 0 || bh <= 0 || cw <= 0 || ch <= 0)
                            return baseFontSize
                        return Math.max(8, baseFontSize * Math.min(bw / cw, bh / ch))
                    }
                    // shrinkToFit: the box may shrink the text but never
                    // grow it past the set size (FreeShow's exact rule —
                    // "set font size by default, but can shrink if the text
                    // does not fit"). growToFit: the text fills the box in
                    // BOTH directions (PowerPoint-style grow-to-fit), capped
                    // at a sane rendering max. none: the set size, always.
                    readonly property real shrinkFitSize: autoSizeMode === "shrinkToFit"
                        ? Math.min(baseFontSize, fitsBoxSize)
                        : autoSizeMode === "growToFit" ? Math.min(400, fitsBoxSize)
                        : baseFontSize
                    visible: canvasItemObject.modelData.kind === "text"
                    color: itemTextLabel.tmeta.color ?? "#f2f4fa"
                    font.family: itemTextLabel.tmeta.fontFamily ?? "Inter"
                    // shrinkFitSize == baseFontSize in every mode except
                    // shrinkToFit, where it's the overflow-clamped size.
                    font.pixelSize: itemTextLabel.shrinkFitSize
                    // "bold" (the B toggle) is an emphasis override on top
                    // of whatever base weight is picked in the font-family
                    // row's dropdown — matching common rich-text-editor UX
                    // where Bold stays a quick on/off regardless of the
                    // family's own weight.
                    font.weight: itemTextLabel.tmeta.bold ? Font.Bold
                        : itemTextLabel.tmeta.fontWeight === "Regular" ? Font.Normal
                        : itemTextLabel.tmeta.fontWeight === "SemiBold" ? Font.DemiBold
                        : itemTextLabel.tmeta.fontWeight === "Bold" ? Font.Bold
                        : Font.Medium
                    font.italic: itemTextLabel.tmeta.italic === true
                    font.underline: itemTextLabel.tmeta.underline === true
                    font.strikeout: itemTextLabel.tmeta.strikethrough === true
                    font.letterSpacing: itemTextLabel.tmeta.letterSpacing ?? 0
                    lineHeight: itemTextLabel.tmeta.lineHeight ?? 1.2
                    // Always Fixed — every mode's size is computed above;
                    // Qt's Fit is never used (see fitsBoxSize's comment).
                    fontSizeMode: Text.FixedSize
                    horizontalAlignment: {
                        const a = itemTextLabel.tmeta.align ?? "center"
                        return a === "left" ? Text.AlignLeft
                            : a === "right" ? Text.AlignRight
                            : a === "justify" ? Text.AlignJustify
                            : Text.AlignHCenter
                    }
                    text: canvasItemObject.modelData.text
                    // (The box is the master in every auto-size mode — see
                    // autoSizeMode above — so nothing here resizes modelData
                    // geometry from content size anymore; the box-resizing
                    // "grow" that used to live here was FreeShow's inverse
                    // and is gone.)
                    // Live per-keystroke propagation into the item object
                    // — the thumbnail binds to this same object, so rows
                    // update as you type, not only on commit. beginTextEdit
                    // pushes the undo snapshot on the session's first
                    // keystroke (see its header comment for why it can't
                    // push before the fact); endTextEdit closes the session
                    // so the next edit session gets its own undo step.
                    onEdited: (value) => {
                        root.beginTextEdit(canvasItemObject.modelData.key)
                        canvasItemObject.modelData.text = value
                    }
                    onCommitted: (value) => {
                        canvasItemObject.modelData.text = value
                        root.endTextEdit()
                    }
                    // Closes the typing-undo session on every exit path —
                    // commit (onCommitted above) but also Escape, which
                    // exits edit mode without committing. Without this, a
                    // second double-click-and-type into the same item would
                    // silently merge into the first session's undo step.
                    //
                    // Also tracks root.anyTextEditing — separate from
                    // textUndoKey, which only gets set on the session's
                    // FIRST KEYSTROKE (see beginTextEdit's own comment) and
                    // so stays empty the whole time if you enter edit mode
                    // and press Ctrl+Z before typing anything. This flips
                    // true the instant edit mode actually starts (the
                    // double-click), which is what the undo/redo Shortcuts
                    // below actually need to stay enabled through.
                    onEditingChanged: {
                        root.anyTextEditing = itemTextLabel.editing
                        if (!itemTextLabel.editing)
                            root.endTextEdit()
                    }
                    onSelectRequested: (mods) => root.handleCanvasSelect(canvasItemObject.modelData.key, mods)
                    onMoveRequested: (dx, dy, snapDisabled) => {
                        if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                            root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                        root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                    }
                    onDragEnded: root.endCanvasDrag()
                    // Undo/redo chords pressed while this item's edit is
                    // open (see EditableCanvasLabel.undoRedoRequested for
                    // why they arrive here instead of via the Shortcuts
                    // below: the focused TextEdit accepts their
                    // ShortcutOverride, so the Shortcuts never activate).
                    // Deferred with Qt.callLater for the same reason
                    // UndoHistory.commit is deferred: undo() applies a
                    // snapshot that DESTROYS AND RECREATES this very
                    // delegate — running it synchronously here would mutate
                    // the model mid-key-dispatch from inside the dying
                    // TextEdit (the documented AudioEffectsPanel hazard).
                    // By the callLater tick the event is done, undo() has
                    // abandoned the open session's pending snapshot and
                    // pulled focus back to mCanvas — a clean exit path.
                    onUndoRedoRequested: (undo) => Qt.callLater(undo ? root.undo
                                                                     : root.redo)
                }

                // The full live-preview visual (lifted from the former fixed
                // camObject) — same Shape gradient, LIVE badge, cam icons,
                // CAM 1 label, timestamp — for every "camera" kind item, not
                // just a single hardcoded one.
                Item {
                    id: camContent
                    visible: canvasItemObject.modelData.kind === "camera"
                    anchors.fill: parent

                    Shape {
                        anchors.fill: parent
                        ShapePath {
                            fillGradient: LinearGradient {
                                x1: camContent.width * 0.5; x2: camContent.width * 0.5
                                y1: 0; y2: camContent.height
                                GradientStop { color: "#ff123326"; position: 0 }
                                GradientStop { color: "#ff07130e"; position: 1 }
                            }
                            strokeColor: "#000"
                            strokeWidth: 0
                            PathRectangle { width: camContent.width; height: camContent.height; radius: 8 }
                        }
                    }
                    Image { x: 8; y: 8; source: Qt.resolvedUrl("assets/cam_dot.png") }
                    Text {
                        x: 18; y: 7
                        color: "#e2e8f0"
                        font.family: "Inter"
                        font.pixelSize: 7
                        font.weight: Font.Medium
                        text: qsTr("LIVE")
                    }
                    Rectangle { x: 10; y: 24; height: 34; width: 36; color: "#14503a"; radius: 4 }
                    Rectangle { x: 30; y: 24; height: 34; width: 24; color: "#0f3a2c"; radius: 4 }
                    Image { x: 96; y: 10; source: Qt.resolvedUrl("assets/cam_lens.png") }
                    Text {
                        x: 10; y: 68
                        color: "#eef0f6"
                        font.family: "Inter"
                        font.pixelSize: 8
                        font.weight: Font.Medium
                        // The source picked in cameraSourceModal (see
                        // addCanvasItem's onApplied handler below), falling
                        // back to a generic label for items created before
                        // that field was set.
                        text: canvasItemObject.modelData.text.length > 0 ? canvasItemObject.modelData.text : qsTr("Camera")
                    }
                    Text {
                        x: 68; y: 68
                        color: "#5c6475"
                        font.family: "Inter"
                        font.pixelSize: 8
                        text: qsTr("10:24:07")
                    }

                    // Click to select, drag the body to move — shared
                    // press/threshold/delta body (see CanvasDragArea), the
                    // same mechanics EditableCanvasLabel uses internally.
                    CanvasDragArea {
                        id: camDragArea
                        anchors.fill: parent
                        onMoved: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragFinished: root.endCanvasDrag()
                        onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers)
                    }
                }

                // Live-ticking visual for every "clock" kind item, driven by
                // the config picked in clockSourceModal (see its onApplied
                // handler below) and LiveClock.qml's shared ticking/
                // formatting logic — not a static placeholder.
                Item {
                    id: clockContent
                    readonly property var cfg: canvasItemObject.modelData.meta
                    readonly property bool hour12: clockContent.cfg.format !== "24"
                    readonly property bool showSeconds: clockContent.cfg.showSeconds !== false
                    readonly property bool showDate: clockContent.cfg.showDate === true
                    readonly property bool analog: clockContent.cfg.style === "analog"
                    visible: canvasItemObject.modelData.kind === "clock"
                    anchors.fill: parent

                    LiveClock {
                        id: clockTicker
                        running: clockContent.visible
                    }

                    // Background/border/corner-radius come straight from
                    // the item's own style now (see primarySelectedSupportsFill
                    // above) — same neutral fallbacks the shape/camera
                    // visuals use when no color's been picked yet.
                    Rectangle {
                        anchors.fill: parent
                        radius: canvasItemObject.style ? canvasItemObject.style.cornerRadius : 8
                        color: (canvasItemObject.style && canvasItemObject.style.backgroundColor.a > 0)
                            ? canvasItemObject.style.backgroundColor : "#12131a"
                        border.color: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                            ? canvasItemObject.style.borderColor : "#3a4155"
                        border.width: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                            ? canvasItemObject.style.borderWidth : 1
                    }

                    Column {
                        visible: !clockContent.analog
                        anchors.centerIn: parent
                        spacing: 2

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#9b8ff5"
                            font.family: "Inter"
                            font.weight: Font.DemiBold
                            font.pixelSize: Math.max(10, Math.min(clockContent.width, clockContent.height) * 0.22)
                            text: clockTicker.formatClock(clockTicker.now, clockContent.hour12, clockContent.showSeconds)
                        }
                        Text {
                            visible: clockContent.showDate
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#5c6475"
                            font.family: "Inter"
                            font.pixelSize: 9
                            text: Qt.formatDate(clockTicker.now, "dddd, MMMM d")
                        }
                    }

                    // Simple analog face — a circle, three rotated hands, a
                    // center pin. No real clock-face artwork; same
                    // placeholder-visual status as the rest of this app's
                    // per-kind content until that gets built out.
                    Item {
                        visible: clockContent.analog
                        anchors.centerIn: parent
                        width: Math.min(clockContent.width, clockContent.height) * 0.8
                        height: width

                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: "#12131a"
                            border.color: "#3a4155"
                            border.width: 2
                        }
                        Rectangle {
                            width: 4
                            height: parent.height * 0.24
                            radius: 2
                            color: "#eef1f8"
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.verticalCenter
                            transformOrigin: Item.Bottom
                            rotation: (clockTicker.now.getHours() % 12 + clockTicker.now.getMinutes() / 60) * 30
                        }
                        Rectangle {
                            width: 3
                            height: parent.height * 0.34
                            radius: 1.5
                            color: "#c8cdd9"
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.verticalCenter
                            transformOrigin: Item.Bottom
                            rotation: (clockTicker.now.getMinutes() + clockTicker.now.getSeconds() / 60) * 6
                        }
                        Rectangle {
                            visible: clockContent.showSeconds
                            width: 1.5
                            height: parent.height * 0.4
                            color: "#6c5ce7"
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.verticalCenter
                            transformOrigin: Item.Bottom
                            rotation: clockTicker.now.getSeconds() * 6
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            color: "#6c5ce7"
                        }
                    }

                    CanvasDragArea {
                        id: clockDragArea
                        anchors.fill: parent
                        onMoved: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragFinished: root.endCanvasDrag()
                        onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers)
                    }
                }

                // Live-ticking visual for every "timer" kind item, driven by
                // the config picked in timerSourceModal (see its onApplied
                // handler below) and LiveClock.qml's shared countdown/
                // count-up/time-of-day math — not a static placeholder.
                Item {
                    id: timerContent
                    readonly property var cfg: canvasItemObject.modelData.meta
                    readonly property string mode: timerContent.cfg.mode ?? "countdown"
                    readonly property real durationSeconds: timerContent.cfg.durationSeconds ?? 300
                    readonly property real startedAt: timerContent.cfg.startedAt ?? Date.now()
                    visible: canvasItemObject.modelData.kind === "timer"
                    anchors.fill: parent

                    LiveClock {
                        id: timerTicker
                        running: timerContent.visible
                    }

                    // Background/border/corner-radius come straight from
                    // the item's own style now (see primarySelectedSupportsFill
                    // above) — same neutral fallbacks the shape/camera
                    // visuals use when no color's been picked yet.
                    Rectangle {
                        anchors.fill: parent
                        radius: canvasItemObject.style ? canvasItemObject.style.cornerRadius : 8
                        color: (canvasItemObject.style && canvasItemObject.style.backgroundColor.a > 0)
                            ? canvasItemObject.style.backgroundColor : "#12131a"
                        border.color: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                            ? canvasItemObject.style.borderColor : "#3a4155"
                        border.width: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                            ? canvasItemObject.style.borderWidth : 1
                    }

                    Text {
                        anchors.centerIn: parent
                        color: "#9b8ff5"
                        font.family: "Inter"
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.max(10, Math.min(timerContent.width, timerContent.height) * 0.28)
                        text: timerTicker.formatDuration(
                            timerTicker.timerSeconds(timerTicker.now, timerContent.mode, timerContent.durationSeconds, timerContent.startedAt))
                    }

                    CanvasDragArea {
                        id: timerDragArea
                        anchors.fill: parent
                        onMoved: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragFinished: root.endCanvasDrag()
                        onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers)
                    }
                }

                // Visual for every "shape" kind item, driven by the type
                // picked in shapeSourceModal (see its onApplied handler
                // below) plus the item's OWN CanvasItemStyle — unlike
                // camera/media/timer/clock, a shape's fill/border/corner-
                // radius genuinely ARE the item's whole visual, so this
                // reads style.backgroundColor/borderColor/borderWidth/
                // cornerRadius directly instead of using fixed colors (see
                // primarySelectedSupportsFill above, which keeps Background/
                // Border active for this kind specifically).
                Item {
                    id: shapeContent
                    readonly property var cfg: canvasItemObject.modelData.meta
                    readonly property string shapeType: shapeContent.cfg.shapeType ?? "rectangle"
                    readonly property var st: canvasItemObject.style
                    // Alpha-based transparent test, NOT `!== "transparent"`:
                    // a QML color holding "transparent" reads back as
                    // #00000000, so a string comparison is ALWAYS true and
                    // the fallback never fired — every shape rendered with a
                    // fully transparent fill (the "shape doesn't show" bug).
                    // Verified with a standalone qml runtime probe.
                    readonly property bool hasFill: shapeContent.st && shapeContent.st.backgroundColor.a > 0
                    readonly property color fillColor: shapeContent.hasFill
                        ? shapeContent.st.backgroundColor : "#3a3f55"
                    readonly property color strokeColor: shapeContent.st ? shapeContent.st.borderColor : "#6c5ce7"
                    readonly property real strokeWidth: (shapeContent.st && shapeContent.st.borderEnabled) ? shapeContent.st.borderWidth : 0
                    readonly property bool isCircle: shapeContent.shapeType === "circle"
                    readonly property bool isLine: shapeContent.shapeType === "line"
                    readonly property bool isGlyph: ["triangle", "arrow", "star", "hexagon"].includes(shapeContent.shapeType)
                    // The fallback for "rectangle"/"rounded" AND anything
                    // unrecognized — always shows a filled box rather than
                    // nothing if shapeType ever comes out unexpected.
                    readonly property bool isBox: !shapeContent.isCircle && !shapeContent.isLine && !shapeContent.isGlyph
                    visible: canvasItemObject.modelData.kind === "shape"
                    anchors.fill: parent

                    // rectangle / rounded (and fallback) — a plain box;
                    // corner radius is already its own independent style
                    // control.
                    Rectangle {
                        visible: shapeContent.isBox
                        anchors.fill: parent
                        radius: shapeContent.st ? shapeContent.st.cornerRadius : 0
                        color: shapeContent.fillColor
                        border.color: shapeContent.strokeColor
                        border.width: shapeContent.strokeWidth
                    }

                    // circle — square-fit ellipse (a true ellipse for
                    // non-square boxes is future work, same simplification
                    // status as the rest of this app's per-kind visuals).
                    Rectangle {
                        visible: shapeContent.isCircle
                        anchors.centerIn: parent
                        width: Math.min(parent.width, parent.height)
                        height: width
                        radius: width / 2
                        color: shapeContent.fillColor
                        border.color: shapeContent.strokeColor
                        border.width: shapeContent.strokeWidth
                    }

                    // line — thin horizontal bar at vertical center.
                    Rectangle {
                        visible: shapeContent.isLine
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: Math.max(2, shapeContent.strokeWidth || 3)
                        color: shapeContent.fillColor
                    }

                    // triangle/arrow/star/hexagon — a simplified glyph fill,
                    // same language shapeSourceModal's own picker cells use
                    // for these (plain Text icons, not custom path art);
                    // real vector shapes are future work.
                    Text {
                        visible: shapeContent.isGlyph
                        anchors.centerIn: parent
                        color: shapeContent.fillColor
                        font.pixelSize: Math.min(parent.width, parent.height) * 0.7
                        text: shapeContent.shapeType === "triangle" ? "▲"
                            : shapeContent.shapeType === "arrow" ? "→"
                            : shapeContent.shapeType === "star" ? "★"
                            : "⬡"
                    }

                    CanvasDragArea {
                        id: shapeDragArea
                        anchors.fill: parent
                        onMoved: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragFinished: root.endCanvasDrag()
                        onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers)
                    }
                }

                // Generic placeholder for every other kind — its own
                // icon/label from root.contentTypes, not just the bare kind
                // name, so Media/Audio/Shape at least read as visually
                // distinct from each other while real per-kind content is
                // still future work.
                Rectangle {
                    id: genericPlaceholder
                    readonly property var typeInfo: {
                        for (let i = 0; i < root.contentTypes.length; ++i) {
                            if (root.contentTypes[i].kind === canvasItemObject.modelData.kind)
                                return root.contentTypes[i]
                        }
                        return { icon: "?", label: canvasItemObject.modelData.kind }
                    }
                    visible: !["text", "camera", "clock", "timer", "shape"].includes(canvasItemObject.modelData.kind)
                    anchors.fill: parent
                    color: "#1a1c26"
                    border.color: "#3a4155"
                    border.width: 1
                    radius: 6

                    Column {
                        anchors.centerIn: parent
                        spacing: 4

                    // IconGlyph, not a unicode Text glyph — same missing-
                    // glyph problem the Add Content chips had (see
                    // contentTypes' comment).
                    Item {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 16
                        width: 20
                        height: 20

                        Text {
                            anchors.fill: parent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            visible: genericPlaceholder.typeInfo.icon === "text"
                            color: "#9b8ff5"
                            font.pixelSize: 16
                            text: "Aa"
                        }
                        IconGlyph {
                            anchors.centerIn: parent
                            visible: genericPlaceholder.typeInfo.icon !== "text"
                            name: genericPlaceholder.typeInfo.icon
                            color: "#9b8ff5"
                            width: 16
                            height: 16
                        }
                    }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#aeb6c8"
                            font.family: "Inter"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            text: genericPlaceholder.typeInfo.label.toUpperCase()
                        }
                        // The source picked in mediaSourceModal (see
                        // addCanvasItem's onApplied handler below) — blank
                        // for kinds without a picker yet (audio/shape/
                        // timer/clock), so nothing extra shows for those.
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: canvasItemObject.modelData.text.length > 0
                            color: "#5c6475"
                            font.family: "Inter"
                            font.pixelSize: 9
                            text: canvasItemObject.modelData.text
                        }
                    }

                    // Click to select, drag the body to move — shared
                    // press/threshold/delta body (see CanvasDragArea), the
                    // same mechanics EditableCanvasLabel uses internally.
                    CanvasDragArea {
                        id: placeholderDragArea
                        anchors.fill: parent
                        onMoved: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragFinished: root.endCanvasDrag()
                        onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers)
                    }
                }
            }
        }

        // The actual edit-area border — declared last so it draws on top of
        // every other child (checkerboard, background, text objects, camera
        // panel), instead of underneath them the way a border set on
        // mCanvas's own Rectangle would (see the comment up top).
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: "#ffffff"
            border.width: 2
        }
    }

    // Empty-state veil over the canvas — shown only when no slide is active.
    // A root-level sibling (not a child of mCanvas) deliberately: mCanvas is
    // disabled in this state, and a disabled parent disables its children's
    // input, so the "Add slide" chip here must not be inside it. Covers the
    // canvas area only; the slide list and its Add slide button stay live.
    Rectangle {
        x: mCanvas.x
        y: mCanvas.y
        width: mCanvas.width
        height: mCanvas.height
        visible: !root.hasActiveSlide
        color: "#b012131a"

        Column {
            anchors.centerIn: parent
            spacing: 12

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#9aa0b5"
                font.family: "Inter"
                font.pixelSize: 13
                text: qsTr("No slides yet")
            }
            Text {
                width: 300
                anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter
                color: "#5c6475"
                font.family: "Inter"
                font.pixelSize: 10
                wrapMode: Text.Wrap
                text: qsTr("Add a slide to start building — then place text, cameras and more on the canvas")
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                height: 36
                width: 140
                radius: 18
                color: emptyAddArea.containsMouse ? "#7a6cf0" : "#6c5ce7"
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    color: "#ffffff"
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    text: qsTr("+ Add slide")
                }

                MouseArea {
                    id: emptyAddArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: showSession.addSlide()
                }
            }
        }
    }

    Rectangle {
        x: root.width - 558
        y: root.height - 59
        height: 32
        width: 124
        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8

        Row {
            anchors.fill: parent
            Text {
                width: 41; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomOutArea.containsMouse ? "#c8cdd9" : "#8a94a6"
                font.pixelSize: 12
                text: "−"
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: zoomOutArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = Math.max(0.5, Math.round((root.canvasZoom - 0.1) * 10) / 10)
                }
            }
            Text {
                width: 42; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomLabelArea.containsMouse ? "#c8cdd9" : "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 10
                text: Math.round(root.canvasZoom * 100) + "%"
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: zoomLabelArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = 1
                }
            }
            Text {
                width: 41; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomInArea.containsMouse ? "#c8cdd9" : "#8a94a6"
                font.pixelSize: 12
                text: "+"
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: zoomInArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = Math.min(2, Math.round((root.canvasZoom + 0.1) * 10) / 10)
                }
            }
        }
    }

    Rectangle {
        id: addContentChip
        x: 286
        y: root.height - 62
        height: 46
        width: 46
        radius: 23
        color: "#6c5ce7"
        // Nothing to add content TO without an active slide — dim and inert.
        enabled: root.hasActiveSlide
        opacity: root.hasActiveSlide ? 1 : 0.35

        Text {
            anchors.centerIn: parent
            color: "#ffffff"
            font.family: "Inter"
            font.pixelSize: 22
            // A "+" rotated 45° reads as an "X" — the ground truth exports
            // this literally as a pre-rotated glyph for the open state
            // rather than swapping characters.
            rotation: root.addMenuOpen ? 45 : 0
            Behavior on rotation { NumberAnimation { duration: 120 } }
            text: "＋"
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.addMenuOpen = !root.addMenuOpen
        }
    }

    Rectangle {
        id: addContentMenu
        x: 344
        y: root.height - 63
        height: 46
        width: 388
        border.color: "#2a2f3a"
        border.width: 1
        color: "#151824"
        radius: 14
        visible: root.addMenuOpen

        // Centered, not hand-pinned x:10: 7 chips × 48 + 6 gaps × 4 = 360
        // wide inside a 388-wide menu — a 10px left inset left 18px hanging
        // on the right (the whole content block read shifted left). centerIn
        // keeps the row optically centered whatever the roster does next.
        Row {
            id: contentTypeRow
            anchors.centerIn: parent
            spacing: 4

            Repeater {
                model: root.contentTypes
                delegate: Rectangle {
                    id: typeChip
                    required property var modelData
                    required property int index
                    readonly property bool active: chipHover.hoveredIndex === index

                    height: 34
                    width: 48
                    radius: 8
                    border.width: typeChip.active ? 1 : 0
                    border.color: "#406c5ce7"
                    color: typeChip.active ? "#206c5ce7" : "#1b1e2a"
                    // NO Behavior animations here: at slow hand speed the
                    // 100ms fades on adjacent chips fired for every boundary
                    // crossing and read as shimmer/flicker ("slow down your
                    // mouse" was the exact reproduction). Instant switching
                    // makes the highlight a hard swap — nothing to see at
                    // any speed.

                    // Two fixed vertical bands inside the 34px pill — icon
                    // band y 3..17 (14px), label band y 19..31 (12px) — 3px
                    // off the top and bottom edges. The old offsets (icon
                    // y:4, label y:22 with NO height) let the label's natural
                    // ~11px line box run to the pill's bottom edge and the
                    // whole block read 1px low/left-of-center. Explicit band
                    // heights + verticalAlignment pin each glyph INSIDE its
                    // band so neither the 12px "Aa" line box nor the 8px
                    // label line box can drift with font metrics.
                    Text {
                        y: 3
                        height: 14
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: typeChip.active ? "#9b8ff5" : "#9aa0b5"
                        font.family: "Inter"
                        font.pixelSize: 12
                        // "text" keeps its Aa letters; every other kind
                        // renders its real IconGlyph (see contentTypes above).
                        text: typeChip.modelData.icon === "text" ? "Aa"
                            : typeChip.modelData.icon
                        visible: typeChip.modelData.icon === "text"
                    }
                    IconGlyph {
                        y: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        name: typeChip.modelData.icon
                        color: typeChip.active ? "#9b8ff5" : "#9aa0b5"
                        visible: typeChip.modelData.icon !== "text"
                        // Explicit box + origin, not implicit: with only
                        // implicitWidth/Height, the root-level scale below
                        // has measured its center from a 0x0 box on some
                        // builds — play/music (1.6×) then drew up-left of
                        // the pill entirely (the "icons are out" report)
                        // while the 1× icons hid the defect. Same lesson as
                        // AppHeader.qml's gear: explicit width/height first,
                        // THEN centerIn/scale compute from real geometry.
                        width: 14
                        height: 14
                        transformOrigin: Item.Center
                        // No per-icon scale: all seven kinds now draw
                        // from the same Lucide 24-grid family (play/music
                        // re-authored natively — the old Figma-export
                        // glyphs needed boosts that fattened their strokes
                        // and made them read oversized).
                    }
                    Text {
                        y: 19
                        height: 12
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: typeChip.active ? "#eef1f8" : "#6b7080"
                        font.family: "Inter"
                        font.pixelSize: 8
                        text: typeChip.modelData.label
                    }
                }
            }
        }

        // A sibling of the Row, not a child of it — Row (like Column/Grid)
        // doesn't allow its own children to use anchors.fill/left/right/etc,
        // since it positions them itself; anchors.fill: contentTypeRow from
        // outside the Row works fine.
        // Covers the WHOLE menu, not just the row: sized to the row exactly,
        // the menu's 6px padding bands above/below are hover dead zones —
        // pointer truth crossing the row's top/bottom edge read outside →
        // hoveredIndex −1 → the highlight blinked OFF for the crossing — the
        // "first hover flickers" report. zoneRow tells the area to map x
        // into the row's frame, so any point over the menu is a live zone
        // (columnar: one x → one chip) and the highlight only resets when
        // the pointer truly leaves the menu. onClicked re-checks the press
        // actually landed in the row's band, so padding clicks are no-ops
        // rather than acting on whatever chip that column happens to hold.
        ZoneHoverArea {
            id: chipHover
            anchors.fill: parent
            container: contentTypeRow
            zoneRow: contentTypeRow
            cursorShape: Qt.PointingHandCursor
            // Formal parameter, not injected `mouse` — parameter injection
            // into signal handlers is deprecated and logs a warning toast
            // on every open (surfaced by the self-test grab).
            onClicked: (mouse) => {
                // The row band inside this menu: y 6..40 (menu 46 tall,
                // row 34 tall, centered). Outside it the press is on
                // padding — never act on it.
                if (mouse.y < 6 || mouse.y > 40)
                    return
                if (hoveredIndex < 0)
                    return
                const kind = root.contentTypes[hoveredIndex].kind
                root.addMenuOpen = false
                // Camera/Media/Timer/Clock/Shape each need configuration
                // picked first (see cameraSourceModal/mediaSourceModal/
                // timerSourceModal/clockSourceModal/shapeSourceModal below)
                // rather than landing on the canvas immediately — Audio is
                // the only kind left with no ground-truth picker popup, so
                // it's the only one that still adds straight away.
                if (kind === "camera")
                    root.cameraModalOpen = true
                else if (kind === "media")
                    root.mediaModalOpen = true
                else if (kind === "timer")
                    root.timerModalOpen = true
                else if (kind === "clock")
                    root.clockModalOpen = true
                else if (kind === "shape")
                    root.shapeModalOpen = true
                else
                    root.addCanvasItem(kind)
            }
        }
    }

    // ---- Left panel: slide list ----
    Rectangle {
        id: leftPanel
        y: 48
        height: root.height - 48
        width: 280
        border.color: "#232530"
        border.width: 1
        color: "#12131a"

        // Plain background cursor reset — declared FIRST (so every real
        // button below sits above it in z-order and still wins its own
        // hover/cursor). This panel sits outside mCanvas's scale transform,
        // so a direct cursorShape works here without going through
        // AppCursor at all (that mechanism exists specifically to work
        // around cursorShape not applying under a scale transform — see
        // AppCursorCatcher.qml). Without this, moving the pointer here
        // right after something elsewhere pushed a non-default OS cursor
        // (a resize handle's size cursor, a text item's I-beam...) left it
        // stuck: nothing in this panel's plain background space ever
        // asserted a cursor of its own to replace it, same gap
        // EditScreen's canvasBackgroundArea exists to close for the canvas
        // side.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.ArrowCursor
        }

        Rectangle {
            x: 12
            y: 6
            height: 64
            width: 256
            border.color: "#252836"
            border.width: 1
            color: "#171924"
            radius: 10

            Rectangle {
                x: 8
                y: 8
                height: 34
                width: 34
                border.color: "#3a4a7a"
                border.width: 1
                color: "#1a2240"
                radius: 8

                Text {
                    anchors.centerIn: parent
                    color: "#7b9eff"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                    text: qsTr("VGR")
                }
            }
            Text {
                x: 50
                y: 13
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                text: qsTr("Sunday Service")
            }
            Text {
                x: 50
                y: 32
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 9
                text: qsTr("%1 slide(s)").arg(slideListRepeater.count)
            }
            Text {
                x: 236
                y: 24
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 11
                text: "⌄"
            }
        }

        Text {
            x: 12
            y: 80
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 10
            text: qsTr("SLIDES · %1").arg(slideListRepeater.count)
        }

        Flickable {
            id: slideListFlick
            x: 12
            y: 98
            width: 256
            height: leftPanel.height - 98 - 12
            clip: true
            contentWidth: width
            contentHeight: slideListColumn.height
            boundsBehavior: Flickable.StopAtBounds

        Column {
            id: slideListColumn
            width: parent.width
            spacing: 8

            Repeater {
                id: slideListRepeater
                model: slideModel
                // No inline required-property declarations here —
                // SlideListItem.qml declares them and the model injects them
                // there. Duplicating them inline broke injection: the
                // delegate failed to create ("Required property ... was not
                // initialized"), which is exactly why thumbnails never
                // appeared even though slides were created.
                delegate: SlideListItem {
                    // Live thumbnail: the slide's items straight from the
                    // per-slide store — the same objects the canvas edits,
                    // so a row re-renders the moment its content does.
                    previewItems: slideStore.items(slideId)
                    contentTypes: root.contentTypes
                    // The design space the items' coordinates live in — the
                    // canvas's actual on-screen size, so the mini-canvas
                    // scale can never drift from what you see while editing.
                    canvasWidth: mCanvas.width
                    canvasHeight: mCanvas.height
                    onSelected: slideModel.selectSlide(index)
                    onDuplicateRequested: showSession.duplicateSlide(index)
                    onDeleteRequested: showSession.removeSlide(index)
                    onContextMenuRequested: (mx, my) => {
                        // Shared map+clamp helper — openAt maps into root's
                        // space and clamps to the window, so the menu escapes
                        // slideListFlick's clip without landing off-screen.
                        slideContextMenu.openAt(this, mx, my, root)
                        root.contextMenuSlideIndex = index
                    }
                }
            }

            Rectangle {
                id: addSlideButton
                height: 36
                width: 256
                border.color: "#2a3140"
                border.width: 1
                color: addSlideArea.containsMouse ? "#20242f" : "#1a1c26"
                radius: 8
                Behavior on color { ColorAnimation { duration: 100 } }

                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    Text { color: "#6c5ce7"; font.family: "Inter"; font.pixelSize: 12; font.weight: Font.Medium; text: "+" }
                    Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("Add slide") }
                }

                MouseArea {
                    id: addSlideArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: showSession.addSlide()
                }
            }
        }
        }

        AppScrollBar {
            x: 272
            y: slideListFlick.y
            height: slideListFlick.height
            flickable: slideListFlick
        }
    }

    // ---- Right panel ----
    Rectangle {
        id: rightPanel
        x: root.width - 400
        y: 48
        height: root.height - 48
        width: 400
        color: "#0f1015"
        // Greyed with the canvas in the empty state — Background/Size & Style
        // target the active slide, which doesn't exist yet.
        enabled: root.hasActiveSlide
        opacity: root.hasActiveSlide ? 1 : 0.35

        // Plain background cursor reset — see leftPanel's own copy of this
        // MouseArea for the full reasoning (this is the exact gap behind
        // the "cursor stays as I-beam after leaving a text edit to pick a
        // background/border color" report: this panel hosts that Change
        // button, and had no cursor handling of its own at all).
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.ArrowCursor
        }

        // Interactive now — the ground truth export (VGRPresenter_Main_
        // Screen_Edit_Add_Camera.qml's r_tab*) has this same three-tab bar
        // but as a single flat frame with ITEMS always active and no
        // captured design for what TEXT/SLIDE should show, so this only
        // switches the indicator for now; the content below doesn't vary
        // by tab yet (see KNOWN_ISSUES.md if that scope grows later).
        Row {
            id: rightTabRow
            // Spreads the tabs evenly across the full panel width — each
            // label owns exactly one third of the panel and sits centered
            // in its cell (the classic tab-strip distribution). Centering
            // the Row as a fixed-width cluster (the previous fix) left the
            // three labels bunched together in the middle; equal cells read
            // more naturally at this panel width.
            x: 0
            width: parent.width
            y: 19

            Text {
                id: tabItemsLabel
                width: parent.width / 3
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "items" ? "#ff4d3d" : "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                // Constant weight in every state — a weight that changes
                // with the active tab changes the label's advance width,
                // which re-flows this Row and shoves the neighbouring tabs
                // sideways on every click. Color alone carries the active
                // state; the underline indicator does the rest.
                font.weight: Font.Medium
                text: qsTr("ITEMS")
                Behavior on color { ColorAnimation { duration: 100 } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.rightPanelTab = "items"
                }
            }
            Text {
                id: tabTextLabel
                width: parent.width / 3
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "text" ? "#ff4d3d" : "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Medium // constant — see tabItemsLabel above
                text: qsTr("TEXT")
                Behavior on color { ColorAnimation { duration: 100 } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.rightPanelTab = "text"
                }
            }
            Text {
                id: tabSlideLabel
                width: parent.width / 3
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "slide" ? "#ff4d3d" : "#8a94a6"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Medium // constant — see tabItemsLabel above
                text: qsTr("SLIDE")
                Behavior on color { ColorAnimation { duration: 100 } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.rightPanelTab = "slide"
                }
            }
        }
        Rectangle {
            y: 40
            height: 3
            radius: 1.50
            color: "#ff4d3d"
            // Centered under the active label: each label is one eqzual cell
            // of the tab strip, so the underline is the label's text width
            // placed at the cell's horizontal center — it glides between
            // cell centers when the tab changes and tracks automatically if
            // the panel is ever resized.
            property string _activeLabel: root.rightPanelTab === "items" ? "items"
                : root.rightPanelTab === "text" ? "text" : "slide"
            x: rightTabRow.x
                + (root.rightPanelTab === "items" ? tabItemsLabel.x
                : root.rightPanelTab === "text" ? tabTextLabel.x : tabSlideLabel.x)
                + (root.rightPanelTab === "items" ? tabItemsLabel.width
                : root.rightPanelTab === "text" ? tabTextLabel.width : tabSlideLabel.width) / 2
                - width / 2
            width: root.rightPanelTab === "items" ? tabItemsLabel.implicitWidth
                : root.rightPanelTab === "text" ? tabTextLabel.implicitWidth : tabSlideLabel.implicitWidth
            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
            Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
        }
        Rectangle { y: 43; height: 1; width: 400; color: "#232530" }

        // ITEMS tab content — Outputs grid + the selected item's Background/
        // Size & Style. Not literally "canvas items" despite the tab's
        // name (see rightPanelTab's header comment — this predates the tab
        // bar becoming interactive and hasn't been reorganized to match its
        // label); TEXT below is the first tab with real, distinct content.
        Item {
            id: itemsTabContent
            anchors.fill: parent
            visible: root.rightPanelTab === "items"

        // The SAME wall the Show screen's right column renders — the shared
        // MonitorWall component (hero page for a lone output, 2×2 paging,
        // snap swipe, dots), not a private Grid hosting the tiles directly:
        // "the same monitors" had come to mean same tiles, different hosting,
        // which rendered visibly differently per surface (this tab stayed a
        // flat 182px two-column grid while Show grew hero/paging). y=62
        // keeps the tab's own top spacing; width matches the Show wall's
        // 376 so tiles are pixel-identical across surfaces.
        MonitorWall {
            x: 12
            y: 62
            width: 376
        }

        Text {
            x: 16
            y: 390
            color: "#8a8fa3"
            font.family: "Inter"
            font.pixelSize: 10
            font.weight: Font.Bold
            text: qsTr("SLIDE")
        }
        Rectangle {
            x: 8
            y: 406
            height: 46
            width: 384
            color: "#161823"
            radius: 8
            // Greyed out (not hidden — the row's meaning would otherwise
            // silently flip back to "slide background" while something's
            // still selected, which reads as more confusing than a dimmed,
            // inert control) when the selected item's own visual (camera's
            // live preview, the generic placeholder) fully covers whatever
            // a background color would draw underneath it.
            enabled: root.primarySelectedSupportsFill
            opacity: root.primarySelectedSupportsFill ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                // Same row, two targets: the selected item's background when
                // something's selected, the slide's own background
                // otherwise — one control, not a duplicate built into
                // Size & Style too (see that file's header comment).
                text: root.primarySelectedItemStyle ? qsTr("Item Background") : qsTr("Background")
            }
            Rectangle {
                x: 256
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                border.color: "#3a4a7a"
                border.width: 1
                radius: 5
                // A flat swatch — the slide background is a solid color now
                // (gradients left with the opacity feature), so no swatch
                // gradient plumbing.
                color: root.primarySelectedItemStyle ? root.primarySelectedItemStyle.backgroundColor : slideStore.current.background
            }
            Rectangle {
                x: 298
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                border.color: "#2a3140"
                border.width: 1
                color: changeArea.containsMouse ? "#20242f" : "#1a1c26"
                radius: 12
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    color: "#aeb6c8"
                    font.family: "Inter"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    text: qsTr("Change")
                }

                MouseArea {
                    id: changeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.bgModalTarget = root.primarySelectedItemStyle ? "itemBackground" : "background"
                        root.bgModalOpen = true
                    }
                }
            }
            Text {
                x: 368
                anchors.verticalCenter: parent.verticalCenter
                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 14
                text: "›"
            }
        }

        SizeStyleCard {
            id: sizeStyleCard
            x: 8
            y: 462
            width: 384
            // Every selected object's style, in selection order — the card
            // displays the first one and applies changes to all of them.
            // canvasObjectByKey(k).style resolves correctly whether k is one
            // of the fixed objects (registry-backed) or a "+"-added item
            // (its own inline style) — one lookup path for both.
            targets: root.selectedCanvasObjects.map((k) => root.canvasObjectByKey(k)?.style).filter((s) => s !== null && s !== undefined)
            fillSupported: root.primarySelectedSupportsFill
            undoHook: root.pushUndoSnapshot
            focusHook: function () { mCanvas.forceActiveFocus() }
            onChangeBorderRequested: {
                root.bgModalTarget = "border"
                root.bgModalOpen = true
            }
        }
        } // itemsTabContent

        // TEXT tab content — typography + geometry for the selected text
        // item (see TextItemPanel.qml). No ground-truth capture of the
        // ITEMS/SLIDE tabs' own designs exists either (see rightTabRow's
        // header comment), so this is the first of the three actually
        // built out, per the reference panel design supplied directly.
        TextItemPanel {
            id: textItemPanel
            x: 8
            y: 56
            width: 384
            visible: root.rightPanelTab === "text" && root.primarySelectedItem !== null
                && root.primarySelectedItem.kind === "text"
            target: visible ? root.primarySelectedItem : null
            undoHook: root.pushUndoSnapshot
            focusHook: function () { mCanvas.forceActiveFocus() }
            onChangeColorRequested: {
                root.bgModalTarget = "textColor"
                root.bgModalOpen = true
            }
            onChangeFontRequested: textFontMenu.openAt(textItemPanel, 14, 96, root)
        }

        Text {
            x: 8
            y: 56
            width: 384
            wrapMode: Text.Wrap
            visible: root.rightPanelTab === "text" && !(root.primarySelectedItem !== null && root.primarySelectedItem.kind === "text")
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 11
            text: qsTr("Select a text item on the canvas to edit its formatting.")
        }

        // SLIDE tab — no design captured for this one either (see
        // rightTabRow's header comment); a plain placeholder until there's
        // an actual per-slide settings panel to build here.
        Text {
            x: 8
            y: 56
            width: 384
            horizontalAlignment: Text.AlignHCenter
            visible: root.rightPanelTab === "slide"
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 11
            text: qsTr("Coming soon")
        }
    }

    // Swallows the next left-click anywhere to close whichever floating
    // menu is open (slide row, canvas object, or the TEXT tab's font
    // weights), without intercepting input when all are closed. Only claims
    // the left button (MouseArea's default), so a right-click on a
    // different row/object still passes through to reopen the menu there
    // instead of being eaten here — same pattern as AppMenuBar.qml's own
    // catcher.
    MouseArea {
        anchors.fill: parent
        enabled: slideContextMenu.visible || canvasContextMenu.visible || textFontMenu.visible
        onClicked: {
            slideContextMenu.visible = false
            canvasContextMenu.visible = false
            textFontMenu.visible = false
        }
    }

    DropdownPanel {
        id: slideContextMenu
        visible: false
        model: [
            { label: "Edit" },
            { label: "Duplicate" },
            { divider: true },
            { label: "Delete", danger: true }
        ]
        onItemActivated: (label) => {
            switch (label) {
            case "Edit":
                slideModel.selectSlide(root.contextMenuSlideIndex)
                break
            case "Duplicate":
                // The ENGINE duplicates the slide (fresh id, its items copied); the list
                // and the copy's canvas are rebuilt from what it made.
                showSession.duplicateSlide(root.contextMenuSlideIndex)
                break
            case "Delete":
                // The engine deletes the slide (and anything scoped to it); the UI
                // drops its archive and lands on the neighbouring slide.
                showSession.removeSlide(root.contextMenuSlideIndex)
                break
            }
            slideContextMenu.visible = false
        }
    }

    // Right-click menu for canvas items: "Duplicate" copies this one item
    // (offset one tile down-right, like the item add cascading), "Delete"
    // removes just it.
    DropdownPanel {
        id: canvasContextMenu
        visible: false
        model: [
            { label: "Edit" },
            { label: "Duplicate" },
            { divider: true },
            { label: "Delete", danger: true }
        ]
        onItemActivated: (label) => {
            switch (label) {
            case "Edit": {
                const d = root.delegateForKey(root.canvasContextTarget)
                if (d && d.modelData && d.modelData.kind === "text")
                    d.textLabel.editing = true
                break
            }
            case "Duplicate":
                const src = root.canvasObjectByKey(root.canvasContextTarget)
                if (src)
                    root.addCanvasItem(src.kind, src)
                break
            case "Delete":
                // removeCanvasItems also deselects the removed keys.
                root.removeCanvasItems([root.canvasContextTarget])
                break
            }
            canvasContextMenu.visible = false
        }
    }

    // Font-weight picker for TextItemPanel's font row (see its
    // changeFontRequested signal) — no font-family list yet, just the
    // weight presets its rendering already understands (see itemTextLabel's
    // font.weight ternary above).
    DropdownPanel {
        id: textFontMenu
        visible: false
        model: [
            { label: "Regular" },
            { label: "Medium" },
            { label: "SemiBold" },
            { label: "Bold" }
        ]
        onItemActivated: (label) => {
            if (root.primarySelectedItem) {
                canvasHistory.push(qsTr("Change font weight"))
                root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { fontWeight: label })
            }
            textFontMenu.visible = false
        }
    }

    BackgroundColorModal {
        id: bgColorModal
        open: root.bgModalOpen
        title: root.bgModalTarget === "border" ? qsTr("Border Color")
            : root.bgModalTarget === "textColor" ? qsTr("Text Color") : qsTr("Background Color")
        onApplied: (selection) => {
            canvasHistory.push(qsTr("Change color"))
            if (root.bgModalTarget === "border" || root.bgModalTarget === "itemBackground") {
                // Solid colors only (gradients aren't a border/item-fill
                // concept here); the picker is shared with the slide
                // Background case, which does support gradients.
                const hex = selection.kind === "color" ? selection.color : selection.from
                root.selectedCanvasObjects.forEach((k) => {
                    const s = root.canvasObjectByKey(k)?.style
                    if (!s) return
                    if (root.bgModalTarget === "border") s.borderColor = hex
                    else s.backgroundColor = hex
                })
            } else if (root.bgModalTarget === "textColor") {
                // Solid only, same reasoning as border/itemBackground above.
                const hex = selection.kind === "color" ? selection.color : selection.from
                if (root.primarySelectedItem)
                    root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { color: hex })
            } else {
                slideStore.current.background = selection.kind === "color" ? selection.color : selection.from
            }
            root.bgModalOpen = false
        }
        onCancelled: root.bgModalOpen = false
    }

    CameraSourceModal {
        id: cameraSourceModal
        open: root.cameraModalOpen
        onApplied: (source) => {
            const item = root.addCanvasItem("camera")
            item.text = source.name
            root.cameraModalOpen = false
        }
        onCancelled: root.cameraModalOpen = false
    }

    MediaSourceModal {
        id: mediaSourceModal
        open: root.mediaModalOpen
        onApplied: (item) => {
            const canvasItem = root.addCanvasItem("media")
            canvasItem.text = item.name
            root.mediaModalOpen = false
        }
        onCancelled: root.mediaModalOpen = false
    }

    TimerSourceModal {
        id: timerSourceModal
        open: root.timerModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("timer")
            // startedAt is captured here (add-time), not inside the modal —
            // it has to reflect when the item actually lands on the canvas,
            // not when the picker happened to be configured.
            item.meta = Object.assign({}, config, { startedAt: Date.now() })
            root.timerModalOpen = false
        }
        onCancelled: root.timerModalOpen = false
    }

    ClockSourceModal {
        id: clockSourceModal
        open: root.clockModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("clock")
            item.meta = config
            root.clockModalOpen = false
        }
        onCancelled: root.clockModalOpen = false
    }

    ShapeSourceModal {
        id: shapeSourceModal
        open: root.shapeModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("shape")
            item.meta = config
            root.shapeModalOpen = false
        }
        onCancelled: root.shapeModalOpen = false
    }

    // ---- Keyboard shortcuts ---------------------------------------------
    // Every sequence reads from root.shortcutBindings (see its header
    // comment) rather than being written here — rebinding later means
    // changing that one map, not these elements. Gated on mCanvas having
    // focus, the same signal mCanvas's own Keys.onPressed (Delete/arrows/
    // Escape) already relies on to know nothing is mid-edit — a focused
    // TextInput (an item being typed into, a modal's search field, ...)
    // steals focus away from mCanvas, so these can't fire mid-typing.
    //
    // Undo/redo specifically ALSO stay enabled while root.anyTextEditing is
    // true — i.e. while a canvas text item is mid-edit specifically (not
    // just any focused text field elsewhere, like a slide rename or a hex
    // color input, which don't set this) — by explicit request: Ctrl+Z
    // should jump straight out of an in-progress text edit and revert the
    // whole step, not require clicking away first. canvasHistory.undo()/
    // redo() below hand focus back to mCanvas either way. Deliberately NOT
    // textUndoKey here — that only gets set on the edit session's first
    // KEYSTROKE (see beginTextEdit's comment), so it stays "" if you enter
    // edit mode and press Ctrl+Z before typing anything; anyTextEditing
    // flips true the instant edit mode itself starts.
    Shortcut {
        sequence: root.shortcutBindings.undo
        enabled: mCanvas.activeFocus || root.anyTextEditing
        onActivated: root.undo()
    }
    Shortcut {
        sequence: root.shortcutBindings.redo
        enabled: mCanvas.activeFocus || root.anyTextEditing
        onActivated: root.redo()
    }
    // Ctrl+Y — the second-standard redo chord, alongside Ctrl+Shift+Z above.
    // Both map to the same redo; applications that ship only one of the two
    // are why users report "redo is broken" on the other.
    Shortcut {
        sequence: root.shortcutBindings.redoAlt
        enabled: mCanvas.activeFocus || root.anyTextEditing
        onActivated: root.redo()
    }
    Shortcut {
        sequence: root.shortcutBindings.copy
        enabled: mCanvas.activeFocus && root.selectedCanvasObjects.length > 0
        onActivated: root.copySelectedCanvasObjects()
    }
    Shortcut {
        sequence: root.shortcutBindings.paste
        enabled: mCanvas.activeFocus && root.clipboardItems.length > 0
        onActivated: root.pasteClipboardItems()
    }
    Shortcut {
        sequence: root.shortcutBindings.duplicateSelected
        enabled: mCanvas.activeFocus && root.selectedCanvasObjects.length > 0
        onActivated: {
            const src = root.canvasObjectByKey(root.selectedCanvasObjects[0])
            if (src)
                root.addCanvasItem(src.kind, src)
        }
    }
    Shortcut {
        sequence: root.shortcutBindings.addText
        enabled: mCanvas.activeFocus && root.hasActiveSlide
        onActivated: root.addCanvasItem("text")
    }
    Shortcut {
        sequence: root.shortcutBindings.addCamera
        enabled: mCanvas.activeFocus && root.hasActiveSlide
        onActivated: root.cameraModalOpen = true
    }
}
