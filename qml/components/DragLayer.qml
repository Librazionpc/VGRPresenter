import QtQuick
import VGRPresenterUI

Item {
    id: root
    anchors.fill: parent
    z: 100000
    // Nothing here takes the pointer: the layer only carries the chip and passes every event on.
    enabled: false

    readonly property bool dragging: ghost.Drag.active
    // What is being carried: { kind, items: [{ ref, name, type, meta }] }
    readonly property var payload: ghost.payload

    // Starts a drag. (x, y) are window coordinates of the pointer.
    function begin(payload, label, x, y) {
        ghost.payload = payload
        ghost.label = label
        ghost.count = payload.items ? payload.items.length : 1
        root.move(x, y)
        ghost.visible = true
        ghost.Drag.active = true
    }
    function move(x, y) {
        const origin = root.mapToItem(null, 0, 0)
        const s = root.scale > 0 ? root.scale : 1
        ghost.x = (x - origin.x) / s + 12
        ghost.y = (y - origin.y) / s + 12
    }
    // The pointer was released: whatever DropArea is under the chip gets the drop.
    function drop() {
        if (ghost.Drag.active)
            ghost.Drag.drop()
        ghost.Drag.active = false
        ghost.visible = false
        ghost.payload = null
    }
    // The drag was abandoned (Escape): nothing is dropped.
    function cancel() {
        ghost.Drag.active = false
        ghost.visible = false
        ghost.payload = null
    }

    Rectangle {
        id: ghost
        property var payload: null
        property string label: ""
        property int count: 1
        visible: false
        width: chip.width + 20
        height: 28
        radius: 14
        color: "#e61a1c26"
        border.color: Theme.accent
        border.width: 1

        Drag.dragType: Drag.Internal
        Drag.keys: ["app-drag"]
        Drag.source: ghost
        Drag.hotSpot.x: 0
        Drag.hotSpot.y: 0

        Text {
            id: chip
            anchors.centerIn: parent
            text: ghost.count > 1 ? qsTr("%1  (+%2)").arg(ghost.label).arg(ghost.count - 1) : ghost.label
            color: Theme.textPrimary
            font.family: Theme.fontFamily; font.pixelSize: 14
            elide: Text.ElideRight
            width: Math.min(implicitWidth, 260)
        }
    }
}