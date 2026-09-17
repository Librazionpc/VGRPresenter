import QtQuick

// Reusable "label — value — draggable track" slider row: a label on the
// left, the current value on the right, and a horizontal track/thumb below.
// Used by SizeStyleCard.qml's Padding/Opacity/Radius/Width rows; the same
// Binding{when: !dragging} handoff pattern as AppScrollBar.qml's thumb (and
// BackgroundColorModal.qml's Opacity slider, which predates this and could
// be swapped to use it) keeps the declarative position binding and the
// imperative drag from fighting each other.
//
// Literal colors, not Theme.* — matches this file family's convention
// (see EditScreen.qml/DropdownPanel.qml headers) even though this specific
// component isn't itself affected by the AOT singleton-resolution issue.
Item {
    id: root

    property string label: ""
    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string suffix: ""
    property int decimals: 0

    signal moved(real value)

    height: 34

    readonly property real pct: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 12
    }
    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        text: (root.decimals > 0 ? root.value.toFixed(root.decimals) : Math.round(root.value)) + root.suffix
        color: "#aeb6c8"
        font.family: "Inter"
        font.pixelSize: 12
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 14

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: "#2a3140"

            Rectangle {
                width: parent.width * root.pct
                height: parent.height
                radius: 2
                color: "#6c5ce7"
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -5
                onClicked: (mouse) => {
                    const p = Math.max(0, Math.min(1, mouse.x / track.width))
                    root.value = root.minValue + p * (root.maxValue - root.minValue)
                    root.moved(root.value)
                }
            }
        }

        Rectangle {
            id: thumb
            width: 14
            height: 14
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            color: "#ffffff"
            border.color: "#6c5ce7"
            border.width: 2

            Binding {
                target: thumb
                property: "x"
                value: (track.width - thumb.width) * root.pct
                when: !thumbArea.drag.active
            }
            onXChanged: {
                if (thumbArea.drag.active) {
                    const range = track.width - thumb.width
                    const p = range > 0 ? thumb.x / range : 0
                    root.value = root.minValue + p * (root.maxValue - root.minValue)
                    root.moved(root.value)
                }
            }

            MouseArea {
                id: thumbArea
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                drag.target: thumb
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: track.width - thumb.width
            }
        }
    }
}
