import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property real defaultWidth: 320
    property real minWidth: 200
    // Leave the content beside the sidebar at least 360 px.
    property real maxWidth: parent ? Math.max(minWidth, parent.width - 360) : 520
    property bool resizable: true

    // What the pane puts in the sidebar.
    default property alias content: body.data

    width: defaultWidth
    color: Theme.windowBg
    // Above the content beside it, so the drag strip's outer half is reachable.
    z: 10

    Item {
        id: body
        anchors.fill: parent
    }

    // The divider line.
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    SplitHandle {
        visible: root.resizable
        value: root.width
        minValue: root.minWidth
        maxValue: root.maxWidth
        onDragged: (v) => root.width = v
        onResetRequested: root.width = root.defaultWidth
    }
}