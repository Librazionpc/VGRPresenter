import QtQuick

// A thin vertical scrollbar for a Flickable/ListView. QtQuick.Controls'
// ScrollBar doesn't match this app's track/thumb visual language, so this
// is a small from-scratch replacement — hand it a `flickable` and it does
// the rest: computes its own thumb length/position from contentHeight,
// supports drag-to-scroll, and clicking the track jumps/pages. Fully
// interactive: the thumb grabs anywhere ON it (with a fat invisible grip
// margin so a thin bar is still easy to grab), and a track click JUMPS the
// content so the thumb centers on the click (a page step left the click
// target non-sticky — the classic "I clicked where I wanted to go" expect-
// ation; shift-click keeps the old page-up/page-down behavior).
// Reusable anywhere a scrollable list needs one instead of each screen
// hand-rolling its own track/thumb math.
Item {
    id: root

    property Flickable flickable
    property int trackWidth: 8
    property color trackColor: "#161823"
    property color thumbColor: "#3a3f4d"
    property int minThumbLength: 24
    // Horizontal mode: laid out along the BOTTOM, thumb maps contentX
    // (RoutingMatrixModal's sideways matrix). Everything else — fat grip,
    // hover, jump-to-click, drag — behaves identically.
    property bool horizontal: false
    // Fat invisible hover/drag zone around the thumb: an 8px bar with a 5px
    // margin is an 18px-wide grab target — mouse-reachable without pixel
    // hunting. The VISIBLE thumb stays trackWidth; only the hit area grows.
    property int gripMargin: 5
    // True (default): a track click JUMPS so the thumb centers on the click.
    // False (page mode, opt-in): click above/below pages by one viewport.
    property bool jumpToClick: true

    // Length along the bar = width (vertical) or height (horizontal); the
    // caller sizes that dimension. The CROSS dimension is the track width.
    width: horizontal ? implicitLength : trackWidth
    height: horizontal ? trackWidth : implicitLength
    property int implicitLength: 8   // caller overrides along the bar axis
    visible: flickable && (horizontal
        ? flickable.contentWidth > flickable.width + 1
        : flickable.contentHeight > flickable.height + 1)

    readonly property real maxScroll: !flickable ? 1
        : horizontal ? Math.max(flickable.contentWidth - flickable.width, 1)
                     : Math.max(flickable.contentHeight - flickable.height, 1)
    readonly property real thumbLength: !flickable ? 0
        : horizontal ? Math.max(minThumbLength, root.width * flickable.width / Math.max(flickable.contentWidth, 1))
                     : Math.max(minThumbLength, root.height * flickable.height / Math.max(flickable.contentHeight, 1))

    Rectangle {
        anchors.fill: parent
        radius: root.trackWidth / 2
        color: trackHover.hovered || thumbArea.drag.active ? Qt.lighter(root.trackColor, 1.6) : root.trackColor

        HoverHandler { id: trackHover }
    }

    function scrollPos() {
        if (!root.flickable) return 0
        return root.horizontal ? root.flickable.contentX : root.flickable.contentY
    }
    function setScrollPos(v) {
        if (!root.flickable) return
        if (root.horizontal) root.flickable.contentX = v
        else root.flickable.contentY = v
    }
    function page(direction) {
        if (!root.flickable) return
        const step = root.horizontal ? root.flickable.width : root.flickable.height
        root.setScrollPos(Math.max(0, Math.min(root.maxScroll, root.scrollPos() + direction * step)))
    }

    // Track interaction. NOTE the ordering: this MouseArea sits BEHIND the
    // thumb's own (declared later = on top), so a press that lands on the
    // thumb never reaches the track handler.
    MouseArea {
        id: trackArea
        anchors.fill: parent
        anchors.margins: -root.gripMargin   // clicks just beside a thin track count too
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => {
            if (!root.flickable) return
            const clickPos = root.horizontal ? mouse.x : mouse.y
            const at = clickPos - thumbLength / 2
            if (jumpToClick && !(mouse.modifiers & Qt.ShiftModifier)) {
                // JUMP: center the thumb on the click (clamped). Shift-click
                // keeps the classic page step for keyboard-style scrollers.
                const range = Math.max((root.horizontal ? root.width : root.height) - thumbLength, 1)
                root.setScrollPos(Math.max(0, Math.min(root.maxScroll, (at / range) * root.maxScroll)))
            } else {
                root.page(clickPos < thumbPos() ? -1 : 1)
            }
        }
    }

    function thumbPos() {
        const span = (root.horizontal ? root.width : root.height) - thumbLength
        return span > 0 ? span * (root.scrollPos() / root.maxScroll) : 0
    }

    Rectangle {
        id: thumb
        radius: root.trackWidth / 2
        // Hover feedback: brighten before you commit to the grab, the same
        // hover language every chip/button in this app uses.
        color: thumbArea.pressed ? Qt.lighter(root.thumbColor, 1.5)
             : thumbArea.containsMouse ? Qt.lighter(root.thumbColor, 1.25)
             : root.thumbColor
        // The thumb runs across the track; its length/position come from the
        // bar axis. Vertical: fixed trackWidth-wide, y driven. Horizontal:
        // fixed trackWidth-tall, x driven.
        width: root.horizontal ? root.thumbLength : root.trackWidth
        height: root.horizontal ? root.trackWidth : root.thumbLength
        Binding {
            target: thumb
            property: root.horizontal ? "x" : "y"
            value: root.thumbPos()
            when: !thumbArea.drag.active
        }

        // Drag state: the thumb's axis position is being set imperatively by
        // the drag — push it back into the Flickable's content position. Reads
        // the thumb's ACTUAL x/y (thumbPos() derives from the content position
        // being written here — it would never move).
        onXChanged: if (root.horizontal) thumb.syncFromDrag()
        onYChanged: if (!root.horizontal) thumb.syncFromDrag()
        function syncFromDrag() {
            if (!thumbArea.drag.active || !root.flickable) return
            const at = root.horizontal ? thumb.x : thumb.y
            const span = (root.horizontal ? root.width : root.height) - thumbLength
            root.setScrollPos(span > 0 ? (at / span) * root.maxScroll : 0)
        }

        MouseArea {
            id: thumbArea
            anchors.fill: parent
            anchors.margins: -root.gripMargin   // the fat invisible grip
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            drag.target: thumb
            drag.axis: root.horizontal ? Drag.XAxis : Drag.YAxis
            drag.minimumX: 0
            drag.maximumX: root.width - thumb.width
            drag.minimumY: 0
            drag.maximumY: root.height - thumb.height
        }
    }
}
