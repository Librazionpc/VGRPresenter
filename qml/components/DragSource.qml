import QtQuick
import VGRPresenterUI

// Makes a row, tile or card draggable to another pane. Fill the item with it (anchors.fill: parent). A plain click is `activated`, a
// double-click is `opened`, the right button is `contextRequested`, and a press that moves past the threshold becomes a drag carried
// by the window's DragLayer (so a click that ends a drag is not also a click).
//
//   DragSource {
//       anchors.fill: parent
//       payload: ({ kind: "show_drawer", items: [{ ref: path, name: "Amazing Grace" }] })
//       label: "Amazing Grace"
//       onActivated: ...
//       onOpened: ...
//   }
//
// `kind` is one of the engine's drop kinds (see bps::library::AcceptsDrop): show_drawer, media, audio, overlay, scripture, camera...
// (The mouse handlers are this component's own - use the signals below, not onPressed / onReleased.)
MouseArea {
    id: root

    // { kind, items: [{ ref, name, type, meta }] }
    property var payload: null
    property string label: ""
    property bool dragEnabled: true
    // Optional owner for callbacks from an embedded DragSource. Its internal
    // MouseArea is also named `root`, so callers must not rely on a captured
    // outer id with that name inside signal handlers.
    property var owner: null

    signal activated()
    signal opened()
    signal contextRequested(real x, real y)

    readonly property var dragLayerRef: Window.window ? Window.window.dragLayer : null
    property bool started: false
    property point pressPoint

    hoverEnabled: true
    acceptedButtons: Qt.LeftButton
    cursorShape: root.started ? Qt.ClosedHandCursor : Qt.PointingHandCursor
    // Mouse only: a Flickable underneath (the verse list, the grids) must not take the press for a scroll and cancel the drag.
    preventStealing: true

    onPressed: (mouse) => {
        root.started = false
        root.pressPoint = Qt.point(mouse.x, mouse.y)
    }
    onPositionChanged: (mouse) => {
        if (!root.pressed || !root.dragEnabled || !root.dragLayerRef || !root.payload)
            return
        const p = root.mapToItem(null, mouse.x, mouse.y)
        if (!root.started) {
            if (Math.abs(mouse.x - root.pressPoint.x) + Math.abs(mouse.y - root.pressPoint.y) < 8)
                return
            root.started = true
            root.dragLayerRef.begin(root.payload, root.label, p.x, p.y)
        } else {
            root.dragLayerRef.move(p.x, p.y)
        }
    }
    onReleased: (mouse) => {
        const wasDrag = root.started
        if (wasDrag && root.dragLayerRef)
            root.dragLayerRef.drop()
        root.started = false
        // Where the pointer is now, not hover state: a control drawn above this one can take the hover away.
        if (!wasDrag && mouse.x >= 0 && mouse.y >= 0 && mouse.x <= root.width && mouse.y <= root.height)
            root.activated()
    }
    onDoubleClicked: root.opened()
    onCanceled: {
        if (root.started && root.dragLayerRef)
            root.dragLayerRef.cancel()
        root.started = false
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => root.contextRequested(point.position.x, point.position.y)
    }
}
