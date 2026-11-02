import QtQuick
import VGRPresenterUI

MouseArea {
    id: root

    // { kind, items: [{ ref, name, type, meta }] }
    property var payload: null
    property string label: ""
    property var payloadProvider: null
    property string providerLabel: ""
    property bool dragEnabled: true
    property var owner: null

    // The click's modifiers ride the signal (see the header note). A handler
    // that ignores them — most of them do — is unaffected by the parameter.
    signal activated(int modifiers)
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
    function resolvePayload() {
        if (!root.payloadProvider)
            return root.payload
        const r = root.payloadProvider()
        if (r && r.payload !== undefined) {
            root.providerLabel = r.label !== undefined ? String(r.label) : ""
            return r.payload
        }
        root.providerLabel = ""
        return r
    }
    onPositionChanged: (mouse) => {
        if (!root.pressed || !root.dragEnabled || !root.dragLayerRef)
            return
        const p = root.mapToItem(null, mouse.x, mouse.y)
        if (!root.started) {
            if (Math.abs(mouse.x - root.pressPoint.x) + Math.abs(mouse.y - root.pressPoint.y) < 8)
                return
            const pl = root.resolvePayload()
            if (!pl)
                return
            root.started = true
            root.dragLayerRef.begin(pl, root.providerLabel !== "" ? root.providerLabel : root.label, p.x, p.y)
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
        if (!wasDrag && mouse.button === Qt.LeftButton && mouse.x >= 0 && mouse.y >= 0 && mouse.x <= root.width && mouse.y <= root.height)
            root.activated(mouse.modifiers)
    }
    onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) root.opened() }
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