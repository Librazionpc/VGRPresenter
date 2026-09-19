import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Smart Config — rebuilt from the reference screenshot
// (1BBTIwaya's VGRPresenter_Settings_Smart_Config.qml is a PNG placeholder
// export — a bare modal.png image like SizeStyleCard's source was — so the
// visible design in the screenshot is ground truth): a Configuration mode
// selector (Strict / Smart / Manual), a Hardware detected card whose rows
// show a green check + a value, and Resource budgets meters.
//
// Same structure as GeneralScreen: an Item root (scrollbar stays fixed at
// the edge while content scrolls), Flickable + Column inside, shared
// AppScrollBar, Theme tokens throughout. Content is short enough that it
// doesn't scroll at the dialog's default size, but the Flickable stays so
// it degrades gracefully at small window sizes.
Item {
    id: root

    // "strict" | "smart" | "manual"
    property string configMode: "smart"
    readonly property var modes: [
        { key: "strict", label: qsTr("Strict"), sub: qsTr("Only initialize what you enable") },
        { key: "smart", label: qsTr("Smart"), sub: qsTr("Auto-tune for this hardware") },
        { key: "manual", label: qsTr("Manual"), sub: qsTr("You configure every option") }
    ]

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space5
            width: flick.width - Theme.space6
            spacing: 16

            // ---- Page header ----
            Column {
                spacing: 2

                Text {
                    text: qsTr("Smart Config")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Let VGR analyze your hardware and optimize the production pipeline.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Configuration mode card ----
            Rectangle {
                width: parent.width
                height: modeCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: modeCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    Text {
                        text: qsTr("Configuration mode")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Row {
                        width: parent.width
                        spacing: 16

                        Repeater {
                            model: root.modes
                            delegate: Rectangle {
                                id: modeCell
                                required property var modelData
                                readonly property bool active: root.configMode === modeCell.modelData.key

                                width: (parent.width - 32) / 3
                                height: 62
                                radius: Theme.radiusMd
                                color: modeCell.active ? "#266C5CE7" : (modeArea.containsMouse ? Theme.chip : Theme.inset)
                                border.width: 1
                                border.color: modeCell.active ? Theme.accent : Theme.border
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Column {
                                    x: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 3

                                    Text {
                                        text: modeCell.modelData.label
                                        color: modeCell.active ? Theme.accentLight : Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        font.weight: modeCell.active ? Font.DemiBold : Font.Medium
                                    }
                                    Text {
                                        width: modeCell.width - 28
                                        text: modeCell.modelData.sub
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    id: modeArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.configMode = modeCell.modelData.key
                                }
                            }
                        }
                    }
                }
            }

            // ---- Hardware detected card ----
            Rectangle {
                width: parent.width
                height: hwCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: hwCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Hardware detected")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }

                    Repeater {
                        model: [
                            { label: qsTr("GPU"), value: qsTr("NVIDIA RTX 4060 · 8 GB") },
                            { label: qsTr("Encoder"), value: qsTr("NVENC available") },
                            { label: qsTr("Audio devices"), value: qsTr("4 outputs, 2 inputs") },
                            { label: qsTr("Displays"), value: qsTr("3 connected") }
                        ]
                        delegate: Column {
                            id: hwRow
                            required property var modelData
                            width: hwCol.width

                            Item {
                                width: hwRow.width
                                height: 32

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: hwRow.modelData.label
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }

                                Row {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 8

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "✓"
                                        color: Theme.success
                                        font.pixelSize: 11
                                        font.weight: Font.Bold
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: hwRow.modelData.value
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                }
                            }
                            Rectangle { width: hwRow.width; height: 1; color: Theme.border; visible: hwRow.index < 3 }
                        }
                    }
                }
            }

            // ---- Resource budgets card ----
            Rectangle {
                width: parent.width
                height: budgetsCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: budgetsCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 10

                    Text {
                        text: qsTr("Resource budgets")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }

                    LabeledMeter { label: qsTr("GPU"); pct: 80; width: parent.width }
                    LabeledMeter { label: qsTr("CPU"); pct: 60; width: parent.width }
                }
            }
        }
    }

    // Shared app scrollbar at the fixed right edge (sibling of the
    // Flickable — see GeneralScreen's note).
    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }
}
