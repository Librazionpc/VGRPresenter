import QtQuick
import VGRPresenterUI

// One typed connection point on the Settings · Routing canvas.
//
// The canvas POSITIONS these (it owns the geometry formulas), so a port is a
// bare dot that knows which node and WHICH SIGNAL it belongs to and reports its
// drag in SCENE coordinates — the canvas converts once with mapFromItem(null, …).
// Reporting the dot's own local x/y would fold its hit area's margin into every
// point, which is the bug PortDot.qml's header records from the Audio & Video
// board.
//
// ONE DOT, CENTRED (FreeShow's own `.port` at `top: 50%`) — except where a card
// really carries two signals, which is an output whose transport takes both video
// and audio: that card draws two dots, stacked symmetrically around the same
// centre (video above, audio below), so the pair still reads as one node.
//
// A bus keeps ONE dot however much it carries, and the KIND is the colour that dot
// reads as: on drop the canvas resolves which half the wire actually carries (see
// RoutingScreen::resolveKind), so a both-bus sends its picture or its sound
// depending on what it lands on.
Rectangle {
    id: root

    // The canvas's key for the node this port belongs to.
    property string nodeKey: ""
    // The signal this dot READS as — "audio" | "video" | "style". The canvas
    // resolves the half a drop carries, so a both-bus's one dot can start either
    // wire.
    property string kind: ""
    // The signal colour this port carries (audio / video / style).
    property color portColor: Theme.accent
    // Something is wired through this port — it stays lit instead of grey.
    property bool connected: false
    // This port is where the live drag started / where it would land.
    property bool dragOrigin: false
    property bool dragTarget: false

    // The node key AND the kind travel with every step: the canvas binds ONE
    // handler per port list, so the port — not the delegate — is what says which
    // node and which signal the gesture started on.
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
        // The dot is 9px; the handle is 25px. A 9px grab target made the connect
        // gesture fiddly (the Audio & Video board's port was widened to 28px for
        // exactly this). One dot per card edge now, so there is no neighbour to
        // meet — the handle can be comfortable without overlapping anything.
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
