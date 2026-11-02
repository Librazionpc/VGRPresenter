import QtQuick
import VGRPresenterUI

Item {
    id: root

    anchors.fill: parent
    // Above the drag layer (100000): a tip is explaining what the pointer is
    // over, and the two never legitimately show at once anyway.
    z: 100001
    // Nothing here takes the pointer: the layer only carries the bubble and
    // passes every event on (the drag layer's own rule).
    enabled: false

    property int showDelay: 1000
    property int gap: 7        // distance from the owner
    property int edge: 6       // keep-out from the window's edges

    readonly property bool wanted: TipService.text !== "" && TipService.owner !== null
    property bool shown: false
    property var shownOwner: null

    function update() {
        // The owner went away, was hidden or disabled, or the tip was cleared.
        if (!root.wanted || !root.ownerShown()) {
            arm.stop()
            root.shown = false
            root.shownOwner = null
            return
        }
        const owner = TipService.owner
        // Same owner, facts changed in place (a pill closed by a click): keep
        // it up and only re-place it, never re-delay — this is not a switch.
        if (root.shown && root.shownOwner === owner) {
            root.place()
            return
        }
        if (root.shown && !TipService.forceDelay) {
            root.shownOwner = owner
            root.place()
            return
        }
        arm.stop()
        root.shown = false
        root.shownOwner = owner
        arm.restart()
    }

    function ownerShown() {
        const o = TipService.owner
        if (!o)
            return false
        try {
            let it = o
            while (it) {
                if (it.visible === false || it.enabled === false)
                    return false
                it = it.parent
            }
            const tl = o.mapToItem(null, 0, 0)
            const br = o.mapToItem(null, o.width, o.height)
            return br.x > 0 && br.y > 0 && tl.x < root.width && tl.y < root.height
        } catch (err) {
            return false       // a destroyed owner reads as gone
        }
    }

    // Place beside the pointer, not at a fixed window corner or the centre of
    // the hovered control (which can be a full-width list row).
    function place() {
        if (bubble.width <= 0)
            return
        const point = AppCursor.point
        if (!point)
            return

        const offset = 16
        const right = root.width - bubble.width - root.edge
        const bottom = root.height - bubble.height - root.edge
        const x = point.x + offset + bubble.width <= root.width - root.edge
                  ? point.x + offset : point.x - bubble.width - offset
        const y = point.y + offset + bubble.height <= root.height - root.edge
                  ? point.y + offset : point.y - bubble.height - offset
        bubble.x = Math.max(root.edge, Math.min(x, right))
        bubble.y = Math.max(root.edge, Math.min(y, bottom))
    }

    Timer {
        id: arm
        interval: root.showDelay
        onTriggered: {
            root.shown = true
            root.place()
        }
    }

    Connections {
        target: TipService
        function onChanged() { root.update() }
    }

    // Keep the bubble beside a moving pointer while it is visible.
    Connections {
        target: root.shown ? AppCursor : null
        function onPointerMoved() { root.place() }
    }

    Rectangle {
        id: bubble
        visible: root.shown && root.wanted
        readonly property int maxTextWidth: 460
        width: tipText.width + 18
        height: tipText.implicitHeight + 12
        radius: Theme.radiusSm
        color: Theme.card
        border.width: 1
        border.color: Qt.alpha(Theme.accent, 0.35)

        // The bubble's width follows the text, and the placement depends on that
        // width: re-place when it changes (a longer tip from another control).
        onWidthChanged: if (root.shown) root.place()
        onHeightChanged: if (root.shown) root.place()

        Rectangle {
            x: bubble.radius
            y: 0
            width: bubble.width - bubble.radius * 2
            height: 1
            color: Qt.alpha(Theme.accentLight, 0.30)
        }

        Text {
            id: tipText
            anchors.centerIn: parent
            text: TipService.text
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
            width: Math.min(implicitWidth, bubble.maxTextWidth)
        }
    }
}