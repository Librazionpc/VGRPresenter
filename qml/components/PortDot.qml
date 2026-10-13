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

    // All three carry SCENE (window) coordinates, not the dot's own frame: the
    // hit area below is deliberately larger than the dot, and mapping the
    // event's own x/y through the DOT added that margin to every reported
    // point. The board measures the drop point against the bus rows, so the
    // old mapping landed 10px right/down of the pointer and drops the user
    // aimed at a card were refused (or read as the next card along). The owner
    // converts scene -> its own space once (AudioVideoScreen.dragPoint).
    signal connectStarted(real sceneX, real sceneY)
    signal connectMoved(real sceneX, real sceneY)
    signal connectFinished(real sceneX, real sceneY)

    width: 8; height: 8; radius: 4
    // At rest, a source port used Theme.border (#232530) — nearly the same
    // value as the dark card it sits on, so the drag handle was effectively
    // invisible until you were already hovering it (found live: a video
    // source's port read as "missing" against its card, making the connect
    // gesture undiscoverable). textMuted keeps the same "not yet connected"
    // read but is actually visible against the card background.
    // `hover` is this item's own MouseArea (declared below): its hover flag is
    // `containsMouse`, NOT `hovered` — a HoverHandler's name. Reading
    // `hover.hovered` left the dot at Theme.textMuted no matter where the
    // pointer was, so the "this port is live, drag from here" brighten never
    // happened and the connect gesture stayed undiscoverable.
    color: root.alwaysColored || root.connected || hover.containsMouse || root.dragActive
           ? root.accent : Theme.textMuted
    border.color: Theme.surface
    border.width: 1

    MouseArea {
        id: hover
        // The grab target is bigger than the 8px dot on purpose: the dot is the
        // gesture's HANDLE, and a 20x20 target made the connect drag fiddly to
        // start ("the connection drag is difficult"). 28x28 is well clear of a
        // neighbouring port — board rows sit 64px apart — so it can never steal
        // a neighbour's press.
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
