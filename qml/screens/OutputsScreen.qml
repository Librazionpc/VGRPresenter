import QtQuick
import VGRPresenterUI
import "../components"

// Settings · Outputs — manage where the show is displayed. Rebuilt as clean
// Theme-token QML because the 1BBTIwaya ground truth for the settings panels
// is flat PNG screenshots (VGRPresenter_Settings_Outputs.qml embeds
// modal_1.png), not element-level QML like the main screen. Follows the
// GeneralScreen pattern: Flickable root + Column of cards inside ModalShell.
Flickable {
    id: root
    contentWidth: width
    contentHeight: layout.height + Theme.space6 * 2
    clip: true

    // Output roster comes from the OutputListModel singleton
    // (src/OutputListModel.{h,cpp}) — the SAME model the Edit screen's
    // output-monitor grid renders, so adding an output here shows up there
    // too instead of two hand-typed arrays drifting apart.

    Column {
        id: layout
        x: 20
        y: 24
        width: root.width - Theme.space5 * 2
        spacing: Theme.space6

        Column {
            spacing: Theme.space1
            Text {
                text: "Outputs"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXl
                font.weight: Font.DemiBold
            }
            Text {
                text: "Settings · Outputs — styles hub. Manage screens, streams, and overlay outputs."
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
        }

        // Output cards
        Repeater {
            model: OutputListModel

            delegate: Rectangle {
                id: card
                required property int index
                required property string name
                required property string badge
                required property string kind
                required property string res
                required property bool active

                width: layout.width
                height: cardCol.height + Theme.space5 * 2
                radius: Theme.radiusLg
                color: Theme.card
                border.width: card.active ? 1 : 0
                border.color: Theme.danger

                Column {
                    id: cardCol
                    x: 20
                    y: 20
                    width: parent.width - Theme.space5 * 2
                    spacing: Theme.space3

                    Row {
                        spacing: Theme.space3

                        Pill {
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.badge
                            tint: false
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.name
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textMd
                            font.weight: Font.Medium
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "·  " + card.kind
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                    }

                    Row {
                        spacing: Theme.space2

                        Text {
                            text: card.res
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            text: "·  Template:"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                        Pill {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Worship"
                            tint: false
                        }
                    }
                }

                // Status + action, anchored top-right of the card.
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space5
                    anchors.top: parent.top
                    anchors.topMargin: Theme.space5
                    spacing: Theme.space3

                    Pill {
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.active ? "LIVE" : "Inactive"
                        baseColor: card.active ? Theme.success : Theme.chip
                        lightColor: Theme.successLight
                        tint: !card.active

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: OutputListModel.setActive(card.index)
                        }
                    }

                    AppButton {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Edit"
                        variant: "ghost"
                    }
                }
            }
        }

        // Add output
        Rectangle {
            width: layout.width
            height: addRow.height + Theme.space5 * 2
            radius: Theme.radiusLg
            color: Theme.card
            border.color: Theme.borderSubtle
            border.width: 1

            Row {
                id: addRow
                x: 20
                y: 20
                spacing: Theme.space4

                AppButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "+ Add output"
                    onClicked: OutputListModel.addOutput()
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Send the program feed to another display or streaming destination."
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
            }
        }
    }
}
