import QtQuick
import VGRPresenterUI

// Settings · Appearance & Actions · "Manage groups": the global registry of
// named song-section tags a
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
//
// THE ROW IS A JOINED TABLE, not a stack of separate fields: one strip whose
// cells touch edge to edge (a 1px divider between them, no inner corner
// radius), holding in order
//     Name        caption over the value, flex 1
//     Colour      a whole CELL of colour, not a small chip
//     Shortcut    the group's live letter
//     Template    the template the group's slides are laid out with
//     Bin         removes the group
// and then a full-width outlined "+ Add" under the list. The version this
// replaced was four detached controls inside a rounded row, with a colour chip
// and a stray "default" caption — it read as a form, not a roster.
//
// Three things stay this app's own, deliberately:
//   * the colours of the cells' INTERNAL captions follow the app's own
//     "filled = accent, empty = muted" rule (ShortcutCaptureField's own cue);
//   * there is no per-row reset/undo control: this dialog writes through on
//     every change, so "Reset to defaults" above the list is the only reset
//     the app offers;
//   * the delete cell is the app's own trash glyph on a danger hover.
ModalCard {
    id: root
    objectName: "manageGroupsModal"
    title: qsTr("Manage groups")
    subtitle: qsTr("Every slide can carry one of these as its group — the colour marks it in the slide grid, the shortcut jumps to the next slide carrying it while live, and the template lays its slides out.")
    cardWidth: 720
    showFooter: false
    onCancelled: root.close()

    // The trailing cells hold their widths, so every row's Name cell is the
    // same width and the captions line up down the list.
    readonly property int colourCellWidth: 110
    readonly property int shortcutCellWidth: 130
    readonly property int templateCellWidth: 150
    readonly property int deleteCellWidth: 40
    // The joined strip is 1px border all round, so the cells start one pixel in.
    readonly property int innerX: 1
    readonly property int innerWidth: rowsCol.width - 2
    readonly property int innerHeight: 48      // 50px row less the 1px borders

    // Which row's colour swatch is being edited — latched the same way
    // EditScreen's openBackgroundPicker latches its target (an apply must
    // land on what the picker was opened for, not on whatever the live
    // selection happens to be when it closes).
    property int colorPickerRow: -1
    // Which row is capturing its next keypress as a shortcut ("" = none).
    property int shortcutCaptureRow: -1
    // Which row's template picker is open (-1 = none).
    property int templatePickerRow: -1

    // The template a stored id names — the picker's own registry is the one
    // list of templates, so a row never invents its own name table.
    function templateName(key) {
        if (!key)
            return ""
        const list = TemplateLibraryService.designs()
        for (let i = 0; i < list.length; ++i) {
            if (list[i].id === key)
                return list[i].name
        }
        return key
    }
    function openTemplatePicker(rowIndex) {
        const categoryNames = {}
        for (const c of TemplateLibraryService.categories)
            categoryNames[c.id] = c.name
        groupTemplatePicker.templates = TemplateLibraryService.designs().map((t) => ({
            key: t.id, name: t.name, color: t.color,
            category: t.category,
            categoryName: t.category ? (categoryNames[t.category] ?? t.category) : qsTr("Unlabeled")
        }))
        root.templatePickerRow = rowIndex
        groupTemplatePicker.selectedKey = String(GroupListModel.getGroup(rowIndex).templateId ?? "")
        groupTemplatePicker.open = true
    }

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
        spacing: 0        // the rows are a joined table: they touch

        Repeater {
            model: GroupListModel
            // Item, not Rectangle: the model's own "color" role (the
            // group's colour) would collide with Rectangle's NATIVE `color`
            // property if the delegate itself painted — both bindings land
            // on the same name, and the cells below read whichever one won
            // instead of the group's actual hex (the white rows / black
            // cells bug). Every surface here is a plain CHILD Rectangle.
            delegate: Item {
                id: row
                required property int index
                required property string name
                required property string color
                required property string shortcut
                required property string templateId
                required property bool isDefault

                width: rowsCol.width
                height: 50

                // ---- the strip: ONE surface behind the four cells ----
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusMd
                    color: Theme.inset
                    border.width: 1
                    border.color: Theme.border
                    clip: true
                }

                // ---- cell 1 · Name (caption above the value) ----
                Item {
                    id: nameCell
                    x: root.innerX
                    y: root.innerX
                    width: root.innerWidth - root.colourCellWidth - root.shortcutCellWidth
                           - root.templateCellWidth - root.deleteCellWidth
                    height: root.innerHeight

                    Text {
                        id: nameCaption
                        x: 13; y: 6
                        text: qsTr("Name")
                        // Lit once the cell carries a value — the same cue the
                        // shortcut cell beside it gives.
                        color: nameInput.text !== "" ? Theme.accentLight : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    TextInput {
                        id: nameInput
                        anchors.left: parent.left
                        anchors.leftMargin: 13
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 8
                        text: row.name
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: row.isDefault ? Font.Normal : Font.DemiBold
                        clip: true
                        selectByMouse: true
                        onTextEdited: (t) => GroupListModel.renameGroup(row.index, t)
                        onAccepted: GroupListModel.renameGroup(row.index, text)
                    }
                }

                // ---- cell 2 · the colour, as a CELL not a chip ----
                // An ITEM, not a Rectangle: a bare Rectangle paints WHITE by
                // default, so the cell showed a white block behind its swatch and
                // broke the row's own dark surface (reported from the first build
                // of this layout: "it cant be whit let match with others bg colour").
                Item {
                    id: colourCell
                    x: nameCell.x + nameCell.width
                    y: root.innerX
                    width: root.colourCellWidth
                    height: root.innerHeight

                    Rectangle {
                        id: swatch
                        anchors.fill: parent
                        anchors.margins: 7
                        radius: 3
                        color: row.color
                        // A hairline in the app's own ink so a near-black group
                        // colour still reads as a swatch inside the dark row.
                        border.width: 1
                        border.color: Qt.alpha(Theme.textPrimary, 0.14)
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.colorPickerRow = row.index
                                groupColorModal.openWith(row.color)
                            }
                        }
                    }
                }

                // ---- cell 3 · Group shortcut ----
                ShortcutCaptureField {
                    id: shortcutCell
                    variant: "boxed"
                    frameless: true
                    x: colourCell.x + colourCell.width
                    y: root.innerX
                    width: root.shortcutCellWidth
                    height: root.innerHeight
                    label: qsTr("Group shortcut")
                    shortcut: row.shortcut
                    capturing: root.shortcutCaptureRow === row.index
                    onCaptureRequested: root.shortcutCaptureRow = capturing ? -1 : row.index
                    onCaptureCancelled: root.shortcutCaptureRow = -1
                    onShortcutEdited: (value) => {
                        GroupListModel.setShortcut(row.index, value)
                        root.shortcutCaptureRow = -1
                    }
                }

                // ---- cell 4 · the group's own template ----
                // A group can name the template its slides use: the picker is the
                // app's own Choose-template dialog (the same list the style and
                // the reference tabs pick from), so there is one template
                // registry and one picker, not a per-dialog copy.
                Item {
                    id: templateCell
                    x: shortcutCell.x + shortcutCell.width
                    y: root.innerX
                    width: root.templateCellWidth
                    height: root.innerHeight

                    readonly property string current: String(row.templateId ?? "")

                    Text {
                        id: templateCaption
                        x: 13; y: 6
                        text: qsTr("Template")
                        color: templateCell.current !== "" ? Theme.accentLight : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    Text {
                        id: templateValue
                        anchors.left: parent.left
                        anchors.leftMargin: 13
                        anchors.right: clearTemplate.left
                        anchors.rightMargin: 6
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 8
                        text: root.templateName(templateCell.current)
                        visible: templateCell.current !== ""
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    IconGlyph {
                        id: clearTemplate
                        visible: templateCell.current !== ""
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        name: "close"
                        color: clearHover.hovered ? Theme.dangerLight : Theme.textMuted
                        width: 11; height: 11
                        HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: GroupListModel.setTemplate(row.index, "")
                        }
                    }
                    IconGlyph {
                        visible: templateCell.current === ""
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        name: "chevronRight"
                        color: Theme.textMuted
                        width: 11; height: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.openTemplatePicker(row.index)
                    }
                }

                // ---- cell 5 · the bin ----
                Item {
                    id: deleteCell
                    x: templateCell.x + templateCell.width
                    y: root.innerX
                    width: root.deleteCellWidth
                    height: root.innerHeight

                    Rectangle {
                        anchors.centerIn: parent
                        width: 26; height: 26; radius: 6
                        color: deleteHover.hovered ? Qt.alpha(Theme.danger, 0.16) : "transparent"
                        IconGlyph {
                            anchors.centerIn: parent
                            name: "trash"
                            color: deleteHover.hovered ? Theme.dangerLight : Theme.textMuted
                            width: 14; height: 14; fit: true
                        }
                        HoverHandler { id: deleteHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: GroupListModel.removeGroup(row.index)
                        }
                    }
                }

                // ---- the 1px joins between the cells ----
                Repeater {
                    model: [nameCell.x + nameCell.width, colourCell.x + colourCell.width,
                            shortcutCell.x + shortcutCell.width,
                            templateCell.x + templateCell.width]
                    delegate: Rectangle {
                        required property var modelData
                        x: modelData
                        y: root.innerX
                        width: 1
                        height: root.innerHeight
                        color: Theme.border
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

    TemplatePickerModal {
        id: groupTemplatePicker
        z: 30000
        contentType: "shows"
        contentTypeLabel: qsTr("All")
        onApplied: (tpl) => {
            groupTemplatePicker.open = false
            if (root.templatePickerRow >= 0)
                GroupListModel.setTemplate(root.templatePickerRow, tpl.key)
            root.templatePickerRow = -1
        }
        onCancelled: {
            groupTemplatePicker.open = false
            root.templatePickerRow = -1
        }
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
