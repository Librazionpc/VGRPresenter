import QtQuick
import VGRPresenterUI

Item {
    id: root

    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string label: ""

    signal moved(real value)
    // Same gesture-bracketing pair as LabeledSlider — a whole drag reports
    // as one gesture, not one event per pixel.
    signal dragStarted()
    signal dragFinished()

    implicitWidth: 120
    implicitHeight: knob.height + captionCol.implicitHeight + Theme.space2

    readonly property real pct: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0
    // 0% points -135° (lower-left), 100% points +135° (lower-right).
    readonly property real pointerRotation: -135 + 270 * pct

    Item {
        id: knob
        width: 110
        height: 110
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top

        Rectangle {
            id: body
            anchors.fill: parent
            radius: width / 2
            color: Theme.inset
            border.width: 1
            border.color: dragArea.pressed ? Theme.accent : Theme.border
            Behavior on border.color { ColorAnimation { duration: 100 } }
        }

        Rectangle {
            width: 3
            height: knob.width / 2 - 14
            radius: 1.5
            color: Theme.accent
            x: knob.width / 2 - width / 2
            y: knob.height / 2 - height
            transformOrigin: Item.Bottom
            rotation: root.pointerRotation
            Behavior on rotation { NumberAnimation { duration: 40 } }
        }

        // Center cap over the pivot.
        Rectangle {
            anchors.centerIn: parent
            width: 16
            height: 16
            radius: 8
            color: Theme.surface
            border.width: 1
            border.color: Theme.border
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            preventStealing: true

            onPressed: (mouse) => {
                root.dragStarted()
                setFromPosition(mouse.x, mouse.y)
            }
            onPositionChanged: (mouse) => setFromPosition(mouse.x, mouse.y)
            onReleased: root.dragFinished()
            onCanceled: root.dragFinished()

            function setFromPosition(px, py) {
                const dx = px - knob.width / 2
                const dy = py - knob.height / 2
                const deg = Math.atan2(dx, -dy) * 180 / Math.PI
                let stopAngle = deg
                if (deg < -135)
                    stopAngle = -135   // dead zone: hold the lower stop (0%)
                else if (deg > 135)
                    stopAngle = 135    // dead zone: hold the upper stop (100%)
                root.moved(root.minValue + (stopAngle + 135) / 270 * (root.maxValue - root.minValue))
            }
        }
    }

    Column {
        id: captionCol
        anchors.top: knob.bottom
        anchors.topMargin: Theme.space2
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 1

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Math.round(root.value) + "%"
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }

        Text {
            visible: root.label !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.label
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}