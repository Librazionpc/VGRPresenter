import QtQuick
import VGRPresenterUI

// The drag-to-connect port dot on a routing-board card's edge — and the ONE
// connection indicator (per user's model): grey at rest, accent when the
// row is CONNECTED to any bus, hovered, or dragging from it. Line + card
// highlight carry the rest of the feedback.
//
// The gesture must never fall to the page Flickable: preventStealing keeps
// events flowing to this area even past its own bounds, so the whole drag
// belongs to the port (without it the board scrolls mid-connect).
Rectangle {
    id: root

    property color accent: Theme.success
    // Row routed into any bus (set by the delegate via rowRouted()).
    property bool connected: false
    // True only while a connect-drag originated from THIS port.
    property bool dragActive: false
    // Source ports are drag handles; bus ports are decorations (the drag
    // originates from the source side) — disable their input so clicks and
    // right-clicks pass through to the card underneath.
    property bool interactive: true
    // Bus ports are always-colored (they announce which signal types the
    // bus accepts — green audio-in / blue video-in); source ports stay
    // grey at rest and light on hover/drag only.
    property bool alwaysColored: false

    signal connectStarted(real x, real y)
    signal connectMoved(real x, real y)
    signal connectFinished(real x, real y)

    width: 8; height: 8; radius: 4
    color: root.alwaysColored || root.connected || hover.hovered || root.dragActive
           ? root.accent : Theme.border
    border.color: Theme.surface
    border.width: 1

    MouseArea {
        id: hover
        anchors.fill: parent
        anchors.margins: -6
        hoverEnabled: true
        enabled: root.interactive
        cursorShape: Qt.CrossCursor
        preventStealing: true
        onPressed: (mouse) => root.connectStarted(mouse.x, mouse.y)
        onPositionChanged: (mouse) => root.connectMoved(mouse.x, mouse.y)
        onReleased: (mouse) => root.connectFinished(mouse.x, mouse.y)
    }
}
