import QtQuick
import VGRPresenterUI

// "Select Style" picker — opens from the Outputs Edit dialog's Style row
// (see OutputsScreen.qml) instead of the old inline chip Flow, matching the
// same scrim + centered card + preview-grid convention as
// CameraSourceModal.qml/MediaSourceModal.qml/ShapeSourceModal.qml.
//
// Cards preview the style's ACTUAL background colour now that the model
// carries one ("transparent" falls back to the deterministic swatch cycle).
// A "None" card leads the grid: an output with no style renders unstyled
// (the engine's default composition) — the same "style: optional" contract
// FreeShow's picker has.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false
    // Selection by the style's STABLE id ("" = the None card). Ids survive
    // roster edits between the dialog opening and the pick landing; an id
    // that no longer exists reads as None.

    // A handful of distinguishable swatch colors, cycled by index — not
    // meant to represent any real per-style color (none exists yet).
    readonly property var swatchColors: [
        "#6c5ce7", "#3b82f6", "#14b8a6", "#f39c12",
        "#e74c3c", "#8b5cf6", "#2ecc71", "#e84393"
    ]

    // Selection by the style's STABLE id ("" = the None card). Ids survive
    // roster edits between the dialog opening and the pick landing; an id
    // that no longer exists reads as None.
    property string selectedId: ""

    // Fired when "Select" is clicked, carrying the picked style's id
    // ("" = None) into the caller (OutputsScreen maps it to the output).
    signal applied(string styleId)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 620
        height: Math.min(parent.height - 80, content.height + 40)
        radius: 14
        color: "#13151c"
        border.color: "#232530"
        border.width: 1
        clip: true

        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 16

            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Select Style")
                        color: "#f1f3f8"
                        font.family: "Segoe UI"
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        radius: 7
                        color: closeArea.containsMouse ? "#20222c" : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: "#8a94a6"
                            font.pixelSize: 14
                        }

                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Choose a presentation theme for this output.")
                    color: "#8a94a6"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            // Empty state — the roster starts empty by design (see
            // StyleListModel's own header comment).
            Text {
                width: parent.width
                visible: StyleListModel.rowCount() === 0
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("No styles yet — add one from Settings · Styles first.")
                color: "#5c6475"
                font.family: "Segoe UI"
                font.pixelSize: 14
                wrapMode: Text.Wrap
            }

            Grid {
                width: parent.width
                columns: 3
                columnSpacing: 14
                rowSpacing: 14

                // "None" — no style. selectedId "" both means "None is picked
                // here" and is what applied() carries for it, matching
                // OutputListModel's "" styleId through OutputsScreen's mapping.
                Rectangle {
                    readonly property bool selected: root.selectedId === ""
                    width: (parent.width - 28) / 3
                    height: 140
                    radius: 10
                    color: "#0d0f16"
                    border.width: selected ? 1.5 : 1
                        border.color: selected ? Theme.accent : "#262a38"
                    Behavior on border.color { ColorAnimation { duration: 100 } }

                    Rectangle {
                        x: 10
                        y: 10
                        width: parent.width - 20
                        height: 80
                        radius: 6
                        color: "#0a0b10"
                        border.color: "#262a38"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("—")
                            color: "#5c6475"
                            font.family: "Segoe UI"
                            font.pixelSize: 25
                            font.weight: Font.DemiBold
                        }
                    }

                    Column {
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 10
                        width: parent.width
                        spacing: 2

                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("None")
                            color: "#eef1f8"
                            font.family: "Segoe UI"
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("No style")
                            color: "#5c6475"
                            font.family: "Segoe UI"
                            font.pixelSize: 12
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selectedId = ""
                    }
                }

                Repeater {
                    model: StyleListModel

                    delegate: Rectangle {
                        id: styleCard
                        required property int index
                        required property string styleId
                        required property string name
                        required property string res
                        required property string backgroundColor
                        readonly property bool selected: root.selectedId === styleCard.styleId
                        readonly property bool transparentBg: styleCard.backgroundColor === "transparent"

                        width: (parent.width - 28) / 3
                        height: 140
                        radius: 10
                        color: "#0d0f16"
                        border.width: styleCard.selected ? 1.5 : 1
                        border.color: styleCard.selected ? Theme.accent : "#262a38"
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        // Preview "slide" — the style's real background when it
                        // has one; the deterministic swatch cycle only covers
                        // "transparent" (no colour to show).
                        Rectangle {
                            x: 10
                            y: 10
                            width: parent.width - 20
                            height: 80
                            radius: 6
                            color: styleCard.transparentBg
                                   ? root.swatchColors[styleCard.index % root.swatchColors.length]
                                   : styleCard.backgroundColor
                            opacity: 0.85

                            Text {
                                anchors.centerIn: parent
                                text: "Aa"
                                color: "#ffffff"
                                font.family: "Segoe UI"
                                font.pixelSize: 25
                                font.weight: Font.DemiBold
                            }
                        }

                        Column {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            width: parent.width
                            spacing: 2

                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: styleCard.name
                                color: "#eef1f8"
                                font.family: "Segoe UI"
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: styleCard.res
                                color: "#5c6475"
                                font.family: "Segoe UI"
                                font.pixelSize: 12
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedId = styleCard.styleId
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 90
                    height: 34
                    radius: 9
                    color: cancelArea.containsMouse ? "#20222c" : "#1a1c26"
                    border.color: "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#c8cdd9"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelled()
                    }
                }

                Rectangle {
                    width: 90
                    height: 34
                    radius: 9
                    color: selectArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Select")
                        color: "#ffffff"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: selectArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        // -1 = the None card — a valid pick, not "nothing picked".
                        onClicked: root.applied(root.selectedId)
                    }
                }
            }
        }
    }
}
