import QtQuick

// The Edit screen's "Size & Style" collapsible panel: Padding/Opacity
// sliders, then a Border section (color row, Width, Corner Radius, and a
// Line/Dotted/Dashed style selector — radius lives here, grouped with the
// border it rounds, not up with Padding/Opacity). 1BBTIwaya/
// VGRPresenter_Settings_Outputs_Edit.qml ships
// this only as a flat PNG (style_dialog.png/modal_9.png), so it's rebuilt
// here as real QML from the reference screenshot, model-driven where it
// matters (borderStyle options) so it stays a self-contained, reusable card
// rather than living inline in EditScreen.qml.
//
// Literal colors, not Theme.* — same house convention as DropdownPanel.qml/
// LabeledSlider.qml at this nesting depth.
Column {
    id: root

    property bool expanded: true
    property real padding: 24
    property real styleOpacity: 100
    property real radius: 12
    property real borderWidth: 2
    property string borderStyle: "line" // "line" | "dotted" | "dashed"
    property var borderColor: ({ kind: "color", color: "#ffffff" })
    // Off by default — FreeShow (and most editors) don't put a border on a
    // text item unless you deliberately turn one on; a checkbox is the
    // explicit "yes, style this one" switch rather than every item always
    // carrying whatever width/style happen to be dialed in here.
    property bool borderEnabled: false

    // Fired when the Border row's "Change" chip is clicked — this card owns
    // no color-picker UI itself; the consumer (EditScreen.qml) reuses its
    // existing BackgroundColorModal instance for both Background and Border.
    signal changeBorderRequested()

    spacing: 14

    Rectangle {
        width: parent.width
        height: 44
        radius: 8
        color: "#161823"

        Row {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Text {
                text: "|||"
                color: "#8b5cf6"
                font.pixelSize: 12
                font.weight: Font.Bold
            }
            Text {
                text: qsTr("Size & Style")
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: Font.Medium
            }
        }

        Text {
            x: parent.width - 28
            anchors.verticalCenter: parent.verticalCenter
            text: root.expanded ? "⌃" : "⌄"
            color: "#6b7280"
            font.pixelSize: 12
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }
    }

    Column {
        width: parent.width
        spacing: 18
        visible: root.expanded
        height: visible ? implicitHeight : 0

        LabeledSlider {
            width: parent.width
            label: qsTr("Padding")
            value: root.padding
            minValue: 0
            maxValue: 64
            onMoved: (v) => root.padding = v
        }
        LabeledSlider {
            width: parent.width
            label: qsTr("Opacity")
            value: root.styleOpacity
            minValue: 0
            maxValue: 100
            suffix: "%"
            onMoved: (v) => root.styleOpacity = v
        }

        // Border color row — same card language as EditScreen.qml's
        // Background row (swatch + "Change" chip at the same x offsets,
        // assuming the same 384px row width).
        Rectangle {
            width: parent.width
            height: 46
            radius: 8
            color: "#161823"

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Border")
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
            }

            // Enable checkbox — the swatch/Change chip below stay visible
            // (so you can still see/prepare a color) but dimmed and inert
            // until this is checked.
            Rectangle {
                id: borderCheckbox
                x: 216
                anchors.verticalCenter: parent.verticalCenter
                height: 18
                width: 18
                radius: 4
                color: root.borderEnabled ? "#6c5ce7" : "#1a1c26"
                border.color: root.borderEnabled ? "#6c5ce7" : "#3a4050"
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    visible: root.borderEnabled
                    text: "✓"
                    color: "#ffffff"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.borderEnabled = !root.borderEnabled
                }
            }

            Rectangle {
                x: 256
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: "#3a4a7a"
                border.width: 1
                radius: 5
                Behavior on opacity { NumberAnimation { duration: 100 } }
                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: root.borderColor.kind === "gradient" ? root.borderColor.from : root.borderColor.color
                    }
                    GradientStop {
                        position: 1
                        color: root.borderColor.kind === "gradient" ? root.borderColor.to : root.borderColor.color
                    }
                }
            }

            Rectangle {
                x: 298
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: "#2a3140"
                border.width: 1
                color: borderChangeArea.containsMouse ? "#20242f" : "#1a1c26"
                radius: 12
                Behavior on color { ColorAnimation { duration: 100 } }
                Behavior on opacity { NumberAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Change")
                    color: "#aeb6c8"
                    font.family: "Inter"
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: borderChangeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.changeBorderRequested()
                }
            }

            Text {
                x: 368
                anchors.verticalCenter: parent.verticalCenter
                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 14
                text: "›"
            }
        }

        LabeledSlider {
            width: parent.width
            opacity: root.borderEnabled ? 1 : 0.4
            label: qsTr("Width")
            value: root.borderWidth
            minValue: 0
            maxValue: 12
            suffix: "px"
            onMoved: (v) => root.borderWidth = v
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }
        LabeledSlider {
            width: parent.width
            opacity: root.borderEnabled ? 1 : 0.4
            label: qsTr("Corner Radius")
            value: root.radius
            minValue: 0
            maxValue: 48
            suffix: "px"
            onMoved: (v) => root.radius = v
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }

        // Line / Dotted / Dashed segmented selector.
        Row {
            width: parent.width
            spacing: 8
            opacity: root.borderEnabled ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            Repeater {
                model: [
                    { key: "line", label: qsTr("Line") },
                    { key: "dotted", label: qsTr("Dotted") },
                    { key: "dashed", label: qsTr("Dashed") }
                ]
                delegate: Rectangle {
                    id: styleBtn
                    required property var modelData
                    readonly property bool active: root.borderStyle === styleBtn.modelData.key

                    width: (parent.width - 16) / 3
                    height: 32
                    radius: 8
                    color: styleBtn.active ? "#6c5ce7" : (styleArea.containsMouse ? "#20222c" : "#1a1c26")
                    border.color: styleBtn.active ? "#6c5ce7" : "#2a3140"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: styleBtn.modelData.label
                        color: styleBtn.active ? "#ffffff" : "#aeb6c8"
                        font.family: "Inter"
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: styleArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.borderStyle = styleBtn.modelData.key
                    }
                }
            }
        }
    }
}
