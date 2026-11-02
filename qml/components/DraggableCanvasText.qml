import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

Item {
    id: root

    default property alias content: contentHolder.data
    property bool selected: false
    property real minWidth: 40
    property real minHeight: 20
    readonly property int edgeMargin: 16

    property CanvasItemStyle style: null
    property bool drawsOwnBody: false

    property bool contentHovered: false

    signal selectedRequested(var modifiers)
    // (NO contextMenuRequested: the canvas's right-click item menu is gone by
    // request — see the note where its MouseArea used to be.)
    signal resizing(var geom)
    signal moving(real dx, real dy, bool snapDisabled)
    signal dragEnded()

    width: contentHolder.childrenRect.width
    height: contentHolder.childrenRect.height

    // ---- the resize affordance stays INSIDE the edit canvas ------------------
    readonly property real handleDotSize: 7
    readonly property real handleHalf: root.handleDotSize / 2
    // How far OUTSIDE one side of the item still lies inside the canvas: 0 when
    // the item is flush with (or past) that side, edgeMargin when it is well in.
    function roomFor(side) {
        const p = root.parent
        if (!p || p.width === undefined || p.width <= 0)
            return root.edgeMargin
        if (side === "left")   return Math.max(0, Math.min(root.edgeMargin, root.x))
        if (side === "right")  return Math.max(0, Math.min(root.edgeMargin, p.width - (root.x + root.width)))
        if (side === "top")    return Math.max(0, Math.min(root.edgeMargin, root.y))
        return Math.max(0, Math.min(root.edgeMargin, p.height - (root.y + root.height)))
    }
    readonly property real roomLeft: root.roomFor("left")
    readonly property real roomRight: root.roomFor("right")
    readonly property real roomTop: root.roomFor("top")
    readonly property real roomBottom: root.roomFor("bottom")
    readonly property real dotAtLeft: root.roomLeft >= root.handleHalf ? -root.handleHalf : 0
    readonly property real dotAtRight: root.roomRight >= root.handleHalf ? root.width - root.handleHalf
                                                                        : root.width - root.handleDotSize
    readonly property real dotAtTop: root.roomTop >= root.handleHalf ? -root.handleHalf : 0
    readonly property real dotAtBottom: root.roomBottom >= root.handleHalf ? root.height - root.handleHalf
                                                                          : root.height - root.handleDotSize
    readonly property string handleSuffix: root.objectName !== "" ? "_" + root.objectName : ""

    // Local shorthands so every consumer below reads one name instead of
    // re-testing for a null style object.
    readonly property real stylePadding: style ? style.padding : 0
    readonly property color styleBackgroundColor: style ? style.backgroundColor : "transparent"
    readonly property bool styleBorderEnabled: style ? style.borderEnabled : false
    readonly property real styleBorderWidth: style ? style.borderWidth : 2
    readonly property color styleBorderColor: style ? style.borderColor : "#ffffff"
    readonly property string styleBorderStyle: style ? style.borderStyle : "line"
    readonly property real styleCornerRadius: style ? style.cornerRadius : 0
    readonly property real styleOpacity: style ? style.opacity : 1

    opacity: root.styleOpacity

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

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#ffffff"
        border.width: 2
        opacity: 0.22
        radius: root.styleCornerRadius
    }

    // True bounds, no edgeMargin offset — has to match where contentHolder
    // actually clips. Purple accent, and the ONE outline the canvas draws.
    Rectangle {
        id: selectionOutline
        anchors.fill: parent
        border.color: Theme.accent
        border.width: 2
        color: Qt.alpha(Theme.accent, 0.08)
        radius: root.styleCornerRadius
        opacity: root.selected ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
    }

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
            if (visible && enabled && (pressed || AppCursor.hovered(handle)))
                AppCursor.push(handle.targetShape(), handle,
                               pressed ? AppCursor.priorityPressed : AppCursor.priorityHandle)
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
        onEnabledChanged: {
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

    ResizeHandle {
        objectName: "canvasHandleEdgeTop" + root.handleSuffix
        resizeTop: true
        x: -root.roomLeft; y: -root.roomTop
        width: root.width + root.roomLeft + root.roomRight; height: root.roomTop
    }
    ResizeHandle {
        objectName: "canvasHandleEdgeBottom" + root.handleSuffix
        resizeBottom: true
        x: -root.roomLeft; y: root.height
        width: root.width + root.roomLeft + root.roomRight; height: root.roomBottom
    }
    ResizeHandle {
        objectName: "canvasHandleEdgeLeft" + root.handleSuffix
        resizeLeft: true
        x: -root.roomLeft; y: -root.roomTop
        width: root.roomLeft; height: root.height + root.roomTop + root.roomBottom
    }
    ResizeHandle {
        objectName: "canvasHandleEdgeRight" + root.handleSuffix
        resizeRight: true
        x: root.width; y: -root.roomTop
        width: root.roomRight; height: root.height + root.roomTop + root.roomBottom
    }

    // Dots straddle the true corner (FreeShow convention); ResizeHandle hit
    // area is enlarged past the dot for an easier grab.
    Repeater {
        model: 4
        delegate: Rectangle {
            id: cornerHandle
            required property int index
            readonly property bool isRight: index % 2 === 1
            readonly property bool isBottom: index >= 2

            objectName: "canvasDotCorner" + index + root.handleSuffix
            width: root.handleDotSize
            height: root.handleDotSize
            radius: 1
            color: "#ffffff"
            border.color: Theme.accent
            border.width: 1
            // Clamped to the canvas (dotAt* — nothing here is ever clipped away).
            x: isRight ? root.dotAtRight : root.dotAtLeft
            y: isBottom ? root.dotAtBottom : root.dotAtTop
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

            objectName: "canvasDotEdge" + index + root.handleSuffix
            width: root.handleDotSize
            height: root.handleDotSize
            radius: 1
            color: "#ffffff"
            border.color: Theme.accent
            border.width: 1
            x: isLeft ? root.dotAtLeft : isRight ? root.dotAtRight : root.width / 2 - width / 2
            y: isTop ? root.dotAtTop : isBottom ? root.dotAtBottom : root.height / 2 - height / 2
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