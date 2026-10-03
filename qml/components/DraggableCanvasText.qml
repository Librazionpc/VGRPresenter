import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// One movable/resizable text object on the slide canvas (title, verse,
// reference, ...). Literal colors, pending migration to Theme.* (the old
// "AOT singleton-resolution" reason is disproven — see DropdownPanel.qml).
Item {
    id: root

    default property alias content: contentHolder.data
    property bool selected: false
    property real minWidth: 40
    property real minHeight: 20
    readonly property int edgeMargin: 16

    // The item's persistent style (padding + border), as one CanvasItemStyle
    // object shared with the rest of the app — EditScreen creates one per
    // canvas item and hands the same instance here and to SizeStyleCard, so
    // a slider move shows up here with no mirroring layer. Null means "no
    // styling": defaults below apply (no padding, no border).
    property CanvasItemStyle style: null
    // True when the content draws its OWN body from that style - a circle, a line or a glyph shape, a vignette, screen corners.
    // The plain box fill and border below are then not drawn: they are a rectangle, and a rectangle behind a circle is
    // exactly what makes a circle read as a box.
    property bool drawsOwnBody: false

    // Drives the hover-wash Rectangle below. Set externally from whichever
    // content MouseArea is actually present (EditableCanvasLabel.hovered,
    // or a CanvasDragArea's own containsMouse) — not tracked by a second
    // MouseArea of this item's own layered on top of content: two stacked
    // hoverEnabled MouseAreas over the same region is exactly what made
    // click delivery flaky after a hover-leave/hover-enter cycle.
    property bool contentHovered: false

    signal selectedRequested(var modifiers)
    // (x, y) in local coordinates — consumer maps into its own space to
    // escape mCanvas's clip.
    signal contextMenuRequested(real x, real y)
    signal resizing(var geom)
    // Shift+drag-the-ring — per-event incremental deltas (not total-since-
    // press), matching EditableCanvasLabel.moveRequested's exact contract so
    // both feed the same consumer-side applyCanvasMove(key, dx, dy, ...)
    // accumulation logic identically.
    signal moving(real dx, real dy, bool snapDisabled)
    signal dragEnded()

    width: contentHolder.childrenRect.width
    height: contentHolder.childrenRect.height

    // Local shorthands so every consumer below reads one name instead of
    // re-testing for a null style object.
    readonly property real stylePadding: style ? style.padding : 0
    readonly property color styleBackgroundColor: style ? style.backgroundColor : "transparent"
    readonly property bool styleBorderEnabled: style ? style.borderEnabled : false
    readonly property real styleBorderWidth: style ? style.borderWidth : 2
    readonly property color styleBorderColor: style ? style.borderColor : "#ffffff"
    readonly property string styleBorderStyle: style ? style.borderStyle : "line"
    readonly property real styleCornerRadius: style ? style.cornerRadius : 0

    // Fill, full bounds (not inset by padding — same footprint as the
    // border below it shares corner radius with). Declared before
    // contentHolder so the fill sits behind the actual text/content. No
    // visible: check needed — "transparent" already renders as nothing.
    Rectangle {
        visible: !root.drawsOwnBody
        anchors.fill: parent
        radius: root.styleCornerRadius
        color: root.styleBackgroundColor
    }

    Item {
        id: contentHolder
        anchors.fill: parent
        anchors.margins: root.stylePadding
        clip: true
    }

    // Rectangle.border has no dash option, hence Shape; PathRectangle
    // handles the rounded corners directly. antialiasing: true so the
    // rounded corners render as a smooth curve, not a faceted polygon.
    Shape {
        anchors.fill: parent
        visible: !root.drawsOwnBody && root.styleBorderEnabled && root.styleBorderWidth > 0
        antialiasing: true
        ShapePath {
            strokeColor: root.styleBorderColor
            strokeWidth: root.styleBorderWidth
            fillColor: "transparent"
            strokeStyle: root.styleBorderStyle === "line" ? ShapePath.SolidLine : ShapePath.DashLine
            dashPattern: root.styleBorderStyle === "dotted" ? [1, 2] : [4, 3]
            capStyle: ShapePath.RoundCap
            // Without this, corners default to a bevel/round join at the
            // stroke level even when radius is 0 and the path itself is a
            // true sharp corner — miter is what actually gives a square.
            joinStyle: ShapePath.MiterJoin

            PathRectangle {
                x: 0
                y: 0
                width: root.width
                height: root.height
                radius: root.styleCornerRadius
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.margins: -root.edgeMargin
        acceptedButtons: Qt.RightButton
        onClicked: (mouse) => {
            const p = mapToItem(root, mouse.x, mouse.y)
            root.contextMenuRequested(p.x, p.y)
        }
    }

    // Very faint always-on outline so an unselected item's bounds stay
    // legible without hunting for them — kept dim enough to not read as
    // "selected" (selectionOutline below is a completely different color/
    // weight) and to not tire the eye with several items on screen at once.
    // radius follows the item's own cornerRadius — a hardcoded 2 here was
    // exactly the mystery radius on fresh boxes (border off, yet rounded).
    Rectangle {
        anchors.fill: parent
        radius: root.styleCornerRadius
        color: "transparent"
        border.color: "#ffffff"
        border.width: 2
        opacity: root.selected ? 0 : 0.22
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    // Hover wash.
    Rectangle {
        anchors.fill: parent
        radius: root.styleCornerRadius
        color: "#0dffffff"
        opacity: (!root.selected && root.contentHovered) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    // True bounds, no edgeMargin offset — has to match where contentHolder
    // actually clips. Purple accent so it reads as a distinct state from
    // the neutral outline above, not just a brighter version of it.
    Rectangle {
        id: selectionOutline
        anchors.fill: parent
        border.color: Theme.accent
        border.width: 2
        color: "#146c5ce7"
        radius: root.styleCornerRadius
        opacity: root.selected ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
    }

    // One hit area per edge/corner. Manual press/positionChanged tracking
    // (not drag.target) since resize grows one property while shrinking
    // another. Shift+drag moves instead of resizing.
    component ResizeHandle: MouseArea {
        id: handle
        // Not "left"/"right"/"top"/"bottom" — collide with Item's anchor
        // line properties.
        property bool resizeLeft: false
        property bool resizeRight: false
        property bool resizeTop: false
        property bool resizeBottom: false

        property bool moving: false
        property bool dragMoved: false
        property var pressModifiers: 0
        property real pressX: 0
        property real pressY: 0
        property real pressWidth: 0
        property real pressHeight: 0
        property real pressMouseX: 0
        property real pressMouseY: 0

        enabled: root.selected
        hoverEnabled: true
        // Cursor via the AppCursor override stack (see EditableCanvasLabel's
        // dragArea for the full why): the canvas sits under a scale (zoom)
        // transform and per-MouseArea cursorShape silently stops applying
        // there — push the computed shape instead; Main.qml's unscaled
        // AppCursorCatcher renders it. cursorShape remains as the zoom==1
        // fallback and computes the identical shape.
        function targetShape() {
            if (handle.moving)
                return Qt.ClosedHandCursor
            const h = handle.resizeLeft || handle.resizeRight
            const v = handle.resizeTop || handle.resizeBottom
            if (h && v)
                return (handle.resizeLeft === handle.resizeTop) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
            return h ? Qt.SizeHorCursor : Qt.SizeVerCursor
        }
        function syncCursor() {
            // AppCursor.hovered(handle) — position truth — covers
            // select-under-a-stationary-pointer AND is the only hover signal
            // besides `pressed`: containsMouse latches in this build
            // (hover-exit never delivers — KNOWN_ISSUES.md), so gating on it
            // left the size cursor stuck after leaving the handle.
            if (visible && enabled && (pressed || AppCursor.hovered(handle)))
                AppCursor.push(handle.targetShape(), handle)
            else
                AppCursor.pop(handle)
        }
        cursorShape: handle.targetShape()
        // Recompute from the pointer-position stream (every move) instead of
        // onContainsMouseChanged — same mechanism as PositionHoverArea.
        Connections {
            target: AppCursor
            function onPointerMoved() { handle.syncCursor() }
        }
        // Handles disable on deselect while the pointer may still be over
        // them — a disabled MouseArea stops receiving hover events, so
        // containsMouse can stay stale-true with nobody left to clear it.
        // Pop explicitly on disable (the visible flip inside editing also
        // routes here) so the pushed shape can't outlive the handle.
        onEnabledChanged: {
            // Deselect (and the editing visible-flip) route here: drop the
            // pushed size shape immediately; re-enabling re-checks with
            // position truth (syncCursor above).
            if (enabled)
                syncCursor()
            else
                AppCursor.pop(handle)
        }
        Component.onDestruction: AppCursor.pop(handle)
        onPressed: (mouse) => {
            syncCursor()
            root.forceActiveFocus()
            moving = (mouse.modifiers & Qt.ShiftModifier) !== 0
            dragMoved = false
            pressModifiers = mouse.modifiers
            pressX = root.x
            pressY = root.y
            pressWidth = root.width
            pressHeight = root.height
            const g = mapToItem(root.parent, mouse.x, mouse.y)
            pressMouseX = g.x
            pressMouseY = g.y
        }
        onReleased: {
            syncCursor()
            if (!dragMoved)
                root.selectedRequested(pressModifiers)
            root.dragEnded()
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            const g = mapToItem(root.parent, mouse.x, mouse.y)
            const dx = g.x - pressMouseX
            const dy = g.y - pressMouseY

            if (handle.moving) {
                // Per-event delta, not total-since-press: reset the tracked
                // point after every emit so this matches
                // EditableCanvasLabel's dragArea exactly (see the `moving`
                // signal's header comment — the consumer's applyCanvasMove
                // expects per-event increments, not absolute totals).
                if (dragMoved || Math.abs(dx) > 3 || Math.abs(dy) > 3) {
                    dragMoved = true
                    root.moving(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
                    pressMouseX = g.x
                    pressMouseY = g.y
                }
                return
            }

            if (Math.abs(dx) > 3 || Math.abs(dy) > 3)
                dragMoved = true

            // Computed into locals, never self-assigned onto root — a
            // consumer may bind root.x/y/width/height one-way to external
            // data (e.g. a Repeater delegate's modelData); self-assigning
            // here would permanently break that binding the first time a
            // resize happens (the same footgun LabeledSlider.qml's
            // controlled-component rewrite exists to avoid).
            let newX = root.x, newY = root.y, newWidth = root.width, newHeight = root.height

            if (handle.resizeRight) {
                newWidth = Math.max(root.minWidth, pressWidth + dx)
            } else if (handle.resizeLeft) {
                const nw = Math.max(root.minWidth, pressWidth - dx)
                newX = pressX + (pressWidth - nw)
                newWidth = nw
            }
            if (handle.resizeBottom) {
                newHeight = Math.max(root.minHeight, pressHeight + dy)
            } else if (handle.resizeTop) {
                const nh = Math.max(root.minHeight, pressHeight - dy)
                newY = pressY + (pressHeight - nh)
                newHeight = nh
            }

            root.resizing({
                x: newX, y: newY, width: newWidth, height: newHeight,
                left: handle.resizeLeft, right: handle.resizeRight,
                top: handle.resizeTop, bottom: handle.resizeBottom,
                snapDisabled: (mouse.modifiers & Qt.AltModifier) !== 0
            })
        }
    }

    ResizeHandle { resizeTop: true;    x: -root.edgeMargin; y: -root.edgeMargin; width: root.width + root.edgeMargin * 2; height: root.edgeMargin }
    ResizeHandle { resizeBottom: true; x: -root.edgeMargin; y: root.height;      width: root.width + root.edgeMargin * 2; height: root.edgeMargin }
    ResizeHandle { resizeLeft: true;   x: -root.edgeMargin; y: 0; width: root.edgeMargin; height: root.height }
    ResizeHandle { resizeRight: true;  x: root.width;       y: 0; width: root.edgeMargin; height: root.height }

    // Dots straddle the true corner (FreeShow convention); ResizeHandle hit
    // area is enlarged past the dot for an easier grab.
    Repeater {
        model: 4
        delegate: Rectangle {
            id: cornerHandle
            required property int index
            readonly property bool isRight: index % 2 === 1
            readonly property bool isBottom: index >= 2

            width: 7
            height: 7
            radius: 1
            color: "#ffffff"
            border.color: Theme.accent
            border.width: 1
            x: (isRight ? root.width : 0) - width / 2
            y: (isBottom ? root.height : 0) - height / 2
            scale: root.selected ? 1 : 0.4
            opacity: root.selected ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
            Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutBack } }

            ResizeHandle {
                anchors.centerIn: parent
                width: 18
                height: 18
                resizeLeft: !cornerHandle.isRight
                resizeRight: cornerHandle.isRight
                resizeTop: !cornerHandle.isBottom
                resizeBottom: cornerHandle.isBottom
            }
        }
    }

    Repeater {
        model: 4
        delegate: Rectangle {
            id: edgeHandle
            required property int index
            readonly property bool isTop: index === 0
            readonly property bool isRight: index === 1
            readonly property bool isBottom: index === 2
            readonly property bool isLeft: index === 3

            width: 7
            height: 7
            radius: 1
            color: "#ffffff"
            border.color: Theme.accent
            border.width: 1
            x: (isLeft ? 0 : isRight ? root.width : root.width / 2) - width / 2
            y: (isTop ? 0 : isBottom ? root.height : root.height / 2) - height / 2
            scale: root.selected ? 1 : 0.4
            opacity: root.selected ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
            Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutBack } }

            ResizeHandle {
                anchors.centerIn: parent
                width: 18
                height: 18
                resizeTop: edgeHandle.isTop
                resizeRight: edgeHandle.isRight
                resizeBottom: edgeHandle.isBottom
                resizeLeft: edgeHandle.isLeft
            }
        }
    }
}
