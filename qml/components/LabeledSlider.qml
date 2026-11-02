import QtQuick

Item {
    id: root

    property string label: ""
    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string suffix: ""
    property int decimals: 0
    property color labelColor: Theme.textSecondary
    property color valueColor: "#aeb6c8"
    property real step: 0
    readonly property bool hasSteppers: root.step > 0 && root.enabled

    function nudge(direction) {
        const raw = root.value + direction * root.step
        const clamped = Math.max(root.minValue, Math.min(root.maxValue, raw))
        // Round off the float drift a fractional step accumulates
        // (0.30000000000000004), so the row's own readout stays clean.
        const snapped = root.step < 1 ? Math.round(clamped * 100) / 100 : clamped
        if (Math.abs(snapped - root.value) < 1e-9)
            return
        root.dragStarted()
        root.moved(snapped)
        root.dragFinished()
    }

    signal moved(real value)
    signal dragStarted()
    signal dragFinished()

    implicitHeight: 34

    readonly property real pct: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: root.labelColor
        font.family: Theme.fontFamily
        font.pixelSize: 13
        font.weight: Font.Medium
    }
    Text {
        anchors.right: root.hasSteppers ? stepperCol.left : parent.right
        anchors.rightMargin: root.hasSteppers ? 8 : 0
        anchors.top: parent.top
        text: (root.decimals > 0 ? root.value.toFixed(root.decimals) : Math.round(root.value)) + root.suffix
        color: root.valueColor
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Font.Medium
    }

    Column {
        id: stepperCol
        visible: root.hasSteppers
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: -4
        spacing: 1

        Repeater {
            model: [1, -1]   // up, then down
            delegate: Rectangle {
                id: stepBtn
                required property int modelData
                readonly property int direction: stepBtn.modelData
                // Dimmed while the whole row is disabled — the two arrows are
                // a row control, not a separate one.
                opacity: root.enabled ? 1 : 0.4
                width: 16
                height: 11
                radius: 3
                color: stepArea.containsMouse ? Theme.hoverBg : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                IconGlyph {
                    anchors.centerIn: parent
                    name: stepBtn.direction > 0 ? "chevronUp" : "chevronDown"
                    color: stepArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                    width: 9
                    height: 9
                }

                MouseArea {
                    id: stepArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: root.enabled
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.nudge(stepBtn.direction)
                }
            }
        }
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
            color: Theme.borderSubtle

            Rectangle {
                width: parent.width * root.pct
                height: parent.height
                radius: 2
                color: Theme.accent
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -5
                onClicked: (mouse) => {
                    const p = Math.max(0, Math.min(1, mouse.x / track.width))
                    root.dragStarted()
                    // Report only — the consumer assigns `value`; see the
                    // controlled-component note in the header comment.
                    root.moved(root.minValue + p * (root.maxValue - root.minValue))
                    root.dragFinished()
                }
            }
        }

        Rectangle {
            id: thumb
            width: 14
            height: 14
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.controlInk
            border.color: Theme.accent
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
                    // Report only — the consumer assigns `value`; see the
                    // controlled-component note in the header comment.
                    root.moved(root.minValue + p * (root.maxValue - root.minValue))
                }
            }

            MouseArea {
                id: thumbArea
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                // Same as LevelDial: the slider lives in scrollable dialogs
                // — the Flickable must not steal a thumb drag mid-gesture.
                preventStealing: true
                onPressed: root.dragStarted()
                onReleased: root.dragFinished()
                drag.target: thumb
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: track.width - thumb.width
            }
        }
    }
}