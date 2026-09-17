import QtQuick

// A thin vertical scrollbar for a Flickable/ListView. QtQuick.Controls'
// ScrollBar doesn't match this app's track/thumb visual language, so this
// is a small from-scratch replacement — hand it a `flickable` and it does
// the rest: computes its own thumb length/position from contentHeight,
// supports drag-to-scroll, and clicking the track pages up/down. Reusable
// anywhere a scrollable list needs one instead of each screen hand-rolling
// its own track/thumb math.
Item {
    id: root

    property Flickable flickable
    property int trackWidth: 4
    property color trackColor: "#161823"
    property color thumbColor: "#3a3f4d"
    property int minThumbLength: 24

    width: trackWidth
    visible: flickable && flickable.contentHeight > flickable.height

    readonly property real maxScroll: flickable ? Math.max(flickable.contentHeight - flickable.height, 1) : 1
    readonly property real thumbLength: flickable
        ? Math.max(minThumbLength, root.height * flickable.height / Math.max(flickable.contentHeight, 1))
        : 0

    Rectangle {
        anchors.fill: parent
        radius: root.trackWidth / 2
        color: root.trackColor
    }

    // Click above/below the thumb to page in that direction.
    MouseArea {
        anchors.fill: parent
        onClicked: (mouse) => {
            if (!root.flickable) return
            if (mouse.y < thumb.y) {
                root.flickable.contentY = Math.max(0, root.flickable.contentY - root.flickable.height)
            } else if (mouse.y > thumb.y + thumb.height) {
                root.flickable.contentY = Math.min(root.maxScroll, root.flickable.contentY + root.flickable.height)
            }
        }
    }

    Rectangle {
        id: thumb
        width: root.trackWidth
        radius: root.trackWidth / 2
        color: thumbArea.pressed ? Qt.lighter(root.thumbColor, 1.3) : root.thumbColor
        height: root.thumbLength

        // Normal state: thumb position follows the Flickable (including
        // wheel/programmatic scrolling). Disabled mid-drag so the drag
        // itself can drive `y` without fighting this binding.
        Binding {
            target: thumb
            property: "y"
            value: (root.height - thumb.height) * (root.flickable ? root.flickable.contentY / root.maxScroll : 0)
            when: !thumbArea.drag.active
        }

        // Drag state: thumb.y is being set imperatively by the drag: push
        // it back into the Flickable's contentY.
        onYChanged: {
            if (thumbArea.drag.active && root.flickable) {
                const range = root.height - thumb.height
                root.flickable.contentY = range > 0 ? (thumb.y / range) * root.maxScroll : 0
            }
        }

        MouseArea {
            id: thumbArea
            anchors.fill: parent
            anchors.margins: -4
            cursorShape: Qt.PointingHandCursor
            drag.target: thumb
            drag.axis: Drag.YAxis
            drag.minimumY: 0
            drag.maximumY: root.height - thumb.height
        }
    }
}
