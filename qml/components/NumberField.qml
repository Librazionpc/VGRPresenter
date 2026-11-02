import QtQuick

Rectangle {
    id: num

    property string label: ""
    property real value: 0
    property real step: 1
    property real minValue: -100
    property real maxValue: 100
    property int decimals: 0
    property bool showSlider: false
    property bool compact: false

    signal moved(real v)

    height: num.compact ? 38 : 52
    radius: num.compact ? 0 : 10
    HoverHandler {
        id: numHover
        enabled: num.enabled
        cursorShape: num.enabled ? Qt.IBeamCursor : Qt.ArrowCursor
    }
    color: num.compact ? "transparent" : (numHover.hovered || numInput.activeFocus ? Theme.hoverBg : Theme.inset)
    border.color: numHover.hovered || numInput.activeFocus ? Theme.border : Theme.borderSubtle
    border.width: num.compact ? 0 : 1
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
    opacity: num.enabled ? 1 : 0.4
    Behavior on opacity { NumberAnimation { duration: 100 } }

    function commit(v) {
        if (!num.enabled)
            return
        const clamped = Math.max(num.minValue, Math.min(num.maxValue, v))
        const factor = Math.pow(10, num.decimals)
        num.moved(Math.round(clamped * factor) / factor)
    }
    function format(v) {
        const n = Number(v)
        return isNaN(n) ? "0" : String(Number(n.toFixed(num.decimals)))
    }
    // A value changed from OUTSIDE (undo, a preset, another selection) rewrites
    // the field — but never under a user who is mid-edit.
    onValueChanged: {
        if (!numInput.activeFocus)
            numInput.text = num.format(num.value)
    }

    Text {
        x: num.compact ? 0 : 12
        y: num.compact ? (parent.height - height) / 2 : 6
        text: num.label
        color: num.compact ? Theme.textPrimary : Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: num.compact ? Theme.textSm : 11
        font.weight: num.compact ? Font.Normal : Font.Medium
        font.letterSpacing: num.compact ? 0 : 0.2
        elide: Text.ElideRight
        width: num.compact ? Math.max(60, parent.width - 270) : parent.width - 24
    }

    // The typed value's own little well in the row layout, so it still reads as editable.
    Rectangle {
        visible: num.compact
        x: numInput.x - 8
        width: numInput.width + 16
        height: 26
        radius: 6
        anchors.verticalCenter: parent.verticalCenter
        color: numHover.hovered || numInput.activeFocus ? Theme.hoverBg : Theme.inset
        border.width: 1
        border.color: numInput.activeFocus ? Theme.accent : Theme.borderSubtle
    }
    TextInput {
        id: numInput
        x: num.compact ? parent.width - 34 - 12 - width : 12
        anchors.bottom: num.compact ? undefined : parent.bottom
        anchors.bottomMargin: 9
        anchors.verticalCenter: num.compact ? parent.verticalCenter : undefined
        horizontalAlignment: num.compact ? TextInput.AlignRight : TextInput.AlignLeft
        width: num.compact ? 38 : (num.showSlider ? 46 : parent.width - 52)
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: num.compact ? Theme.textSm : 16
        selectByMouse: true
        validator: DoubleValidator { bottom: num.minValue; top: num.maxValue; decimals: num.decimals }
        text: num.format(num.value)
        onEditingFinished: {
            const typed = parseFloat(text)
            if (!isNaN(typed) && Math.abs(typed - num.value) > 0.000001)
                num.commit(typed)
            else
                numInput.text = num.format(num.value)
        }
    }
    Item {
        id: sliderWrap
        visible: num.showSlider
        // The tile puts the track between the value and the stepper; the row puts it LEFT of the
        // value, a fixed length, so every row's controls end on the same right edge.
        x: num.compact ? numInput.x - 16 - width : numInput.x + numInput.width + 10
        width: num.compact ? 150 : stepperCol.x - 10 - x
        anchors.verticalCenter: parent.verticalCenter
        height: 14

        readonly property real pct: num.maxValue > num.minValue
            ? Math.max(0, Math.min(1, (num.value - num.minValue) / (num.maxValue - num.minValue)))
            : 0

        Rectangle {
            id: sliderTrack
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.borderSubtle

            Rectangle {
                width: parent.width * sliderWrap.pct
                height: parent.height
                radius: 2
                color: Theme.accent
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -5
                enabled: num.enabled
                onClicked: (mouse) => {
                    const p = Math.max(0, Math.min(1, mouse.x / sliderTrack.width))
                    num.commit(num.minValue + p * (num.maxValue - num.minValue))
                }
            }
        }

        Rectangle {
            id: sliderThumb
            width: 14
            height: 14
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.controlInk
            border.color: Theme.accent
            border.width: 2

            Binding {
                target: sliderThumb
                property: "x"
                value: (sliderTrack.width - sliderThumb.width) * sliderWrap.pct
                when: !sliderThumbArea.drag.active
            }
            onXChanged: {
                if (sliderThumbArea.drag.active) {
                    const range = sliderTrack.width - sliderThumb.width
                    const p = range > 0 ? sliderThumb.x / range : 0
                    num.commit(num.minValue + p * (num.maxValue - num.minValue))
                }
            }

            MouseArea {
                id: sliderThumbArea
                anchors.fill: parent
                anchors.margins: -6
                enabled: num.enabled
                cursorShape: Qt.PointingHandCursor
                preventStealing: true
                drag.target: sliderThumb
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: sliderTrack.width - sliderThumb.width
            }
        }
    }

    // Stepper: up / down, two small pills of their own on the field's right, so
    // they read as buttons and not as a divider between two neighbouring fields.
    Column {
        id: stepperCol
        anchors.right: parent.right
        anchors.rightMargin: num.compact ? 0 : 7
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3
        Repeater {
            model: [1, -1]
            delegate: Rectangle {
                id: stepBtn
                required property int modelData
                width: 18
                height: 15
                radius: 4
                color: stepArea.hovered ? Theme.hoverBg : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                IconGlyph {
                    anchors.centerIn: parent
                    name: stepBtn.modelData > 0 ? "chevronUp" : "chevronDown"
                    color: stepArea.hovered ? Theme.textPrimary : Theme.textMuted
                    width: 10
                    height: 10
                }
                PositionHoverArea {
                    id: stepArea
                    anchors.fill: parent
                    checkAncestors: false
                    onClicked: num.commit(num.value + num.step * stepBtn.modelData)
                }
            }
        }
    }
}