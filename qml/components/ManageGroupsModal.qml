import QtQuick
import VGRPresenterUI

// Settings · Appearance & Actions · "Manage groups" (FreeShow's
// ManageGroups.svelte): the global registry of named song-section tags a
// slide can carry (verse, chorus...), each with its own colour and an
// optional single-letter LIVE shortcut — press it during GO LIVE to jump to
// the next slide carrying that group (see LiveOutputService.jumpToGroup /
// GroupListModel.shortcutMap).
//
// Built on ModalCard — the SAME shared shell every other settings dialog in
// this app uses (Screens' Add/Edit, StylesScreen's own dialogs): the scrim,
// scaling, Esc/Enter via Hotkeys' scope stack, header and scrollbar all come
// from there, so this dialog can't drift from (or quietly break, the way a
// hand-rolled copy of that same wiring did) the rest of the app. No footer:
// every row writes straight through to GroupListModel the moment it
// changes, the same "no Save button, it applies as you go" contract every
// other roster editor here follows (StylesScreen, OutputsScreen) — Esc or
// the header's own close-X is the only way out, and both just mean "done".
ModalCard {
    id: root
    objectName: "manageGroupsModal"
    title: qsTr("Manage groups")
    subtitle: qsTr("Every slide can carry one of these as its group — the colour marks it in the slide grid, and the shortcut jumps to the next slide carrying it while live.")
    cardWidth: 620
    showFooter: false
    onCancelled: root.close()

    // Which row's colour swatch is being edited — latched the same way
    // EditScreen's openBackgroundPicker latches its target (an apply must
    // land on what the picker was opened for, not on whatever the live
    // selection happens to be when it closes).
    property int colorPickerRow: -1
    // Which row is capturing its next keypress as a shortcut ("" = none).
    property int shortcutCaptureRow: -1

    Item {
        width: parent.width
        height: resetRow.height

        // resetRow is an Item (not a Row) specifically so its MouseArea can
        // anchors.fill it — a Row's own direct children may not use fill/
        // left/right/horizontalCenter/centerIn anchors at all (it silently
        // disables the WHOLE Row's layout the moment one does; that was the
        // actual bug here). The icon+label still lay out via an inner Row.
        Item {
            id: resetRow
            anchors.right: parent.right
            width: resetRowContent.width
            height: resetRowContent.height

            Row {
                id: resetRowContent
                spacing: 6
                IconGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "cloudSync"
                    color: resetArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                    width: 13; height: 13
                    fit: true
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Reset to defaults")
                    color: resetArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
            }
            MouseArea {
                id: resetArea
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: GroupListModel.resetToDefaults()
            }
        }
    }

    Column {
        id: rowsCol
        width: parent.width
        spacing: Theme.space2

        Repeater {
            model: GroupListModel
            // Item, not Rectangle: the model's own "color" role (the
            // group's colour) would collide with Rectangle's NATIVE `color`
            // property if the delegate itself painted — both bindings land
            // on the same name, and the swatch/background below read
            // whichever one won instead of the group's actual hex (the
            // white rows / black swatches bug). The background below is a
            // plain CHILD Rectangle instead, with nothing to collide with.
            delegate: Item {
                id: row
                required property int index
                required property string name
                required property string color
                required property string shortcut
                required property bool isDefault

                width: rowsCol.width
                height: 44

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusMd
                    color: Theme.inset
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.right: deleteBtn.left
                    anchors.rightMargin: 10
                    spacing: 10

                    // Colour swatch — opens the shared colour picker (solid
                    // colours only; a group's colour is a flat tag colour,
                    // not a background with its own transparency concern).
                    Rectangle {
                        width: 28; height: 28; radius: 6
                        anchors.verticalCenter: parent.verticalCenter
                        color: row.color
                        border.width: 1
                        border.color: Theme.border
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.colorPickerRow = row.index
                                groupColorModal.openWith(row.color)
                            }
                        }
                    }

                    SettingsField {
                        width: 200
                        anchors.verticalCenter: parent.verticalCenter
                        showLabel: false
                        text: row.name
                        onTextEdited: (t) => GroupListModel.renameGroup(row.index, t)
                        onAccepted: GroupListModel.renameGroup(row.index, text)
                    }
                    // Shared single-key capture used by both group and action rows.
                    ShortcutCaptureField {
                        width: 90; height: 28
                        anchors.verticalCenter: parent.verticalCenter
                        shortcut: row.shortcut
                        capturing: root.shortcutCaptureRow === row.index
                        onCaptureRequested: root.shortcutCaptureRow = capturing ? -1 : row.index
                        onCaptureCancelled: root.shortcutCaptureRow = -1
                        onShortcutEdited: (value) => {
                            GroupListModel.setShortcut(row.index, value)
                            root.shortcutCaptureRow = -1
                        }
                    }

                    Text {
                        visible: row.isDefault
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("default")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.italic: true
                    }
                }

                Rectangle {
                    id: deleteBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28; height: 28; radius: 6
                    color: deleteHover.hovered ? Qt.alpha(Theme.danger, 0.16) : "transparent"
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "close"
                        color: deleteHover.hovered ? Theme.dangerLight : Theme.textMuted
                        width: 12; height: 12
                    }
                    HoverHandler { id: deleteHover }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: GroupListModel.removeGroup(row.index)
                    }
                }
            }
        }
    }

    AppButton {
        width: parent.width
        text: qsTr("+ Add")
        variant: "secondary"
        onClicked: GroupListModel.addGroup()
    }

    BackgroundColorModal {
        id: groupColorModal
        onApplied: (selection) => {
            if (root.colorPickerRow < 0)
                return
            const hex = selection.kind === "color" ? selection.color : selection.from
            GroupListModel.setColor(root.colorPickerRow, hex)
            root.colorPickerRow = -1
        }
        onCancelled: root.colorPickerRow = -1
    }
}
