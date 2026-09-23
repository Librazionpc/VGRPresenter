import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Styles — the presentation "themes" an output renders with,
// promoted from a card inside Outputs to its own section. Roster lives in
// the StyleListModel C++ singleton (starts empty — nothing hardcoded, same
// rule as slides). Rows are inline-editable; each shows its live
// "applied by N outputs" count from the shared OutputListModel.
Item {
    id: root

    // Bumped on any change to either model so the usage counts (and the
    // empty-state text below) recompute. Plain Q_INVOKABLE reads in
    // bindings aren't tracked, so the revision counter is the honest
    // dependency — StyleListModel is watched too now, not just
    // OutputListModel: adding/removing a style is exactly the kind of
    // change "No styles yet" needs to react to.
    property int modelsRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: StyleListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    function styleUsage(styleIdx) {
        let count = 0
        const rows = OutputListModel.rowCount()
        for (let i = 0; i < rows; ++i) {
            if (OutputListModel.getOutput(i).styleIndex === styleIdx)
                count++
        }
        return count
    }

    // ---- Edit Style dialog ----
    // Editable copies loaded when the dialog opens — the model is only
    // written on Save, matching every other dialog's Cancel/Save contract
    // (see OutputsScreen.qml's openEdit/saveEdit for the same pattern).
    property int editStyleIndex: -1
    property string editStyleName: ""
    property string editStyleContentType: "shows"
    property string editStyleTemplateKey: "lowerThird"
    property string editStyleBackgroundColor: "transparent"

    function openEditStyle(index) {
        const data = StyleListModel.getStyle(index)
        root.editStyleIndex = index
        root.editStyleName = data.name
        root.editStyleContentType = data.contentType
        root.editStyleTemplateKey = data.templateKey
        root.editStyleBackgroundColor = data.backgroundColor
    }

    function saveEditStyle() {
        if (root.editStyleIndex < 0)
            return
        StyleListModel.renameStyle(root.editStyleIndex, root.editStyleName)
        StyleListModel.setContentType(root.editStyleIndex, root.editStyleContentType)
        StyleListModel.setTemplateKey(root.editStyleIndex, root.editStyleTemplateKey)
        StyleListModel.setBackgroundColor(root.editStyleIndex, root.editStyleBackgroundColor)
        root.editStyleIndex = -1
    }

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
                    text: qsTr("Styles")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Presentation themes an output renders with — assign one per screen in its Edit dialog.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Styles card ----
            Rectangle {
                width: parent.width
                height: stylesCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: stylesCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    // A plain Item, not a Row — Row/Column/Grid forbid their
                    // own children from using anchors.left/right/fill/
                    // centerIn (the positioner sets each child's geometry
                    // itself), and both children here need anchors to sit
                    // on opposite edges of the row.
                    Item {
                        width: parent.width
                        height: Math.max(styleHeaderText.implicitHeight, addStyleButton.height)

                        Text {
                            id: styleHeaderText
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("All styles")
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }

                        AppButton {
                            id: addStyleButton
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add style")
                            variant: "ghost"
                            onClicked: StyleListModel.addStyle()
                        }
                    }

                    // Empty state — the roster starts empty by design.
                    // root.modelsRev is read (not used) purely to establish
                    // a reactive dependency — rowCount() is a plain
                    // Q_INVOKABLE call, so this wouldn't otherwise notice a
                    // style being added or removed (see modelsRev's own
                    // header comment).
                    Text {
                        visible: (root.modelsRev, StyleListModel.rowCount() === 0)
                        width: parent.width
                        text: qsTr("No styles yet. Add one, then assign it to an output via its Edit dialog.")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        topPadding: 4
                    }

                    Repeater {
                        model: StyleListModel

                        delegate: Rectangle {
                            id: styleRow
                            required property int index
                            required property string name
                            required property string res
                            required property string contentType
                            required property string templateKey
                            required property string backgroundColor
                            // backgroundColor is a model STRING role (Qt
                            // role values arrive as strings, not color) — the
                            // string comparison is correct here.
                            readonly property bool isTransparent: styleRow.backgroundColor === "transparent"
                            // `bg` is a pre-tinted 8-digit ARGB hex literal,
                            // not computed via Qt.rgba(color.r, ...) — the
                            // map's `color` values here are plain JS
                            // strings (not QML `color` values), which have
                            // no .r/.g/.b to read.
                            readonly property var contentTypeInfo: ({
                                shows: { label: qsTr("Shows"), color: "#9b8ff5", bg: "#2e6c5ce7" },
                                media: { label: qsTr("Media"), color: "#5eead4", bg: "#2e14b8a6" },
                                scripture: { label: qsTr("Scripture"), color: "#fbbf24", bg: "#2ef39c12" }
                            }[styleRow.contentType] ?? { label: styleRow.contentType, color: Theme.textMuted, bg: Theme.chip })

                            width: stylesCol.width
                            height: Math.max(64, rowCol.implicitHeight + 16)
                            radius: Theme.radiusMd
                            color: Theme.inset

                            // Background preview swatch — a checkerboard
                            // shows through "transparent" (same convention
                            // as BackgroundColorModal's own swatches), a
                            // real picked color renders solid otherwise.
                            Rectangle {
                                id: swatch
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 40
                                height: 40
                                radius: Theme.radiusMd
                                clip: true
                                color: styleRow.isTransparent ? "#1a1c26" : styleRow.backgroundColor
                                border.color: Theme.borderSubtle
                                border.width: 1

                                Grid {
                                    visible: styleRow.isTransparent
                                    anchors.fill: parent
                                    columns: 4
                                    rows: 4
                                    Repeater {
                                        model: 16
                                        delegate: Rectangle {
                                            required property int index
                                            width: 10
                                            height: 10
                                            color: (Math.floor(index / 4) + (index % 4)) % 2 === 0 ? "#242633" : "#15161d"
                                        }
                                    }
                                }
                            }

                            Column {
                                id: rowCol
                                anchors.left: swatch.right
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                // Inline-editable name — looks like a label,
                                // commits on Enter / focus loss. Quick
                                // renames stay inline; content type/template/
                                // background need the full Edit Style dialog
                                // (see editStyleBtn below).
                                TextInput {
                                    width: Math.max(implicitWidth + 2, 120)
                                    text: styleRow.name
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.Medium
                                    selectByMouse: true
                                    onEditingFinished: StyleListModel.renameStyle(styleRow.index, text)
                                }

                                Row {
                                    spacing: 6

                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 18
                                        width: contentTypeLabel.implicitWidth + 12
                                        radius: 4
                                        color: styleRow.contentTypeInfo.bg

                                        Text {
                                            id: contentTypeLabel
                                            anchors.centerIn: parent
                                            text: styleRow.contentTypeInfo.label
                                            color: styleRow.contentTypeInfo.color
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: templatePicker.nameFor(styleRow.templateKey)
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }

                                Row {
                                    spacing: 8

                                    TextInput {
                                        width: Math.max(implicitWidth + 2, 80)
                                        text: styleRow.res
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        selectByMouse: true
                                        onEditingFinished: StyleListModel.setResolution(styleRow.index, text)
                                    }

                                    Text {
                                        // modelsRev referenced inside the
                                        // binding keeps this count honest.
                                        text: {
                                            root.modelsRev
                                            const u = root.styleUsage(styleRow.index)
                                            return "·  applied by " + u + (u === 1 ? " output" : " outputs")
                                        }
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                            }

                            Rectangle {
                                id: editStyleBtn
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                height: 26
                                width: editStyleLabel.implicitWidth + 18
                                radius: 13
                                color: editStyleArea.containsMouse ? Theme.chip : "transparent"
                                border.color: Theme.border
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    id: editStyleLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Edit")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                    font.weight: Font.Medium
                                }

                                MouseArea {
                                    id: editStyleArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openEditStyle(styleRow.index)
                                }
                            }
                        }
                    }
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

    // ---- Edit Style dialog ----
    // Matches 1BBTIwaya/VGRPresenter_Settings_Outputs_Edit.qml's
    // style_dialog reference, scoped down deliberately: video/audio bus
    // routing (no bus concept exists in this app) and screen behavior/
    // geometry (always-on-top, locked, transparent, cropping — all
    // per-OUTPUT window concerns, not shared-theme concerns, and already
    // live on OutputListModel/OutputsScreen.qml) are left out rather than
    // duplicated here.
    ModalCard {
        id: editStyleDialog
        shown: root.editStyleIndex >= 0
        title: qsTr("Edit Style")
        subtitle: qsTr("Choose the content type, template, and background for this style.")
        cardWidth: 560
        saveText: qsTr("Save Changes")
        onCancelled: root.editStyleIndex = -1
        onAccepted: root.saveEditStyle()

        SettingsField {
            width: parent.width
            label: qsTr("Style name")
            text: root.editStyleName
            onTextEdited: (t) => root.editStyleName = t
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Content type")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Row {
                spacing: Theme.space2

                Repeater {
                    model: [
                        { key: "shows", label: qsTr("Shows") },
                        { key: "media", label: qsTr("Media") },
                        { key: "scripture", label: qsTr("Scripture") }
                    ]
                    delegate: SelectableChip {
                        required property var modelData
                        label: modelData.label
                        selected: root.editStyleContentType === modelData.key
                        onPicked: root.editStyleContentType = modelData.key
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Template")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Rectangle {
                width: parent.width
                height: 46
                radius: Theme.radiusMd
                color: Theme.inset

                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: templatePicker.nameFor(root.editStyleTemplateKey)
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 26
                    width: changeTemplateLabel.implicitWidth + 22
                    radius: 13
                    color: changeTemplateArea.containsMouse ? Theme.chip : "transparent"
                    border.color: Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        id: changeTemplateLabel
                        anchors.centerIn: parent
                        text: qsTr("Change")
                        color: Theme.accentLight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: changeTemplateArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            templatePicker.contentType = root.editStyleContentType
                            templatePicker.contentTypeLabel = {
                                "shows": qsTr("Shows"), "media": qsTr("Media"), "scripture": qsTr("Scripture")
                            }[root.editStyleContentType] ?? qsTr("Shows")
                            templatePicker.selectedKey = root.editStyleTemplateKey
                            templatePicker.open = true
                        }
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Background")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Rectangle {
                width: parent.width
                height: 46
                radius: Theme.radiusMd
                color: Theme.inset

                Row {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        radius: 6
                        color: root.editStyleBackgroundColor === "transparent" ? Theme.inset : root.editStyleBackgroundColor
                        border.color: Theme.borderSubtle
                        border.width: 1
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.editStyleBackgroundColor === "transparent" ? qsTr("Transparent") : root.editStyleBackgroundColor
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: 26
                    width: changeBgLabel.implicitWidth + 22
                    radius: 13
                    color: changeBgArea.containsMouse ? Theme.chip : "transparent"
                    border.color: Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        id: changeBgLabel
                        anchors.centerIn: parent
                        text: qsTr("Change")
                        color: Theme.accentLight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: changeBgArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: styleBgModal.open = true
                    }
                }
            }
        }
    }

    TemplatePickerModal {
        id: templatePicker
        onApplied: (tpl) => {
            root.editStyleTemplateKey = tpl.key
            templatePicker.open = false
        }
        onCancelled: templatePicker.open = false
    }

    BackgroundColorModal {
        id: styleBgModal
        title: qsTr("Background Color")
        onApplied: (selection) => {
            root.editStyleBackgroundColor = selection.kind === "color" ? selection.color : selection.from
            styleBgModal.open = false
        }
        onCancelled: styleBgModal.open = false
    }
}
