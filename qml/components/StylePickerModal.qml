import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root
    z: 31000

    property bool open: false

    // ---- Esc = cancel, Enter = Select (qml/settings/Hotkeys.qml) ----------
    // "" is a real answer here (the None card), so accept never asks whether
    // a style was picked — the button it mirrors does not either.
    property int hotkeyScope: 0
    function accept() { root.applied(root.selectedId) }
    onOpenChanged: {
        // Above whatever dialog is already up: a flat z would open this BEHIND an Edit/Add card (ModalCard bumps its own
        // z on every open), which is how the colour card appeared below the Edit Style card.
        if (root.open)
            root.z = ModalStack.nextZ()
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Style"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    // A handful of distinguishable swatch colors, cycled by index — not
    // meant to represent any real per-style color (none exists yet).
    readonly property var swatchColors: [
        "#6c5ce7", "#3b82f6", "#14b8a6", "#f39c12",
        "#e74c3c", "#8b5cf6", "#2ecc71", "#e84393"
    ]

    function previewColor(value, index) {
        if (typeof value === "string" && value.length > 0 && value !== "transparent")
            return value
        const swatch = root.swatchColors[index % root.swatchColors.length]
        return swatch || Theme.windowBg
    }

    property string selectedId: ""

    // Fired when "Select" is clicked, carrying the picked style's id
    // ("" = None) into the caller (OutputsScreen maps it to the output).
    signal applied(string styleId)
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
        width: 620
        height: Math.min(parent.height - 80, content.height + 40)
        radius: 14
        color: Theme.surface
        border.color: Theme.border
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
                    text: qsTr("Choose a presentation theme for this output.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            // Empty state — the roster starts empty by design (see
            // StyleListModel's own header comment).
            Text {
                width: parent.width
                visible: StyleListModel.rowCount() === 0
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("No styles yet — add one from Settings · Styles first.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 14
                wrapMode: Text.Wrap
            }

            Grid {
                width: parent.width
                columns: 3
                columnSpacing: 14
                rowSpacing: 14

                Rectangle {
                    readonly property bool selected: root.selectedId === ""
                    width: (parent.width - 28) / 3
                    height: 140
                    radius: 10
                    color: Theme.windowBg
                    border.width: selected ? 1.5 : 1
                        border.color: selected ? Theme.accent : Theme.borderSubtle
                    Behavior on border.color { ColorAnimation { duration: 100 } }

                    Rectangle {
                        x: 10
                        y: 10
                        width: parent.width - 20
                        height: 80
                        radius: 6
                        color: "#0a0b10"
                        border.color: Theme.borderSubtle
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("—")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
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
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("No style")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
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
                        readonly property bool transparentBg: !styleCard.backgroundColor
                                                             || styleCard.backgroundColor === "transparent"

                        width: (parent.width - 28) / 3
                        height: 140
                        radius: 10
                        color: Theme.windowBg
                        border.width: styleCard.selected ? 1.5 : 1
                        border.color: styleCard.selected ? Theme.accent : Theme.borderSubtle
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        Rectangle {
                            x: 10
                            y: 10
                            width: parent.width - 20
                            height: 80
                            radius: 6
                            color: root.previewColor(styleCard.backgroundColor, styleCard.index)
                            opacity: 0.85

                            Text {
                                anchors.centerIn: parent
                                text: "Aa"
                                color: Theme.controlInk
                                font.family: Theme.fontFamily
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
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: styleCard.res
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
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

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 90
                    height: 34
                    radius: 9
                    color: cancelArea.containsMouse ? Theme.hoverBg : Theme.inset
                    border.color: Theme.toggleOffTrack
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.navLabelMuted
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
                    width: 90
                    height: 34
                    radius: 9
                    color: selectArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Select")
                        color: Theme.controlInk
                        font.family: Theme.fontFamily
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