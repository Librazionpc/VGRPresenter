import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // ---- Esc = cancel, Enter = Add (qml/settings/Hotkeys.qml) -------------
    property int hotkeyScope: 0
    function accept() { root.applied({ shapeType: root.selectedType }) }
    onOpenChanged: {
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Shape"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

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

    parent: Overlay.overlay

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        // The app's one scale, on the card (see AppScale.qml) — never on the root,
        // whose scrim IS the dialog's black box and has to stay window-sized.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        anchors.centerIn: parent
        width: 520
        height: content.height + 40
        radius: 14
        color: Theme.surface
        border.color: Theme.border
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
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    ModalCloseButton {
                        id: closeBtn
                        onClicked: root.cancelled()
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Choose a shape to add to this slide")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

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
                        color: shapeCell.selected ? Qt.alpha(Theme.accent, 0.125) : (cellArea.containsMouse ? Theme.hoverBg : Theme.inset)
                        border.color: shapeCell.selected ? Theme.accent : Theme.borderSubtle
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        Text {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: -10
                            text: root.glyphs[shapeCell.modelData.key]
                            color: shapeCell.selected ? Theme.accentLight : Theme.textMuted
                            font.pixelSize: 30
                        }

                        Text {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: shapeCell.modelData.label
                            // Selected cell's label is the brightest; an unselected one takes
                            // the second-level tone — the same pairing SelectableChip uses.
                            color: shapeCell.selected ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
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

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 90
                    height: 32
                    radius: 9
                    color: cancelArea.containsMouse ? Theme.hoverBg : Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
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
                    objectName: "selfTestAddShape" // UI self-test click target
                    width: 100
                    height: 32
                    radius: 9
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add Shape")
                        color: Theme.controlInk
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
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