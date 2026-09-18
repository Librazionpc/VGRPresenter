import QtQuick
import VGRPresenterUI

// "Select Style" picker — opens from the Outputs Edit dialog's Style row
// (see OutputsScreen.qml) instead of the old inline chip Flow, matching the
// same scrim + centered card + preview-grid convention as
// CameraSourceModal.qml/MediaSourceModal.qml/ShapeSourceModal.qml.
//
// StyleListModel (src/StyleListModel.{h,cpp}) only carries `name` + `res`
// today — no color/theme/thumbnail data exists yet for a style to actually
// preview. Each card's preview is a simple mock "slide" swatch, colored
// deterministically from the style's own index so different styles at
// least read as visually distinct at a glance — same simplified-preview
// status as this app's other placeholder visuals (ShapeSourceModal's
// glyphs, MediaSourceModal's plain cards) until real per-style theming
// (background/accent color, font) exists to genuinely preview.
Item {
    id: root

    property bool open: false
    property int selectedIndex: -1

    // A handful of distinguishable swatch colors, cycled by index — not
    // meant to represent any real per-style color (none exists yet).
    readonly property var swatchColors: [
        "#6c5ce7", "#3b82f6", "#14b8a6", "#f39c12",
        "#e74c3c", "#8b5cf6", "#2ecc71", "#e84393"
    ]

    // Fired when "Select" is clicked, carrying the picked style's index
    // into StyleListModel.
    signal applied(int index)
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
                        font.family: "Inter"
                        font.pixelSize: 16
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
                            font.pixelSize: 12
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
                    font.family: "Inter"
                    font.pixelSize: 12
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
                font.family: "Inter"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Grid {
                width: parent.width
                columns: 3
                columnSpacing: 14
                rowSpacing: 14

                Repeater {
                    model: StyleListModel

                    delegate: Rectangle {
                        id: styleCard
                        required property int index
                        required property string name
                        required property string res
                        readonly property bool selected: root.selectedIndex === styleCard.index

                        width: (parent.width - 28) / 3
                        height: 140
                        radius: 10
                        color: "#0d0f16"
                        border.width: styleCard.selected ? 1.5 : 1
                        border.color: styleCard.selected ? "#6c5ce7" : "#262a38"
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        // Mock preview "slide" — see header comment.
                        Rectangle {
                            x: 10
                            y: 10
                            width: parent.width - 20
                            height: 80
                            radius: 6
                            color: root.swatchColors[styleCard.index % root.swatchColors.length]
                            opacity: 0.85

                            Text {
                                anchors.centerIn: parent
                                text: "Aa"
                                color: "#ffffff"
                                font.family: "Inter"
                                font.pixelSize: 22
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
                                font.family: "Inter"
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: styleCard.res
                                color: "#5c6475"
                                font.family: "Inter"
                                font.pixelSize: 10
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedIndex = styleCard.index
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
                        font.family: "Inter"
                        font.pixelSize: 12
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
                    enabled: root.selectedIndex >= 0
                    opacity: root.selectedIndex >= 0 ? 1 : 0.4
                    color: selectArea.containsMouse ? "#5a4cd6" : "#6c5ce7"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Select")
                        color: "#ffffff"
                        font.family: "Inter"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: selectArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied(root.selectedIndex)
                    }
                }
            }
        }
    }
}
