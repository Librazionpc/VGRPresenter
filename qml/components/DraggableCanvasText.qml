import QtQuick
import QtQuick.Shapes

// One movable/resizable text object on the slide canvas (title, verse,
// reference, ...). Literal colors, not Theme.* — AOT singleton-resolution
// limitation at this nesting depth.
Item {
    id: root

    default property alias content: contentHolder.data
    property bool selected: false
    property real minWidth: 40
    property real minHeight: 20
    readonly property int edgeMargin: 16

    // Persistent style border (SizeStyleCard's Border row), separate from
    // selectionOutline below. Off by default.
    property bool styleBorderEnabled: false
    property real styleBorderWidth: 2
    property color styleBorderColor: "#ffffff"
    property string styleBorderStyle: "line" // "line" | "dotted" | "dashed"
    property real styleBorderRadius: 0

    // SizeStyleCard's Padding/Opacity — applied to content+border only, not
    // to the selection chrome, so a faded/inset item's handles still show
    // up crisp and fully opaque while it's selected.
    property real stylePadding: 0
    property real styleOpacityPct: 100

    signal selectedRequested(var modifiers)
    // (x, y) in local coordinates — consumer maps into its own space to
    // escape mCanvas's clip.
    signal contextMenuRequested(real x, real y)
    signal resizing(var geom)
    signal dragEnded()

    width: contentHolder.childrenRect.width
    height: contentHolder.childrenRect.height

    Item {
        id: contentHolder
        anchors.fill: parent
        anchors.margins: root.stylePadding
        clip: true
        opacity: root.styleOpacityPct / 100
    }

    // Rectangle.border has no dash option, hence Shape; PathRectangle
    // handles the rounded corners directly. antialiasing: true so the
    // rounded corners render as a smooth curve, not a faceted polygon.
    Shape {
        anchors.fill: parent
        visible: root.styleBorderEnabled && root.styleBorderWidth > 0
        opacity: root.styleOpacityPct / 100
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
                radius: root.styleBorderRadius
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

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        cursorShape: Qt.IBeamCursor
        id: hoverArea
    }

    // Hover wash only — no always-on outline; selectionOutline below is
    // the only chrome an unselected object shows nothing of.
    Rectangle {
        anchors.fill: parent
        radius: 2
        color: "#0dffffff"
        opacity: (!root.selected && hoverArea.containsMouse) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    // True bounds, no edgeMargin offset — has to match where contentHolder
    // actually clips. Purple accent so it reads as a distinct state from
    // the neutral outline above, not just a brighter version of it.
    Rectangle {
        id: selectionOutline
        anchors.fill: parent
        border.color: "#6c5ce7"
        border.width: 1.5
        color: "#146c5ce7"
        radius: 2
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
        cursorShape: {
            if (handle.moving)
                return Qt.ClosedHandCursor
            const h = handle.resizeLeft || handle.resizeRight
            const v = handle.resizeTop || handle.resizeBottom
            if (h && v)
                return (handle.resizeLeft === handle.resizeTop) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
            return h ? Qt.SizeHorCursor : Qt.SizeVerCursor
        }

        onPressed: (mouse) => {
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
            if (Math.abs(dx) > 3 || Math.abs(dy) > 3)
                dragMoved = true

            if (handle.moving) {
                root.x = pressX + dx
                root.y = pressY + dy
                return
            }

            if (handle.resizeRight) {
                root.width = Math.max(root.minWidth, pressWidth + dx)
            } else if (handle.resizeLeft) {
                const nw = Math.max(root.minWidth, pressWidth - dx)
                root.x = pressX + (pressWidth - nw)
                root.width = nw
            }
            if (handle.resizeBottom) {
                root.height = Math.max(root.minHeight, pressHeight + dy)
            } else if (handle.resizeTop) {
                const nh = Math.max(root.minHeight, pressHeight - dy)
                root.y = pressY + (pressHeight - nh)
                root.height = nh
            }

            root.resizing({
                x: root.x, y: root.y, width: root.width, height: root.height,
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
            border.color: "#6c5ce7"
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
            border.color: "#6c5ce7"
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
