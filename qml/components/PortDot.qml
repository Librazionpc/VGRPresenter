import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property color accent: Theme.success
    // Row routed into any bus (set by the delegate via rowRouted()).
    property bool connected: false
    // True only while a connect-drag originated from THIS port.
    property bool dragActive: false
    property bool interactive: true
    property bool alwaysColored: false

    signal connectStarted(real sceneX, real sceneY)
    signal connectMoved(real sceneX, real sceneY)
    signal connectFinished(real sceneX, real sceneY)

    width: 8; height: 8; radius: 4
    color: root.alwaysColored || root.connected || hover.containsMouse || root.dragActive
           ? root.accent : Theme.textMuted
    border.color: Theme.surface
    border.width: 1

    MouseArea {
        id: hover
        anchors.fill: parent
        anchors.margins: -10
        hoverEnabled: true
        enabled: root.interactive
        cursorShape: Qt.CrossCursor
        preventStealing: true
        // Mapped from the MouseArea that RECEIVED the event (see the signals
        // above), to scene coordinates, in the same tick as the event.
        function sceneX(mx) { return hover.mapToItem(null, mx, 0).x }
        function sceneY(my) { return hover.mapToItem(null, 0, my).y }
        onPressed: (mouse) => root.connectStarted(hover.sceneX(mouse.x), hover.sceneY(mouse.y))
        onPositionChanged: (mouse) => root.connectMoved(hover.sceneX(mouse.x), hover.sceneY(mouse.y))
        onReleased: (mouse) => root.connectFinished(hover.sceneX(mouse.x), hover.sceneY(mouse.y))
    }
}