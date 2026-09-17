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

    // Mock CRUD backend (src/SlideListModel.{h,cpp}) — stands in for the
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

    // Switching slides swaps the canvas: the store re-archives the outgoing
    // slide's working set and installs the incoming one. addSlide()
    // auto-selects, so a fresh slide lands here with an empty canvas too.
    Connections {
        target: slideModel
        function onActiveSlideChanged() {
            slideStore.load(slideModel.activeSlideId)
        }
    }

    // True when a slide is active — the roster starts EMPTY (no hardcoded
    // slides), so this is false on a fresh launch until "Add slide" is used.
    // Drives the empty state: canvas and right panel grey out (nothing to
    // edit), while the slide list stays fully live (adding a slide is the
    // way out).
    readonly property bool hasActiveSlide: slideModel.activeSlideId > 0

    // Output roster comes from the OutputListModel singleton (src/OutputListModel.{h,cpp})
    // — the SAME model Settings · Outputs (OutputsScreen.qml) edits, so adding
    // or renaming an output there shows up here too instead of two arrays
    // drifting apart.

    // Right-click context menu for slide rows (Edit / Duplicate / Delete) —
    // see slideContextMenu near the end of this file. Tracks which row it
    // was opened for, since only one shared DropdownPanel instance exists.
    property int contextMenuSlideIndex: -1

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

    // Background/Border only have a visible effect on a "text" item — every
    // other kind's content (camera's live-preview Shape, the generic
    // media/audio/shape/timer/clock placeholder) fills its box edge-to-edge
    // with its own opaque visual, completely covering whatever the style
    // background/border would draw underneath. Drives those controls being
    // greyed out for anything else, rather than looking active but doing
    // nothing when applied.
    readonly property bool primarySelectedSupportsFill: root.primarySelectedItemStyle === null
        || root.primarySelectedItemStyle.kind === "text"

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
        canvasSnapGuides.guides = []
    }

    function nudgeSelectedCanvasObjects(dx, dy) {
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
        const n = slideStore.current.items.length
        const isCamera = kind === "camera"
        const item = slideStore.createItem(
            kind,
            copyFrom ? copyFrom.text : "",
            copyFrom ? copyFrom.x + 16 : 40 + (n % 6) * 20,
            copyFrom ? copyFrom.y + 16 : 40 + (n % 6) * 20,
            copyFrom ? copyFrom.width : (isCamera ? 112 : 220),
            copyFrom ? copyFrom.height : (isCamera ? 84 : 44),
            copyFrom ? copyFrom.style : null)
        slideStore.addItem(item)
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
    readonly property var contentTypes: [
        { kind: "text",   icon: "Aa", label: "Text" },
        { kind: "camera", icon: "◎", label: "Camera" },
        { kind: "media",  icon: "▶", label: "Media" },
        { kind: "audio",  icon: "♪", label: "Audio" },
        { kind: "shape",  icon: "□", label: "Shape" },
        { kind: "timer",  icon: "⏱", label: "Timer" },
        { kind: "clock",  icon: "◷", label: "Clock" }
    ]

    // ---- Middle toolbar ----
    Rectangle {
        id: mHdr
        x: 280
        y: 48
        height: 48
        width: 760
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
        Text {
            x: 556
            y: 17
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Autosaved ✓")
        }
        Text {
            x: 638
            y: 17
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Fit")
        }
        Row {
            x: 662
            y: 10
            spacing: 4

            Rectangle {
                height: 28
                width: 28
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↺" }
            }
            Rectangle {
                height: 28
                width: 28
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.pixelSize: 12; text: "↻" }
            }
            Rectangle {
                height: 28
                width: 34
                color: "#1a1c26"
                radius: 7
                Text { anchors.centerIn: parent; color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 10; text: qsTr("100%") }
            }
        }
    }
    Rectangle {
        x: 280
        y: 96
        height: 1
        width: 760
        color: "#232530"
    }

    // ---- Canvas ----
    // width: 754, not 760 — 286 + 760 = 1046 overlapped 6px into rightPanel
    // (x: 1040). 754 lands exactly on rightPanel's left edge.
    Rectangle {
        id: mCanvas
        x: 286
        y: 308
        height: 428
        width: 754
        clip: true
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
            anchors.fill: parent
            z: -1
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
                onSelectedRequested: (mods) => root.handleCanvasSelect(modelData.key, mods)
                onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(canvasItemObject, mx, my, modelData.key)
                onResizing: (geom) => root.applyCanvasResize(modelData.key, geom)
                onMoving: (dx, dy, snapDisabled) => root.applyCanvasMove(modelData.key, dx, dy, snapDisabled)
                onDragEnded: root.endCanvasDrag()

                EditableCanvasLabel {
                    id: itemTextLabel
                    visible: canvasItemObject.modelData.kind === "text"
                    width: parent.width
                    color: "#f2f4fa"
                    font.family: "Inter"
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    text: canvasItemObject.modelData.text
                    // Live per-keystroke propagation into the item object
                    // — the thumbnail binds to this same object, so rows
                    // update as you type, not only on commit.
                    onEdited: (value) => canvasItemObject.modelData.text = value
                    onCommitted: (value) => canvasItemObject.modelData.text = value
                    onSelectRequested: (mods) => root.handleCanvasSelect(canvasItemObject.modelData.key, mods)
                    onMoveRequested: (dx, dy, snapDisabled) => {
                        if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                            root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                        root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                    }
                    onDragEnded: root.endCanvasDrag()
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
                        text: qsTr("CAM 1")
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
                // name, so Media/Audio/Shape/Timer/Clock at least read as
                // visually distinct from each other while real per-kind
                // content is still future work.
                Rectangle {
                    id: genericPlaceholder
                    readonly property var typeInfo: {
                        for (let i = 0; i < root.contentTypes.length; ++i) {
                            if (root.contentTypes[i].kind === canvasItemObject.modelData.kind)
                                return root.contentTypes[i]
                        }
                        return { icon: "?", label: canvasItemObject.modelData.kind }
                    }
                    visible: !["text", "camera"].includes(canvasItemObject.modelData.kind)
                    anchors.fill: parent
                    color: "#1a1c26"
                    border.color: "#3a4155"
                    border.width: 1
                    radius: 6

                    Column {
                        anchors.centerIn: parent
                        spacing: 4

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#9b8ff5"
                            font.pixelSize: 20
                            text: genericPlaceholder.typeInfo.icon
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#aeb6c8"
                            font.family: "Inter"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            text: genericPlaceholder.typeInfo.label.toUpperCase()
                        }
                    }

                    // Click to select, drag the body to move — shared
                    // press/threshold/delta body (see CanvasDragArea), the
                    // same mechanics EditableCanvasLabel uses internally.
                    CanvasDragArea {
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
                    onClicked: slideModel.addSlide()
                }
            }
        }
    }

    Rectangle {
        x: 882
        y: 841
        height: 32
        width: 124
        border.color: "#232530"
        border.width: 1
        color: "#1a1c26"
        radius: 8

        Row {
            anchors.fill: parent
            Text { width: 41; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.pixelSize: 12; text: "−" }
            Text { width: 42; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 10; text: qsTr("100%") }
            Text { width: 41; height: parent.height; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: "#8a94a6"; font.pixelSize: 12; text: "+" }
        }
    }

    Rectangle {
        id: addContentChip
        x: 286
        y: 838
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
        y: 837
        height: 46
        width: 388
        border.color: "#2a2f3a"
        border.width: 1
        color: "#151824"
        radius: 14
        visible: root.addMenuOpen

        Row {
            x: 10
            y: 6
            spacing: 4

            Repeater {
                model: root.contentTypes
                delegate: Rectangle {
                    id: typeChip
                    required property var modelData
                    readonly property bool isDefault: modelData.kind === "text"

                    height: 34
                    width: 48
                    radius: 8
                    border.width: typeChip.isDefault ? 1 : 0
                    border.color: "#406c5ce7"
                    color: typeChip.isDefault ? "#206c5ce7" : (typeArea.containsMouse ? "#232733" : "#1b1e2a")
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        y: 4
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        color: typeChip.isDefault ? "#9b8ff5" : "#9aa0b5"
                        font.family: "Inter"
                        font.pixelSize: 12
                        text: typeChip.modelData.icon
                    }
                    Text {
                        y: 22
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        color: typeChip.isDefault ? "#eef1f8" : "#6b7080"
                        font.family: "Inter"
                        font.pixelSize: 8
                        text: typeChip.modelData.label
                    }

                    MouseArea {
                        id: typeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.addCanvasItem(typeChip.modelData.kind)
                            root.addMenuOpen = false
                        }
                    }
                }
            }
        }
    }

    // ---- Left panel: slide list ----
    Rectangle {
        id: leftPanel
        y: 48
        height: 852
        width: 280
        border.color: "#232530"
        border.width: 1
        color: "#12131a"

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
                    // The design space the items' coordinates live in — the
                    // canvas's actual on-screen size, so the mini-canvas
                    // scale can never drift from what you see while editing.
                    canvasWidth: mCanvas.width
                    canvasHeight: mCanvas.height
                    onSelected: slideModel.selectSlide(index)
                    onDuplicateRequested: slideModel.duplicateSlide(index)
                    onDeleteRequested: slideModel.removeSlide(index)
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
                    onClicked: slideModel.addSlide()
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
        x: 1040
        y: 48
        height: 852
        width: 400
        color: "#0f1015"
        // Greyed with the canvas in the empty state — Background/Size & Style
        // target the active slide, which doesn't exist yet.
        enabled: root.hasActiveSlide
        opacity: root.hasActiveSlide ? 1 : 0.35

        Row {
            x: 32
            y: 19
            spacing: 56

            Text { color: "#ff4d3d"; font.family: "Inter"; font.pixelSize: 11; font.weight: Font.Medium; text: qsTr("ITEMS") }
            Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("TEXT") }
            Text { color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 11; text: qsTr("SLIDE") }
        }
        Rectangle { y: 40; height: 3; width: 100; color: "#ff4d3d"; radius: 1.50 }
        Rectangle { y: 43; height: 1; width: 400; color: "#232530" }

        Grid {
            x: 12
            y: 62
            columns: 2
            rowSpacing: 11
            columnSpacing: 12

            Repeater {
                model: OutputListModel
                delegate: Rectangle {
                    id: outputCard
                    required property string name
                    required property string badge
                    required property bool active

                    height: 143
                    width: 182
                    border.color: outputCard.active ? "#85261f" : "#232530"
                    border.width: 1
                    color: "#16171e"
                    radius: 8

                    Rectangle {
                        x: 6
                        y: 6
                        height: 110
                        width: 170
                        clip: true
                        color: "#101116"
                        radius: 4

                        Rectangle {
                            x: 6
                            y: 6
                            height: 16
                            width: parent.width * 0.32
                            color: "#b3000000"
                            radius: 4

                            Text {
                                anchors.centerIn: parent
                                color: "#e2e8f0"
                                font.family: "Inter"
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                text: outputCard.badge
                            }
                        }
                    }
                    Row {
                        x: 6
                        y: 120
                        width: 170
                        Text {
                            color: "#e2e8f0"
                            font.family: "Inter"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            text: outputCard.name
                        }
                    }
                }
            }
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
            onChangeBorderRequested: {
                root.bgModalTarget = "border"
                root.bgModalOpen = true
            }
        }
    }

    // Swallows the next left-click anywhere to close whichever context menu
    // is open (slide row or canvas object), without intercepting input when
    // both are closed. Only claims the left button (MouseArea's default),
    // so a right-click on a different row/object still passes through to
    // reopen the menu there instead of being eaten here — same pattern as
    // AppMenuBar.qml's own catcher.
    MouseArea {
        anchors.fill: parent
        enabled: slideContextMenu.visible || canvasContextMenu.visible
        onClicked: {
            slideContextMenu.visible = false
            canvasContextMenu.visible = false
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
            case "Duplicate": {
                // The copy gets a fresh stable id; clone the canvas onto it.
                const newId = slideModel.duplicateSlide(root.contextMenuSlideIndex)
                if (newId > 0)
                    slideStore.cloneSlide(
                        slideModel.slideIdAt(root.contextMenuSlideIndex), newId)
                break
            }
            case "Delete":
                // Drop the archive BEFORE removing the row, so the item
                // objects die with the slide instead of leaking.
                slideStore.dropSlide(slideModel.slideIdAt(root.contextMenuSlideIndex))
                slideModel.removeSlide(root.contextMenuSlideIndex)
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

    BackgroundColorModal {
        id: bgColorModal
        open: root.bgModalOpen
        title: root.bgModalTarget === "border" ? qsTr("Border Color") : qsTr("Background Color")
        onApplied: (selection) => {
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
            } else {
                slideStore.current.background = selection.kind === "color" ? selection.color : selection.from
            }
            root.bgModalOpen = false
        }
        onCancelled: root.bgModalOpen = false
    }
}
