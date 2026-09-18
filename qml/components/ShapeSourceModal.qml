import QtQuick

// "Add Shape" picker — pops up when "Shape" is picked from the Edit
// screen's "+" Add Content menu, matching
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Add_Shape.qml's shape_modal_*/
// shape_cell*_*/shape_btn_* elements. Same convention as
// CameraSourceModal.qml/MediaSourceModal.qml: scrim + centered card, `open`
// to show/hide, `applied`/`cancelled` signals, no application state of its
// own beyond the picker UI.
//
// Unlike Camera/Media/Timer/Clock, a "shape" canvas item's fill/border/
// corner-radius come straight from its own CanvasItemStyle (see
// EditScreen.qml's primarySelectedSupportsFill and shapeContent) — this
// picker only decides which shape TYPE it is, not its color.
Item {
    id: root

    property bool open: false

    property var shapeTypes: [
        { key: "rectangle", label: qsTr("Rectangle") },
        { key: "circle", label: qsTr("Circle") },
        { key: "line", label: qsTr("Line") },
        { key: "triangle", label: qsTr("Triangle") },
        { key: "arrow", label: qsTr("Arrow") },
        { key: "star", label: qsTr("Star") },
        { key: "rounded", label: qsTr("Rounded") },
        { key: "hexagon", label: qsTr("Hexagon") }
    ]
    // Glyphs standing in for the ground truth's per-cell icon artwork —
    // same simplified-icon approach the ground truth itself already uses
    // for triangle/arrow/star/hexagon (plain Text glyphs, not custom paths).
    readonly property var glyphs: ({
        rectangle: "▭", circle: "●", line: "—", triangle: "▲",
        arrow: "→", star: "★", rounded: "▢", hexagon: "⬡"
    })

    property string selectedType: "rectangle"

    // { shapeType } — the consumer decides what a "shape" canvas item does
    // with it (see EditScreen.qml's shapeContent).
    signal applied(var config)
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
        width: 520
        height: content.height + 40
        radius: 14
        color: "#13151c"
        border.color: "#232530"
        border.width: 1

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
                        text: qsTr("Add Shape")
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
                    text: qsTr("Choose a shape to add to this slide")
                    color: "#8a94a6"
                    font.family: "Inter"
                    font.pixelSize: 12
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            // Shape cells — four columns, matching the ground truth's
            // 106-wide cells with 12px gutter in a 472-wide content area.
            Grid {
                width: parent.width
                columns: 4
                columnSpacing: 12
                rowSpacing: 12

                Repeater {
                    model: root.shapeTypes
                    delegate: Rectangle {
                        id: shapeCell
                        required property var modelData
                        readonly property bool selected: root.selectedType === shapeCell.modelData.key

                        width: (parent.width - 36) / 4
                        height: 90
                        radius: 8
                        color: shapeCell.selected ? "#206c5ce7" : (cellArea.containsMouse ? "#20222c" : "#1a1c26")
                        border.color: shapeCell.selected ? "#6c5ce7" : "#262a38"
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: -10
                            text: root.glyphs[shapeCell.modelData.key]
                            color: shapeCell.selected ? "#9b8ff5" : "#525a72"
                            font.pixelSize: 26
                        }

                        Text {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: shapeCell.modelData.label
                            color: shapeCell.selected ? "#eef1f8" : "#c8cdd9"
                            font.family: "Inter"
                            font.pixelSize: 11
                        }

                        MouseArea {
                            id: cellArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedType = shapeCell.modelData.key
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
                    height: 32
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
                    width: 100
                    height: 32
                    radius: 9
                    color: addArea.containsMouse ? "#5a4cd6" : "#6c5ce7"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add Shape")
                        color: "#ffffff"
                        font.family: "Inter"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied({ shapeType: root.selectedType })
                    }
                }
            }
        }
    }
}
