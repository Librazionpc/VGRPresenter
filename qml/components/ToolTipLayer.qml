import QtQuick
import VGRPresenterUI

// The window-level layer that DRAWS a hover tooltip (see TipService for who asks
// for one and why the text is published rather than drawn locally).
//
// One instance lives at the root of the window, above every screen (see
// Main.qml). It is cursor- and pointer-INERT (enabled: false, like the drag
// layer): a tip is paint, and a control that grew a window-sized child would
// stop receiving the very hover it is trying to explain.
//
// It draws in WINDOW space, deliberately NOT in the screens' scaled design
// canvas (contrast DragLayer, which is scaled to match the pane a chip came
// from): the bubble is text, and the window edge it must stay inside is a
// window-space fact. The owner's rect is asked for in window coordinates
// (mapToItem(null, …) on both corners, so an owner inside a scale transform
// still measures correctly) and the bubble is placed above it, centred, pulled
// inside the window's edges — and flipped below the owner when there is no room
// above (a pill in the top row of a card).
Item {
    id: root

    anchors.fill: parent
    // Above the drag layer (100000): a tip is explaining what the pointer is
    // over, and the two never legitimately show at once anyway.
    z: 100001
    // Nothing here takes the pointer: the layer only carries the bubble and
    // passes every event on (the drag layer's own rule).
    enabled: false

    // A beat before the bubble appears, so sweeping the pointer across a row of
    // pills does not flash a tip off every one of them. The tip is dismissed
    // immediately, never on a delay.
    property int showDelay: 350
    property int gap: 7        // distance from the owner
    property int edge: 6       // keep-out from the window's edges

    readonly property bool wanted: TipService.text !== "" && TipService.owner !== null
    property bool shown: false

    // Party trick of the whole file: nothing is placed until the text and the
    // owner are both known, and a moving owner (a card re-laid out under a
    // stationary pointer) re-places the bubble instead of stranding it.
    function update() {
        // The owner went away, was hidden or disabled, or the tip was cleared.
        if (!root.wanted || !root.ownerShown()) {
            arm.stop()
            root.shown = false
            return
        }
        // Already up for this owner: keep it up and only re-place it, so a
        // control whose tip text updates under the pointer (a pill closed by a
        // click) does not blink out and back through the delay.
        if (root.shown) {
            root.place()
            return
        }
        arm.restart()
    }

    function ownerShown() {
        const o = TipService.owner
        if (!o)
            return false
        try {
            // Same eligibility walk as AppCursor: a tip belongs to something the
            // operator can actually see (a card scrolled out of view keeps its
            // own tip out of the window).
            let it = o
            while (it) {
                if (it.visible === false || it.enabled === false)
                    return false
                it = it.parent
            }
            // Scrolled or panned clean out of the window: the control is not on
            // screen any more, so its tip is not either (the bubble would
            // otherwise sit clamped against the window edge, explaining nothing).
            const tl = o.mapToItem(null, 0, 0)
            const br = o.mapToItem(null, o.width, o.height)
            return br.x > 0 && br.y > 0 && tl.x < root.width && tl.y < root.height
        } catch (err) {
            return false       // a destroyed owner reads as gone
        }
    }

    function place() {
        const o = TipService.owner
        if (!o || bubble.width <= 0)
            return
        // Both corners, so an owner inside the screens' scale transform measures
        // in the window units the bubble is drawn in.
        const tl = o.mapToItem(null, 0, 0)
        const br = o.mapToItem(null, o.width, o.height)
        const w = Math.max(0, br.x - tl.x)
        const h = Math.max(0, br.y - tl.y)
        bubble.x = Math.max(root.edge,
                            Math.min(root.width - bubble.width - root.edge,
                                     tl.x + w / 2 - bubble.width / 2))
        // Above the owner by default...
        let y = tl.y - bubble.height - root.gap
        // ...unless that would leave the window (or be clipped by its top).
        if (y < root.edge)
            y = br.y + root.gap
        bubble.y = Math.max(root.edge, Math.min(root.height - bubble.height - root.edge, y))
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

    // The owner can MOVE under a stationary pointer — a pane laid out again, a
    // card scrolled by a wheel event (which is not a pointer move, so the hover
    // area above legitimately still reports hovered). The bubble travels with
    // it instead of being left behind over whatever slid underneath.
    Connections {
        target: root.shown && TipService.owner ? TipService.owner : null
        function onXChanged() { root.place() }
        function onYChanged() { root.place() }
        function onWidthChanged() { root.place() }
        function onHeightChanged() { root.place() }
    }

    Rectangle {
        id: bubble
        visible: root.shown && root.wanted
        width: tipText.implicitWidth + 18
        height: tipText.implicitHeight + 12
        radius: Theme.radiusSm
        color: Theme.card
        border.width: 1
        border.color: Qt.alpha(Theme.accent, 0.35)

        // The bubble's width follows the text, and the placement depends on that
        // width: re-place when it changes (a longer tip from another control).
        onWidthChanged: if (root.shown) root.place()
        onHeightChanged: if (root.shown) root.place()

        // A soft accent wash on the top edge — the same raised-card tell every
        // popup in this app wears, so the tip reads as ours and not as a system
        // tooltip.
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
            font.pixelSize: 11
        }
    }
}
