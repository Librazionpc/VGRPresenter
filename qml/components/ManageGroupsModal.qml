import QtQuick
import VGRPresenterUI

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