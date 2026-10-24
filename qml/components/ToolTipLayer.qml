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
    property int showDelay: 1000
    property int gap: 7        // distance from the owner
    property int edge: 6       // keep-out from the window's edges

    readonly property bool wanted: TipService.text !== "" && TipService.owner !== null
    property bool shown: false
    // The owner the visible (or arming) bubble actually belongs to — tells
    // "this owner's facts just changed" apart from "the pointer moved to a
    // DIFFERENT owner", which only the latter can ever need to re-earn a delay.
    property var shownOwner: null

    // Party trick of the whole file: nothing is placed until the text and the
    // owner are both known, and a moving owner (a card re-laid out under a
    // stationary pointer) re-places the bubble instead of stranding it.
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
        // A genuinely different owner. The toolbar-pill sweep default swaps
        // the bubble in place with no re-delay and no blank gap; a list row
        // that asked for its own delay (HoverDetail.alwaysDelay) earns one
        // every time instead of inheriting whatever was already up.
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

    // ALWAYS under the cursor (user call), not centred over the owner's own
    // rect: a wide owner (a whole list row) used to plant the bubble at the
    // row's midpoint regardless of where in it the pointer actually was.
    // AppCursor.point is the same position-truth every PositionHoverArea
    // already reads, in the same window space this layer draws in.
    function place() {
        const p = AppCursor.point
        if (!p || bubble.width <= 0)
            return
        bubble.x = Math.max(root.edge,
                            Math.min(root.width - bubble.width - root.edge,
                                     p.x - bubble.width / 2))
        // Below the cursor by default...
        let y = p.y + root.gap
        // ...unless that would leave the window (or be clipped at its bottom).
        if (y + bubble.height > root.height - root.edge)
            y = p.y - bubble.height - root.gap
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

    // The bubble follows the cursor now (see place()), so it has to re-place
    // on every pointer move while it is up, not just once when it appears.
    Connections {
        target: root.shown ? AppCursor : null
        function onPointerMoved() { root.place() }
    }

    Rectangle {
        id: bubble
        visible: root.shown && root.wanted
        // A single-line bubble ran as wide as the text was long — a full
        // paragraph's worth read as one unbroken strip across the window.
        // tipText wraps once it would exceed maxTextWidth, and the bubble
        // follows its wrapped size, so a long tip reads as a proper box
        // instead of a line.
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
            font.pixelSize: 12
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
            width: Math.min(implicitWidth, bubble.maxTextWidth)
        }
    }
}
