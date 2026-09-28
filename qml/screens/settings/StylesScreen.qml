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

    // A style's templateKey as a NAME: engine-template ids ("tpl-…") resolve
    // through the Template library (the picker's same catalog), legacy preset
    // keys show their friendly names, anything else falls back to the raw
    // key. Reads modelsRev so a catalog load / rename re-renders — a binding
    // over an invokable alone wouldn't.
    readonly property var templateCatalogRev: TemplateLibraryService.totalCount
    function templateNameFor(key) {
        void root.templateCatalogRev
        if (typeof key !== "string" || key === "")
            return qsTr("None")
        if (key.indexOf("tpl-") === 0) {
            const name = TemplateLibraryService.design(key).name
            if (name) return name
        }
        return templatePicker.nameFor(key)
    }
    // The template's own ACCENT from the engine design (its category chip
    // colour) — the card's template chip takes it, so the row reads as "this
    // exact engine design", not a grey label. Legacy presets get the slate
    // fallback (they have no design to read).
    function templateColorFor(key) {
        void root.templateCatalogRev
        if (typeof key === "string" && key.indexOf("tpl-") === 0) {
            const c = TemplateLibraryService.design(key).color
            if (c) return c
        }
        return ""
    }
    // Is this style ON the active output right now? (engine state, not the
    // roster's opinion — OutputListModel is the one that pushed the spec.)
    function isOnAir(styleId) {
        void root.modelsRev
        const ai = OutputListModel.activeIndex()
        return ai >= 0 && styleId !== "" && OutputListModel.getOutput(ai).styleId === styleId
    }

    // One shared FAMILY map — the style cards' content pills AND the Edit
    // dialog's content chips read the same labels/accents from here, so the
    // two surfaces can't drift. `bg` values are pre-tinted 8-digit ARGB
    // literals (plain JS strings — QML `color` values have no .r/.g/.b to
    // read for compositing).
    readonly property var familyInfo: ({
        shows:     { label: qsTr("Shows"),     color: "#9b8ff5", bg: "#2e6c5ce7" },
        media:     { label: qsTr("Media"),     color: "#5eead4", bg: "#2e14b8a6" },
        scripture: { label: qsTr("Scripture"), color: "#fbbf24", bg: "#2ef39c12" },
        table:     { label: qsTr("The Table"), color: "#4ae0b0", bg: "#2e4ae0b0" }
    })
    function familyInfoFor(key) {
        return root.familyInfo[key] ?? { label: String(key), color: Theme.textMuted, bg: Theme.chip }
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
    // The four content chips (shows/media/scripture/table) — buffer the
    // EDITED flags, loaded from the row on open. The dialog starts them
    // INACTIVE so a brand-new style's chips read as an empty slate (the row
    // itself carries the same default; the open overwrites these anyway).
    property bool editShowShows: false
    property bool editShowMedia: false
    property bool editShowScripture: false
    property bool editShowTable: false
    // PER-FAMILY template picks — each active content type can carry its OWN
    // template; "" = that family inherits the whole-style Template below.
    property string editFamilyShows: ""
    property string editFamilyMedia: ""
    property string editFamilyScripture: ""
    property string editFamilyTable: ""
    // The family the Template row edits (chips switch it; defaults to the
    // style's whole-style family).
    property string editTemplateFamily: "shows"
    // Shows-only category (free text).
    property string editStyleCategory: ""
    // The picked family's template key: its own pick when it has one, else
    // the whole-style key (what the Template row shows and edits).
    readonly property string editActiveFamilyKey: {
        if (root.editTemplateFamily === "shows" && root.editFamilyShows !== "") return root.editFamilyShows
        if (root.editTemplateFamily === "media" && root.editFamilyMedia !== "") return root.editFamilyMedia
        if (root.editTemplateFamily === "scripture" && root.editFamilyScripture !== "") return root.editFamilyScripture
        if (root.editTemplateFamily === "table" && root.editFamilyTable !== "") return root.editFamilyTable
        return root.editStyleTemplateKey
    }
    readonly property bool editActiveFamilyOverridden:
        root.editActiveFamilyKey !== root.editStyleTemplateKey

    function openEditStyle(index, family) {
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
        root.editFamilyShows = data.familyTemplateShows
        root.editFamilyMedia = data.familyTemplateMedia
        root.editFamilyScripture = data.familyTemplateScripture
        root.editFamilyTable = data.familyTemplateTable
        // A clicked card pill pre-targets ITS family in the dialog.
        root.editTemplateFamily = family || data.contentType || "shows"
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
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "shows", root.editFamilyShows)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "media", root.editFamilyMedia)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "scripture", root.editFamilyScripture)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "table", root.editFamilyTable)
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
                            // The row's content-type chip wears the SHARED
                            // family map (the pills below use the same one).
                            readonly property var contentTypeInfo: root.familyInfoFor(styleRow.contentType)
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

                                    // The template as a CHIP wearing the ENGINE
                                    // design's own accent (its category colour),
                                    // not a grey text label — the row now reads
                                    // "this exact engine design" at a glance.
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 18
                                        width: templateLabel.implicitWidth + 12
                                        radius: 4
                                        color: root.templateColorFor(styleRow.templateKey) !== ""
                                               ? root.templateColorFor(styleRow.templateKey) : "#262a3a"

                                        Text {
                                            id: templateLabel
                                            anchors.centerIn: parent
                                            text: root.templateNameFor(styleRow.templateKey)
                                            color: root.templateColorFor(styleRow.templateKey) !== ""
                                                   ? "#ffffff" : Theme.textPrimary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }

                                    // ON AIR — engine truth (this style is the
                                    // active output's): a live-red chip, not a
                                    // dot the eye has to hunt for.
                                    Rectangle {
                                        visible: root.isOnAir(styleRow.styleId)
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 18
                                        width: onAirLabel.implicitWidth + 12
                                        radius: 9
                                        color: "#33ff4d3d"
                                        border.color: "#66ff4d3d"
                                        border.width: 1

                                        Text {
                                            id: onAirLabel
                                            anchors.centerIn: parent
                                            text: qsTr("● ON AIR")
                                            color: "#ff6b61"
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                        }
                                    }

                                    // Content pills — one per family, FULL
                                    // names in each family's own accent:
                                    // coloured = the style serves that tab,
                                    // dimmed = off. A family with its OWN
                                    // template spells it out on the pill
                                    // ("Scripture · Lower Third 2"), so the
                                    // per-family overrides read right on the
                                    // card instead of hiding in the Edit
                                    // dialog. Clicking an active pill opens
                                    // the dialog pre-targeted to that family.
                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 4

                                        Repeater {
                                            // Override names ride the model so a
                                            // template pick/rename re-renders the
                                            // pills (modelsRev + the catalog rev
                                            // are the honest dependencies).
                                            model: {
                                                root.modelsRev
                                                root.templateCatalogRev
                                                const data = StyleListModel.getStyle(styleRow.index)
                                                return [
                                                    { key: "shows", on: styleRow.rowShowShows,
                                                      overrideName: data.familyTemplateShows !== "" ? root.templateNameFor(data.familyTemplateShows) : "" },
                                                    { key: "media", on: styleRow.rowShowMedia,
                                                      overrideName: data.familyTemplateMedia !== "" ? root.templateNameFor(data.familyTemplateMedia) : "" },
                                                    { key: "scripture", on: styleRow.rowShowScripture,
                                                      overrideName: data.familyTemplateScripture !== "" ? root.templateNameFor(data.familyTemplateScripture) : "" },
                                                    { key: "table", on: styleRow.rowShowTable,
                                                      overrideName: data.familyTemplateTable !== "" ? root.templateNameFor(data.familyTemplateTable) : "" }
                                                ]
                                            }
                                            delegate: Rectangle {
                                                id: familyPill
                                                required property var modelData
                                                readonly property var info: root.familyInfoFor(modelData.key)
                                                readonly property bool overridden: modelData.overrideName !== ""
                                                anchors.verticalCenter: parent.verticalCenter
                                                height: 18
                                                width: pillContent.implicitWidth + 12
                                                radius: 4
                                                color: modelData.on ? info.bg : "transparent"
                                                border.color: pillHover.containsMouse && modelData.on ? "#5a6076"
                                                    : (modelData.on ? "#4a5064" : "#2a2c38")
                                                border.width: 1
                                                opacity: modelData.on ? 1 : 0.45

                                                Row {
                                                    id: pillContent
                                                    anchors.centerIn: parent
                                                    spacing: 4

                                                    Text {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        text: familyPill.info.label
                                                        color: familyPill.modelData.on ? familyPill.info.color : Theme.textMuted
                                                        font.family: Theme.fontFamily
                                                        font.pixelSize: 10
                                                        font.weight: Font.Bold
                                                    }
                                                    Text {
                                                        visible: familyPill.overridden
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: Math.min(implicitWidth, 110)
                                                        elide: Text.ElideRight
                                                        text: "·  " + familyPill.modelData.overrideName
                                                        color: familyPill.modelData.on ? familyPill.info.color : Theme.textMuted
                                                        font.family: Theme.fontFamily
                                                        font.pixelSize: 9
                                                    }
                                                }
                                                MouseArea {
                                                    id: pillHover
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    enabled: familyPill.modelData.on
                                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                    onClicked: root.openEditStyle(styleRow.index, familyPill.modelData.key)
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        visible: styleRow.rowCategory !== ""
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "·  " + styleRow.rowCategory
                                        color: Theme.textSecondary
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
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: Font.Medium
                                        selectByMouse: true
                                        onEditingFinished: StyleListModel.setResolution(styleRow.index, text)
                                    }

                                    Text {
                                        // modelsRev referenced inside the
                                        // binding keeps this count honest. BOLD
                                        // when this style is actually on air —
                                        // the usage line carries state now, not
                                        // just a count.
                                        text: {
                                            root.modelsRev
                                            const u = root.styleUsage(styleRow.styleId)
                                            return "·  applied by " + u + (u === 1 ? " output" : " outputs")
                                        }
                                        color: root.isOnAir(styleRow.styleId) ? "#ff8a80" : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: root.isOnAir(styleRow.styleId) ? Font.DemiBold : Font.Normal
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

        // CONTENT TYPE = ACTIVATION (user request): a chip ON means an output
        // wearing this style ALLOWS that content type; OFF refuses it on air
        // (OutputListModel::activeStyleAllows reads these flags). Multi-select
        // — a style serves any subset. The style's template family
        // (editStyleContentType, what the Template picker below edits) follows
        // the chips: picking a chip re-targets the family, deactivating the
        // family's own chip migrates it to another active one.
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
                        readonly property bool on: {
                            if (modelData.key === "shows") return root.editShowShows
                            if (modelData.key === "media") return root.editShowMedia
                            if (modelData.key === "scripture") return root.editShowScripture
                            return root.editShowTable
                        }
                        label: modelData.label
                        selected: on
                        onPicked: {
                            const next = !on
                            if (modelData.key === "shows") root.editShowShows = next
                            else if (modelData.key === "media") root.editShowMedia = next
                            else if (modelData.key === "scripture") root.editShowScripture = next
                            else root.editShowTable = next
                            // The template family must stay on an ACTIVE
                            // type: picking a chip targets it (the Template
                            // row edits THAT family now); switching one OFF
                            // moves the target to another still-active chip
                            // (unchanged when it wasn't the target or when
                            // nothing else is active).
                            if (next) {
                                root.editStyleContentType = modelData.key
                                root.editTemplateFamily = modelData.key
                            } else if (root.editTemplateFamily === modelData.key) {
                                const others = ["shows", "media", "scripture", "table"]
                                    .filter((k) => k !== modelData.key
                                            && (k === "shows" ? root.editShowShows
                                                : k === "media" ? root.editShowMedia
                                                : k === "scripture" ? root.editShowScripture
                                                : root.editShowTable))
                                if (others.length > 0)
                                    root.editTemplateFamily = others[0]
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("An unselected type is not allowed on an output wearing this style — its content is refused on air. The Template below belongs to the picked type.")
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

        // TEMPLATE — PER CONTENT TYPE: the row edits the family picked by the
        // chips above. A family with its own pick shows its template name and
        // an "inherits" chip appears to reset it back to the whole-style
        // template; a family without one shows the whole-style template and
        // picking one here sets the family's OWN override.
        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Template for ") + ({
                    "shows": qsTr("Shows"), "media": qsTr("Media"),
                    "scripture": qsTr("Scripture"), "table": qsTr("The Table")
                }[root.editTemplateFamily] ?? qsTr("Shows"))
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
                    width: parent.width - resetFamilyTemplate.width - changeTemplate.width - 60
                    elide: Text.ElideRight
                    text: root.templateNameFor(root.editActiveFamilyKey)
                          + (root.editActiveFamilyOverridden ? "" : qsTr("  ·  inherited"))
                    color: root.editActiveFamilyOverridden ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // INHERIT/RESET chip: visible when the picked family carries
                // its OWN template; clicking clears it back to the whole-style
                // template. Clicking Change while inherited SETS the family's
                // own pick (the override begins).
                Rectangle {
                    id: resetFamilyTemplate
                    anchors.right: changeTemplate.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.editActiveFamilyOverridden
                    height: 26
                    width: resetFamilyTemplateLabel.implicitWidth + 22
                    radius: 13
                    color: resetFamilyTemplateArea.containsMouse ? Theme.chip : "transparent"
                    border.color: Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        id: resetFamilyTemplateLabel
                        anchors.centerIn: parent
                        text: qsTr("Inherit")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }

                    MouseArea {
                        id: resetFamilyTemplateArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (root.editTemplateFamily === "shows") root.editFamilyShows = ""
                            else if (root.editTemplateFamily === "media") root.editFamilyMedia = ""
                            else if (root.editTemplateFamily === "scripture") root.editFamilyScripture = ""
                            else root.editFamilyTable = ""
                        }
                    }
                }

                Rectangle {
                    id: changeTemplate   // referenced by the row's label width + the Inherit chip's anchor
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
                            // The picker targets the PICKED FAMILY: its pick
                            // lands in that family's own slot (an override
                            // begins), not the whole-style key.
                            templatePicker.contentType = root.editTemplateFamily
                            templatePicker.contentTypeLabel = {
                                "shows": qsTr("Shows"), "media": qsTr("Media"), "scripture": qsTr("Scripture"),
                                "table": qsTr("The Table")
                            }[root.editTemplateFamily] ?? qsTr("Shows")
                            // The ENGINE's template catalog — the same Template library the
                            // Templates tab edits — for EVERY content type (ReferencePane's
                            // mapping, category filter included). The style saves the design's
                            // id as its templateKey, and the engine renders that design as the
                            // style's layout (baked into the pushed spec). Legacy preset keys
                            // from older rosters still resolve through the picker's
                            // legacyPresetNames until re-picked here.
                            const categoryNames = {}
                            for (const c of TemplateLibraryService.categories)
                                categoryNames[c.id] = c.name
                            templatePicker.templates = TemplateLibraryService.designs().map((t) => ({
                                key: t.id, name: t.name, color: t.color,
                                category: t.category,
                                categoryName: t.category ? (categoryNames[t.category] ?? t.category) : qsTr("Unlabeled")
                            }))
                            templatePicker.selectedKey = root.editActiveFamilyKey
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
                        // Seeded with the style's CURRENT background — the
                        // palette highlights it instead of a hardcoded default.
                        onClicked: styleBgModal.openWith(root.editStyleBackgroundColor)
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
            // The pick lands in the PICKED FAMILY's own slot (a per-type
            // override); only a family-less default routes to the whole-style
            // key. "Inherit" on the row is the way back to the shared one.
            if (root.editTemplateFamily === "shows") root.editFamilyShows = tpl.key
            else if (root.editTemplateFamily === "media") root.editFamilyMedia = tpl.key
            else if (root.editTemplateFamily === "scripture") root.editFamilyScripture = tpl.key
            else root.editFamilyTable = tpl.key
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
