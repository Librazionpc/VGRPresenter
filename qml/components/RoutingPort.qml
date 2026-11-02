import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    // The canvas's key for the node this port belongs to.
    property string nodeKey: ""
    property string kind: ""
    // The signal colour this port carries (audio / video / style).
    property color portColor: Theme.accent
    // Something is wired through this port — it stays lit instead of grey.
    property bool connected: false
    // This port is where the live drag started / where it would land.
    property bool dragOrigin: false
    property bool dragTarget: false

    signal dragStarted(string key, string kind, real sceneX, real sceneY)
    signal dragMoved(string key, string kind, real sceneX, real sceneY)
    signal dragEnded(string key, string kind, real sceneX, real sceneY)

    width: 9
    height: 9
    radius: 4.5
    color: (root.connected || root.dragOrigin || root.dragTarget || handle.containsMouse)
           ? root.portColor : Theme.textMuted
    border.color: Theme.surface
    border.width: 1.5
    z: 3

    Behavior on color { ColorAnimation { duration: 100 } }

    MouseArea {
        id: handle
        anchors.fill: parent
        anchors.margins: -6
        cursorShape: Qt.CrossCursor
        // The gesture belongs to the port, not to the canvas's Flickable: without
        // this the page scrolls away mid-connect.
        preventStealing: true

        function sceneX(mx) { return handle.mapToItem(null, mx, 0).x }
        function sceneY(my) { return handle.mapToItem(null, 0, my).y }

        onPressed: (mouse) => root.dragStarted(root.nodeKey, root.kind, handle.sceneX(mouse.x), handle.sceneY(mouse.y))
        onPositionChanged: (mouse) => root.dragMoved(root.nodeKey, root.kind, handle.sceneX(mouse.x), handle.sceneY(mouse.y))
        onReleased: (mouse) => root.dragEnded(root.nodeKey, root.kind, handle.sceneX(mouse.x), handle.sceneY(mouse.y))
    }
}