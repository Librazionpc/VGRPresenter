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

    // How many outputs wear the style — by the style's STABLE id (outputs
    // reference styles by id; the old index reference died with reordering).
    function styleUsage(styleId) {
        let count = 0
        const rows = OutputListModel.rowCount()
        for (let i = 0; i < rows; ++i) {
            if (OutputListModel.getOutput(i).styleId === styleId)
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
    property string editStyleBackgroundImage: ""
    property bool editStyleClearOnText: false
    // The four content pills (shows/media/scripture/table) — grey = that
    // tab's content is NOT meant for an output wearing this style.
    property bool editShowShows: true
    property bool editShowMedia: true
    property bool editShowScripture: true
    property bool editShowTable: true
    // Shows-only category (free text).
    property string editStyleCategory: ""

    function openEditStyle(index) {
        const data = StyleListModel.getStyle(index)
        root.editStyleIndex = index
        root.editStyleName = data.name
        root.editStyleContentType = data.contentType
        root.editStyleTemplateKey = data.templateKey
        root.editStyleBackgroundColor = data.backgroundColor
        root.editStyleBackgroundImage = data.backgroundImage
        root.editStyleClearOnText = data.clearBackgroundOnText
        root.editShowShows = data.showShows
        root.editShowMedia = data.showMedia
        root.editShowScripture = data.showScripture
        root.editShowTable = data.showTable
        root.editStyleCategory = data.category
    }

    function saveEditStyle() {
        if (root.editStyleIndex < 0)
            return
        StyleListModel.renameStyle(root.editStyleIndex, root.editStyleName)
        StyleListModel.setContentType(root.editStyleIndex, root.editStyleContentType)
        StyleListModel.setTemplateKey(root.editStyleIndex, root.editStyleTemplateKey)
        StyleListModel.setBackgroundColor(root.editStyleIndex, root.editStyleBackgroundColor)
        StyleListModel.setBackgroundImage(root.editStyleIndex, root.editStyleBackgroundImage)
        StyleListModel.setClearBackgroundOnText(root.editStyleIndex, root.editStyleClearOnText)
        StyleListModel.setShowTemplate(root.editStyleIndex, "shows", root.editShowShows)
        StyleListModel.setShowTemplate(root.editStyleIndex, "media", root.editShowMedia)
        StyleListModel.setShowTemplate(root.editStyleIndex, "scripture", root.editShowScripture)
        StyleListModel.setShowTemplate(root.editStyleIndex, "table", root.editShowTable)
        StyleListModel.setCategory(root.editStyleIndex, root.editStyleCategory)
        root.editStyleIndex = -1
    }

    // Native file picker for the style background image (same engine dialogs
    // ShowService/ScriptureService use). Non-image files simply fail to
    // decode later and the colour shows instead — a second dialog would be
    // kinder but this keeps the flow one click.
    function pickBackgroundImage() {
        const path = StyleListModel.pickImageFile()
        if (path !== "")
            root.editStyleBackgroundImage = path
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
                            required property string styleId
                            required property string name
                            required property string res
                            required property string contentType
                            required property string templateKey
                            required property string backgroundColor
                            required property string backgroundImage
                            required property bool showShows
                            required property bool showMedia
                            required property bool showScripture
                            required property bool showTable
                            required property string category
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
                                scripture: { label: qsTr("Scripture"), color: "#fbbf24", bg: "#2ef39c12" },
                                table: { label: qsTr("The Table"), color: "#4ae0b0", bg: "#2e4ae0b0" }
                            }[styleRow.contentType] ?? { label: styleRow.contentType, color: Theme.textMuted, bg: Theme.chip })
                            // The four template pills (greyed = off) + image chip,
                            // right on the row — the Edit dialog state at a glance.
                            readonly property bool rowShowShows: showShows
                            readonly property bool rowShowMedia: showMedia
                            readonly property bool rowShowScripture: showScripture
                            readonly property bool rowShowTable: showTable
                            readonly property string rowImage: backgroundImage
                            readonly property string rowCategory: category

                            width: stylesCol.width
                            height: Math.max(64, rowCol.implicitHeight + 16)
                            radius: Theme.radiusMd
                            color: Theme.inset

                            // Background preview swatch — the style's IMAGE
                            // when it has one (cover-cropped), else a checkerboard
                            // behind "transparent" (same convention as
                            // BackgroundColorModal's own swatches), a real picked
                            // color renders solid otherwise.
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
                                    visible: styleRow.isTransparent && styleRow.rowImage === ""
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

                                Image {
                                    visible: styleRow.rowImage !== ""
                                    anchors.fill: parent
                                    source: styleRow.rowImage === "" ? "" : "file:///" + styleRow.rowImage
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
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

                                    // Mini content pills — grey = that tab is
                                    // switched off for this style.
                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 4

                                        Repeater {
                                            model: [
                                                { key: "shows", label: qsTr("Sh"), on: styleRow.rowShowShows },
                                                { key: "media", label: qsTr("Me"), on: styleRow.rowShowMedia },
                                                { key: "scripture", label: qsTr("Sc"), on: styleRow.rowShowScripture },
                                                { key: "table", label: qsTr("Ta"), on: styleRow.rowShowTable }
                                            ]
                                            delegate: Rectangle {
                                                required property var modelData
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: miniLabel.implicitWidth + 8
                                                height: 15
                                                radius: 7
                                                color: modelData.on ? "#2e34404d" : "transparent"
                                                border.color: modelData.on ? "#4a5064" : "#2a2c38"
                                                border.width: 1
                                                opacity: modelData.on ? 1 : 0.4

                                                Text {
                                                    id: miniLabel
                                                    anchors.centerIn: parent
                                                    text: parent.modelData.label
                                                    color: parent.modelData.on ? Theme.textSecondary : Theme.textMuted
                                                    font.family: Theme.fontFamily
                                                    font.pixelSize: 9
                                                    font.bold: parent.modelData.on
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        visible: styleRow.rowCategory !== ""
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "·  " + styleRow.rowCategory
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.italic: true
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
                                            const u = root.styleUsage(styleRow.styleId)
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
                                anchors.right: duplicateStyleBtn.left
                                anchors.rightMargin: 6
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

                            // Duplicate — copies the theme (new stable id, "(copy)"
                            // name); the fastest way to spin a variant off one.
                            Rectangle {
                                id: duplicateStyleBtn
                                anchors.right: deleteStyleBtn.visible ? deleteStyleBtn.left : deleteStyleBtn.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                height: 26
                                width: duplicateStyleLabel.implicitWidth + 18
                                radius: 13
                                color: duplicateStyleArea.containsMouse ? Theme.chip : "transparent"
                                border.color: Theme.border
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    id: duplicateStyleLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Duplicate")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                    font.weight: Font.Medium
                                }

                                MouseArea {
                                    id: duplicateStyleArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: StyleListModel.duplicateStyle(styleRow.index)
                                }
                            }

                            // Delete — hidden while any output still points at the
                            // style (the usage count above says the same thing): a
                            // style an output wears can't go away silently. After
                            // the last output is re-pointed to None, the row goes.
                            Rectangle {
                                id: deleteStyleBtn
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.styleUsage(styleRow.styleId) === 0
                                height: 26
                                width: deleteStyleLabel.implicitWidth + 18
                                radius: 13
                                color: deleteStyleArea.containsMouse ? Theme.chip : "transparent"
                                border.color: Theme.border
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    id: deleteStyleLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Delete")
                                    color: Theme.dangerLight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                    font.weight: Font.Medium
                                }

                                MouseArea {
                                    id: deleteStyleArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: StyleListModel.removeStyle(styleRow.index)
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
                        { key: "scripture", label: qsTr("Scripture") },
                        { key: "table", label: qsTr("The Table") }
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

        // TEMPLATES PER CONTENT TYPE (FreeShow: one style carries a template
        // for Shows, Media, Scripture and Table). A pill OFF greys out — that
        // tab's content is NOT meant for an output wearing this style; go-live
        // refuses it with a toast instead of showing nothing.
        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Templates for")
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
                        { key: "scripture", label: qsTr("Scripture") },
                        { key: "table", label: qsTr("The Table") }
                    ]
                    delegate: Rectangle {
                        id: tmplPill
                        required property var modelData
                        readonly property bool on: {
                            if (modelData.key === "shows") return root.editShowShows
                            if (modelData.key === "media") return root.editShowMedia
                            if (modelData.key === "scripture") return root.editShowScripture
                            return root.editShowTable
                        }
                        width: tmplPillLabel.implicitWidth + 22
                        height: 26
                        radius: 13
                        border.color: tmplPill.on ? Theme.accent : Theme.border
                        border.width: 1
                        color: tmplPill.on ? "#2e6c5ce7" : "transparent"
                        opacity: tmplPill.on ? 1.0 : 0.45
                        Behavior on opacity { NumberAnimation { duration: 120 } }

                        Text {
                            id: tmplPillLabel
                            anchors.centerIn: parent
                            text: tmplPill.modelData.label
                            color: tmplPill.on ? Theme.textPrimary : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            font.weight: tmplPill.on ? Font.DemiBold : Font.Normal
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (tmplPill.modelData.key === "shows") root.editShowShows = !root.editShowShows
                                else if (tmplPill.modelData.key === "media") root.editShowMedia = !root.editShowMedia
                                else if (tmplPill.modelData.key === "scripture") root.editShowScripture = !root.editShowScripture
                                else root.editShowTable = !root.editShowTable
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("A greyed tab is not meant for this output — its content is refused on air.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }
        }

        // SHOWS-ONLY CATEGORY (free text label — filed under in the shows
        // library; only meaningful when the Shows pill is on).
        Column {
            width: parent.width
            spacing: Theme.space2
            visible: root.editShowShows

            Text {
                text: qsTr("Category (Shows only)")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Rectangle {
                width: parent.width
                height: 40
                radius: Theme.radiusMd
                color: Theme.inset

                TextInput {
                    anchors.fill: parent
                    anchors.margins: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.editStyleCategory
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    selectByMouse: true
                    clip: true
                    onTextEdited: (t) => root.editStyleCategory = t
                }
                Text {
                    visible: root.editStyleCategory === ""
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("e.g. Worship")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
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
                                "shows": qsTr("Shows"), "media": qsTr("Media"), "scripture": qsTr("Scripture"),
                                "table": qsTr("The Table")
                            }[root.editStyleContentType] ?? qsTr("Shows")
                            // The Table's templates live in the ENGINE catalog (the
                            // same "table" category the tab's own picker lists), not
                            // in this default list — hand them in, keyed by id, the
                            // way ReferencePane does for its tabs.
                            if (root.editStyleContentType === "table") {
                                templatePicker.templates = TemplateLibraryService.designs("table").map((t) => ({
                                    key: t.id, name: t.name, color: t.color
                                }))
                            }
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

            // Background IMAGE (absolute path, "" = none) — painted cover-fit
            // on air BEHIND all content, over the colour (FreeShow's
            // style backgroundImage). The preview shows the file name.
            Column {
                width: parent.width
                spacing: Theme.space2

                Text {
                    text: qsTr("Background image")
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
                        width: parent.width - 110

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            height: 22
                            radius: 6
                            clip: true
                            color: root.editStyleBackgroundImage === "" ? Theme.inset : "#22242e"
                            border.color: Theme.borderSubtle
                            border.width: 1

                            Image {
                                visible: root.editStyleBackgroundImage !== ""
                                anchors.fill: parent
                                source: root.editStyleBackgroundImage === "" ? "" : "file:///" + root.editStyleBackgroundImage
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 32
                            text: root.editStyleBackgroundImage === ""
                                  ? qsTr("None")
                                  : root.editStyleBackgroundImage.split("/").pop()
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        height: 26
                        width: pickImgLabel.implicitWidth + 22
                        radius: 13
                        color: pickImgArea.containsMouse ? Theme.chip : "transparent"
                        border.color: Theme.border
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            id: pickImgLabel
                            anchors.centerIn: parent
                            text: root.editStyleBackgroundImage === "" ? qsTr("Pick") : qsTr("Change")
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            font.weight: Font.Medium
                        }

                        MouseArea {
                            id: pickImgArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.pickBackgroundImage()
                        }
                    }
                }

                Rectangle {
                    visible: root.editStyleBackgroundImage !== ""
                    anchors.right: parent.right
                    width: removeImgLabel.implicitWidth + 18
                    height: 22
                    radius: 11
                    color: "transparent"
                    border.color: Theme.border
                    border.width: 1

                    Text {
                        id: removeImgLabel
                        anchors.centerIn: parent
                        text: qsTr("Remove image")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.editStyleBackgroundImage = ""
                    }
                }
            }

            // FreeShow's clearStyleBackgroundOnText: slides that carry their own
            // background keep it when this style is on air; plain text slides
            // get the style's background. SettingsToggle is the pill only —
            // label/description are built here like GeneralScreen's rows
            // (controlled component: the dialog owns the state, the pill reports).
            Row {
                width: parent.width
                spacing: Theme.space2

                Column {
                    width: parent.width - clearOnTextToggle.width - parent.spacing
                    spacing: 1

                    Text {
                        text: qsTr("Let slides keep their own background")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        text: qsTr("Slides with their own background colour keep it; others get this style's.")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                }

                SettingsToggle {
                    id: clearOnTextToggle
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.editStyleClearOnText
                    onToggled: root.editStyleClearOnText = !root.editStyleClearOnText
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
