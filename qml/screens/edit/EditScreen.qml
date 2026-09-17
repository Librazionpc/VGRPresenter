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

    height: 900
    width: 1440

    clip: true
    color: "#12131a"

    // Mock CRUD backend (src/SlideListModel.{h,cpp}) — stands in for the
    // real show/slide data source. Seeded with the same ground-truth
    // content the static array used to hold, but "Add slide" and selecting
    // a row now mutate real model state instead of pointing at fixed data.
    SlideListModel {
        id: slideModel
    }

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
    // bgModalTarget says which row opened it ("background" or "border" —
    // see SizeStyleCard's Border row), so one shared picker instance can
    // serve both instead of duplicating it.
    property bool bgModalOpen: false
    property string bgModalTarget: "background"
    property var slideBackground: ({ kind: "color", color: "#1a2240" })

    // Size & Style panel state (SizeStyleCard, in the right panel below
    // Background) — session-only, not persisted per-slide since
    // SlideListModel has no style fields yet. One style object per
    // canvas-object key (title/verse/ref/date), not shared globally —
    // otherwise turning a style on for one object turned it on for all of
    // them. Kept as a single generic map + get/set/setSelected trio (not
    // one property per style field) specifically so adding the next style
    // property — or reusing this whole thing for template items later —
    // is a one-line addition, not a new parallel set of plumbing each time.
    // SizeStyleCard shows/edits whichever object(s) are currently selected;
    // a key with no entry yet just means "all defaults".
    property var canvasItemStyles: ({})
    readonly property string primaryCanvasKey: root.selectedCanvasObjects.length > 0 ? root.selectedCanvasObjects[0] : ""

    function getCanvasItemStyle(key) {
        return root.canvasItemStyles[key] || {
            padding: 24, opacity: 100,
            enabled: false, width: 2, style: "line", radius: 12,
            color: { kind: "color", color: "#ffffff" }
        }
    }
    function setCanvasItemStyle(key, patch) {
        const current = root.getCanvasItemStyle(key)
        const merged = Object.assign({}, root.canvasItemStyles)
        merged[key] = Object.assign({}, current, patch)
        root.canvasItemStyles = merged
    }
    function setSelectedItemStyle(patch) {
        root.selectedCanvasObjects.forEach((k) => root.setCanvasItemStyle(k, patch))
    }
    // SizeStyleCard's controls mirror whichever object is primary-selected;
    // a plain property binding only syncs once and then breaks the moment
    // the user interacts with a control (same as any other one-way-then-
    // free binding in this file) — but here the "external" side (which
    // object is selected) keeps changing, so it has to be re-pushed
    // explicitly every time the selection changes, not just once.
    onPrimaryCanvasKeyChanged: {
        const s = root.getCanvasItemStyle(root.primaryCanvasKey)
        sizeStyleCard.padding = s.padding
        sizeStyleCard.styleOpacity = s.opacity
        sizeStyleCard.borderEnabled = s.enabled
        sizeStyleCard.borderWidth = s.width
        sizeStyleCard.borderStyle = s.style
        sizeStyleCard.radius = s.radius
        sizeStyleCard.borderColor = s.color
    }

    // The service-date line rendered near the logo — session-only, same
    // pattern as slideBackground above (not per-slide model
    // data, so no SlideListModel field). Made into a real canvas object
    // like title/verse/ref rather than a fixed Text, so it's actually
    // editable — "VGR" next to it stays a fixed logo mark baked into
    // slide_logo.png, not independent content, so it's left alone.
    property string canvasDateLine: "SUNDAY · AUGUST 16, 2026"

    // Which canvas text object(s) are currently selected — drives which
    // ones show the border+handle "selected" chrome (see
    // DraggableCanvasText, wrapping the title/verse/reference objects
    // below). Empty means nothing selected (clicking empty canvas clears
    // it). A plain or Ctrl+click replaces the selection with just that one;
    // Shift+click toggles it in/out of a multi-selection.
    property var selectedCanvasObjects: ["title"]

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

    function canvasObjectByKey(key) {
        if (key === "title") return titleObject
        if (key === "verse") return verseObject
        if (key === "ref") return refObject
        if (key === "date") return dateObject
        return null
    }

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
            const allKeys = ["title", "verse", "ref", "date"]
            allKeys.forEach((k) => {
                if (root.selectedCanvasObjects.indexOf(k) >= 0) return
                const o = root.canvasObjectByKey(k)
                if (o && o.visible) others.push({ x: o.x, y: o.y, width: o.width, height: o.height })
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
        const allKeys = ["title", "verse", "ref", "date"]
        allKeys.forEach((k) => {
            if (k === key) return
            const o = root.canvasObjectByKey(k)
            if (o && o.visible) others.push({ x: o.x, y: o.y, width: o.width, height: o.height })
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

    function deleteSelectedCanvasObjects() {
        root.selectedCanvasObjects.forEach((k) => {
            if (k === "title") slideModel.setActiveTitle("")
            else if (k === "verse") { slideModel.setActiveLine1(""); slideModel.setActiveLine2("") }
            else if (k === "ref") slideModel.setActiveRef("")
            else if (k === "date") root.canvasDateLine = ""
        })
        root.selectedCanvasObjects = []
    }

    // sourceItem is whichever DraggableCanvasText fired contextMenuRequested
    // — mapToItem from it (not from root directly) since (mx, my) are in
    // that object's own local space. Clamped to root's current bounds, same
    // reasoning as slideContextMenu's positioning below.
    function openCanvasContextMenu(sourceItem, mx, my, key) {
        const p = sourceItem.mapToItem(root, mx, my)
        canvasContextMenu.x = Math.max(0, Math.min(p.x, root.width - canvasContextMenu.width))
        canvasContextMenu.y = Math.max(0, Math.min(p.y, root.height - canvasContextMenu.height))
        root.canvasContextTarget = key
        canvasContextMenu.visible = true
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
    Rectangle {
        id: mCanvas
        x: 286
        y: 142
        height: 760
        width: 760
        clip: true
        color: "#0f1015"

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

        Repeater {
            model: 14 * 14
            delegate: Rectangle {
                required property int index
                x: (index % 14) * 56 + 2
                y: Math.floor(index / 14) * 14 + 14
                width: 2
                height: 2
                radius: 1
                color: "#232530"
            }
        }

        Image {
            x: 14
            y: 170
            source: Qt.resolvedUrl("assets/slide_bg.png")
        }
        Image {
            x: 74
            y: 212
            source: Qt.resolvedUrl("assets/slide_logo.png")
        }
        Text {
            x: 78
            y: 218
            color: "#eef0f6"
            font.family: "Inter"
            font.pixelSize: 7
            font.weight: Font.Bold
            text: qsTr("VGR")
        }
        DraggableCanvasText {
            id: dateObject
            x: 264
            y: 212
            width: 221
            selected: root.isCanvasObjectSelected("date") || dateLabel.editing
            onSelectedRequested: (mods) => root.handleCanvasSelect("date", mods)
            onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(dateObject, mx, my, "date")
            onResizing: (geom) => root.applyCanvasResize("date", geom)
            onDragEnded: root.endCanvasDrag()
            styleBorderEnabled: root.getCanvasItemStyle("date").enabled
            styleBorderWidth: root.getCanvasItemStyle("date").width
            styleBorderStyle: root.getCanvasItemStyle("date").style
            styleBorderRadius: root.getCanvasItemStyle("date").radius
            styleBorderColor: {
                const c = root.getCanvasItemStyle("date").color
                return c.kind === "gradient" ? c.from : c.color
            }
            stylePadding: root.getCanvasItemStyle("date").padding
            styleOpacityPct: root.getCanvasItemStyle("date").opacity

            EditableCanvasLabel {
                id: dateLabel
                width: parent.width
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignHCenter
                text: root.canvasDateLine
                onCommitted: (value) => root.canvasDateLine = value
                onSelectRequested: (mods) => root.handleCanvasSelect("date", mods)
                onMoveRequested: (dx, dy, snapDisabled) => {
                    if (!root.isCanvasObjectSelected("date"))
                        root.handleCanvasSelect("date", 0)
                    root.applyCanvasMove("date", dx, dy, snapDisabled)
                }
                onDragEnded: root.endCanvasDrag()
            }
        }

        // Canvas text objects — each one a DraggableCanvasText so it's an
        // actual movable object (click to select, drag anywhere on it to
        // reposition) instead of a fixed-position Text glued to the ground
        // truth's original pixel coordinates. Default x/y below reproduce
        // those original coordinates; once dragged, drag sets x/y directly
        // and these formulas no longer apply (same one-way-then-free
        // pattern as the CUSTOM hex fields in BackgroundColorModal.qml).

        DraggableCanvasText {
            id: titleObject
            x: (mCanvas.width - width) / 2
            y: 288
            width: 461
            selected: root.isCanvasObjectSelected("title") || titleLabel.editing
            onSelectedRequested: (mods) => root.handleCanvasSelect("title", mods)
            onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(titleObject, mx, my, "title")
            onResizing: (geom) => root.applyCanvasResize("title", geom)
            onDragEnded: root.endCanvasDrag()
            styleBorderEnabled: root.getCanvasItemStyle("title").enabled
            styleBorderWidth: root.getCanvasItemStyle("title").width
            styleBorderStyle: root.getCanvasItemStyle("title").style
            styleBorderRadius: root.getCanvasItemStyle("title").radius
            styleBorderColor: {
                const c = root.getCanvasItemStyle("title").color
                return c.kind === "gradient" ? c.from : c.color
            }
            stylePadding: root.getCanvasItemStyle("title").padding
            styleOpacityPct: root.getCanvasItemStyle("title").opacity

            Column {
                width: parent.width

                EditableCanvasLabel {
                    id: titleLabel
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    color: "#f2f4fa"
                    font.family: "Inter"
                    font.pixelSize: 44
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    text: slideModel.activeTitle
                    onCommitted: (value) => slideModel.setActiveTitle(value)
                    onSelectRequested: (mods) => root.handleCanvasSelect("title", mods)
                    onMoveRequested: (dx, dy, snapDisabled) => {
                        if (!root.isCanvasObjectSelected("title"))
                            root.handleCanvasSelect("title", 0)
                        root.applyCanvasMove("title", dx, dy, snapDisabled)
                    }
                    onDragEnded: root.endCanvasDrag()
                }
            }
        }

        DraggableCanvasText {
            id: verseObject
            x: (mCanvas.width - width) / 2
            y: 402
            width: 460
            selected: root.isCanvasObjectSelected("verse") || line1Label.editing || line2Label.editing
            onSelectedRequested: (mods) => root.handleCanvasSelect("verse", mods)
            onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(verseObject, mx, my, "verse")
            onResizing: (geom) => root.applyCanvasResize("verse", geom)
            onDragEnded: root.endCanvasDrag()
            styleBorderEnabled: root.getCanvasItemStyle("verse").enabled
            styleBorderWidth: root.getCanvasItemStyle("verse").width
            styleBorderStyle: root.getCanvasItemStyle("verse").style
            styleBorderRadius: root.getCanvasItemStyle("verse").radius
            styleBorderColor: {
                const c = root.getCanvasItemStyle("verse").color
                return c.kind === "gradient" ? c.from : c.color
            }
            stylePadding: root.getCanvasItemStyle("verse").padding
            styleOpacityPct: root.getCanvasItemStyle("verse").opacity

            Column {
                width: parent.width
                spacing: 4

                EditableCanvasLabel {
                    id: line1Label
                    width: parent.width
                    color: "#c7cbd8"
                    font.family: "Inter"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: slideModel.activeLine1
                    onCommitted: (value) => slideModel.setActiveLine1(value)
                    onSelectRequested: (mods) => root.handleCanvasSelect("verse", mods)
                    onMoveRequested: (dx, dy, snapDisabled) => {
                        if (!root.isCanvasObjectSelected("verse"))
                            root.handleCanvasSelect("verse", 0)
                        root.applyCanvasMove("verse", dx, dy, snapDisabled)
                    }
                    onDragEnded: root.endCanvasDrag()
                }
                EditableCanvasLabel {
                    id: line2Label
                    width: parent.width
                    color: "#c7cbd8"
                    font.family: "Inter"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: slideModel.activeLine2
                    onCommitted: (value) => slideModel.setActiveLine2(value)
                    onSelectRequested: (mods) => root.handleCanvasSelect("verse", mods)
                    onMoveRequested: (dx, dy, snapDisabled) => {
                        if (!root.isCanvasObjectSelected("verse"))
                            root.handleCanvasSelect("verse", 0)
                        root.applyCanvasMove("verse", dx, dy, snapDisabled)
                    }
                    onDragEnded: root.endCanvasDrag()
                }
            }
        }

        DraggableCanvasText {
            id: refObject
            x: (mCanvas.width - width) / 2
            y: 448
            width: 300
            visible: slideModel.activeRef !== ""
            selected: root.isCanvasObjectSelected("ref") || refLabel.editing
            onSelectedRequested: (mods) => root.handleCanvasSelect("ref", mods)
            onContextMenuRequested: (mx, my) => root.openCanvasContextMenu(refObject, mx, my, "ref")
            onResizing: (geom) => root.applyCanvasResize("ref", geom)
            onDragEnded: root.endCanvasDrag()
            styleBorderEnabled: root.getCanvasItemStyle("ref").enabled
            styleBorderWidth: root.getCanvasItemStyle("ref").width
            styleBorderStyle: root.getCanvasItemStyle("ref").style
            styleBorderRadius: root.getCanvasItemStyle("ref").radius
            styleBorderColor: {
                const c = root.getCanvasItemStyle("ref").color
                return c.kind === "gradient" ? c.from : c.color
            }
            stylePadding: root.getCanvasItemStyle("ref").padding
            styleOpacityPct: root.getCanvasItemStyle("ref").opacity

            EditableCanvasLabel {
                id: refLabel
                width: parent.width
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 11
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignHCenter
                text: slideModel.activeRef
                onCommitted: (value) => slideModel.setActiveRef(value)
                onSelectRequested: (mods) => root.handleCanvasSelect("ref", mods)
                onMoveRequested: (dx, dy, snapDisabled) => {
                    if (!root.isCanvasObjectSelected("ref"))
                        root.handleCanvasSelect("ref", 0)
                    root.applyCanvasMove("ref", dx, dy, snapDisabled)
                }
                onDragEnded: root.endCanvasDrag()
            }
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

        Rectangle {
            id: camPanel
            x: 562
            y: 448
            height: 84
            width: 112

            Shape {
                anchors.fill: parent
                ShapePath {
                    fillGradient: LinearGradient {
                        x1: camPanel.width * 0.5; x2: camPanel.width * 0.5
                        y1: 0; y2: camPanel.height
                        GradientStop { color: "#ff123326"; position: 0 }
                        GradientStop { color: "#ff07130e"; position: 1 }
                    }
                    strokeColor: "#000"
                    strokeWidth: 0
                    PathRectangle { width: camPanel.width; height: camPanel.height; radius: 8 }
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
        }

        Text {
            x: 6
            y: 10
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 10
            text: qsTr("Sunday Service  ·  Slide %1 — %2").arg(slideModel.activeNum).arg(slideModel.activeTitle)
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
                            console.log("[edit] add content:", typeChip.modelData.kind)
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
                text: qsTr("12 slides · Worship template")
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

        Rectangle {
            x: 12
            y: 80
            height: 31
            width: 256
            border.color: "#232530"
            border.width: 1
            color: "#1a1c26"
            radius: 8

            Image { x: 10; y: 9; source: Qt.resolvedUrl("assets/so_search_ic.png") }
            Text {
                x: 32
                y: 8
                color: "#5c6475"
                font.family: "Inter"
                font.pixelSize: 11
                text: qsTr("Search this show…")
            }
        }

        Text {
            x: 12
            y: 122
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 10
            text: qsTr("SLIDES · 12")
        }

        Flickable {
            id: slideListFlick
            x: 12
            y: 140
            width: 256
            height: leftPanel.height - 140 - 12
            clip: true
            contentWidth: width
            contentHeight: slideListColumn.height
            boundsBehavior: Flickable.StopAtBounds

        Column {
            id: slideListColumn
            width: parent.width
            spacing: 8

            Repeater {
                model: slideModel
                delegate: SlideListItem {
                    onSelected: slideModel.selectSlide(index)
                    onDuplicateRequested: slideModel.duplicateSlide(index)
                    onDeleteRequested: slideModel.removeSlide(index)
                    onContextMenuRequested: (mx, my) => {
                        // Escape slideListFlick's clip region: reposition
                        // the one shared menu instance (declared at root
                        // level) in root's coordinate space. root.width/
                        // height track the actual window size (root is
                        // anchors.fill: parent on a resizable window), so
                        // clamp against those — not the 1440x900 design
                        // canvas — so the menu can't land partly or fully
                        // off-screen when the window is narrower/shorter.
                        const p = mapToItem(root, mx, my)
                        slideContextMenu.x = Math.max(0, Math.min(p.x, root.width - slideContextMenu.width))
                        slideContextMenu.y = Math.max(0, Math.min(p.y, root.height - slideContextMenu.height))
                        root.contextMenuSlideIndex = index
                        slideContextMenu.visible = true
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

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                text: qsTr("Background")
            }
            Rectangle {
                x: 256
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                border.color: "#3a4a7a"
                border.width: 1
                radius: 5
                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: root.slideBackground.kind === "gradient" ? root.slideBackground.from : root.slideBackground.color
                    }
                    GradientStop {
                        position: 1
                        color: root.slideBackground.kind === "gradient" ? root.slideBackground.to : root.slideBackground.color
                    }
                }
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
                        root.bgModalTarget = "background"
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
            padding: root.getCanvasItemStyle(root.primaryCanvasKey).padding
            styleOpacity: root.getCanvasItemStyle(root.primaryCanvasKey).opacity
            radius: root.getCanvasItemStyle(root.primaryCanvasKey).radius
            borderWidth: root.getCanvasItemStyle(root.primaryCanvasKey).width
            borderStyle: root.getCanvasItemStyle(root.primaryCanvasKey).style
            borderColor: root.getCanvasItemStyle(root.primaryCanvasKey).color
            borderEnabled: root.getCanvasItemStyle(root.primaryCanvasKey).enabled
            onPaddingChanged: root.setSelectedItemStyle({ padding: padding })
            onStyleOpacityChanged: root.setSelectedItemStyle({ opacity: styleOpacity })
            onRadiusChanged: root.setSelectedItemStyle({ radius: radius })
            onBorderWidthChanged: root.setSelectedItemStyle({ width: borderWidth })
            onBorderStyleChanged: root.setSelectedItemStyle({ style: borderStyle })
            onBorderEnabledChanged: root.setSelectedItemStyle({ enabled: borderEnabled })
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
            case "Duplicate":
                slideModel.duplicateSlide(root.contextMenuSlideIndex)
                break
            case "Delete":
                slideModel.removeSlide(root.contextMenuSlideIndex)
                break
            }
            slideContextMenu.visible = false
        }
    }

    // Right-click menu for canvas text objects (title/verse/reference).
    // "Duplicate" duplicates the whole slide (these fields aren't
    // independent list items the way slide rows are, so there's nothing
    // narrower to duplicate); "Delete" clears just that object's own text.
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
            case "Edit":
                switch (root.canvasContextTarget) {
                case "title": titleLabel.editing = true; break
                case "verse": line1Label.editing = true; break
                case "ref":   refLabel.editing = true; break
                case "date":  dateLabel.editing = true; break
                }
                break
            case "Duplicate":
                slideModel.duplicateSlide(slideModel.activeNum - 1)
                break
            case "Delete":
                switch (root.canvasContextTarget) {
                case "title":
                    slideModel.setActiveTitle("")
                    break
                case "verse":
                    slideModel.setActiveLine1("")
                    slideModel.setActiveLine2("")
                    break
                case "ref":
                    slideModel.setActiveRef("")
                    break
                case "date":
                    root.canvasDateLine = ""
                    break
                }
                break
            }
            canvasContextMenu.visible = false
        }
    }

    BackgroundColorModal {
        id: bgColorModal
        open: root.bgModalOpen
        onApplied: (selection, opacityPct) => {
            if (root.bgModalTarget === "border")
                root.setSelectedItemStyle({ color: selection })
            else
                root.slideBackground = selection
            root.bgModalOpen = false
        }
        onCancelled: root.bgModalOpen = false
    }
}
