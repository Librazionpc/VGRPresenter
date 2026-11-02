import QtQuick
import VGRPresenterUI

Item {
    id: root

    // true: a vertical divider, dragged left/right. false: horizontal, dragged up/down.
    property bool vertical: true
    property real value: 0
    property real minValue: 0
    property real maxValue: 100000
    property color lineColor: Theme.accent

    readonly property real thickness: 8
    readonly property bool active: hover.hovered || drag.pressed

    // A drag to `value` (already clamped to minValue..maxValue).
    signal dragged(real value)
    // Double-click: put the divider back to its default.
    signal resetRequested()

    x: vertical ? value - thickness / 2 : 0
    y: vertical ? 0 : value - thickness / 2
    width: vertical ? thickness : (parent ? parent.width : 0)
    height: vertical ? (parent ? parent.height : 0) : thickness
    // Above the content on both sides of the divider, so the whole strip takes the pointer.
    z: 20

    Rectangle {
        anchors.centerIn: parent
        width: root.vertical ? 2 : parent.width
        height: root.vertical ? parent.height : 2
        color: root.active ? root.lineColor : "transparent"
    }

    PositionHoverArea {
        id: hover
        anchors.fill: parent
        cursorShape: root.vertical ? Qt.SizeHorCursor : Qt.SizeVerCursor
    }

    MouseArea {
        id: drag
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            // In the parent's space, so the strip moving under the pointer can't feed back.
            const p = mapToItem(root.parent, mouse.x, mouse.y)
            const v = root.vertical ? p.x : p.y
            root.dragged(Math.max(root.minValue, Math.min(root.maxValue, v)))
        }
        onDoubleClicked: root.resetRequested()
    }
}