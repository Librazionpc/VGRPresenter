import QtQuick
import VGRPresenterUI
import "../components"

Flickable {
    id: root
    contentWidth: width
    contentHeight: layout.height + Theme.space6 * 2
    clip: true

    property var accentColors: [Theme.accent, Theme.info, Theme.success, Theme.warning, Theme.danger]
    property int selectedAccent: 0

    Column {
        id: layout
        x: 20
        y: 24
        width: root.width - Theme.space5 * 2
        spacing: Theme.space6

        Column {
            spacing: Theme.space1
            Text {
                text: "General"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXl
                font.weight: Font.DemiBold
            }
            Text {
                text: "Application behavior, appearance, and startup."
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
        }

        // Appearance card
        Rectangle {
            width: parent.width
            height: appearanceCol.height + Theme.space5 * 2
            radius: Theme.radiusLg
            color: Theme.card

            Column {
                id: appearanceCol
                x: 20
                y: 20
                width: parent.width - Theme.space5 * 2
                spacing: Theme.space4

                Text {
                    text: "Accent color"
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textMd
                    font.weight: Font.Medium
                }

                Row {
                    spacing: Theme.space3

                    Repeater {
                        model: root.accentColors
                        delegate: Rectangle {
                            required property string modelData
                            required property int index
                            width: 34
                            height: 34
                            radius: Theme.radiusMd
                            color: modelData
                            border.width: root.selectedAccent === index ? 2 : 0
                            border.color: Theme.accent

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectedAccent = index
                            }
                        }
                    }
                }
            }
        }

        // Behavior card
        Rectangle {
            width: parent.width
            height: behaviorCol.height + Theme.space5 * 2
            radius: Theme.radiusLg
            color: Theme.card

            Column {
                id: behaviorCol
                x: 20
                y: 20
                width: parent.width - Theme.space5 * 2
                spacing: Theme.space4

                Text {
                    text: "Startup"
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textMd
                    font.weight: Font.Medium
                }

                Repeater {
                    model: [
                        { label: "Launch on system startup", checked: true },
                        { label: "Check for updates automatically", checked: true },
                        { label: "Hardware-accelerated rendering", checked: false }
                    ]
                    delegate: Row {
                        required property var modelData
                        width: behaviorCol.width
                        height: 24

                        Text {
                            width: parent.width - 40
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }

                        ToggleSwitch {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: modelData.checked
                        }
                    }
                }
            }
        }
    }
}
