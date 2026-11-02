import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root

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
    function templateColorFor(key) {
        void root.templateCatalogRev
        if (typeof key === "string" && key.indexOf("tpl-") === 0) {
            const c = TemplateLibraryService.design(key).color
            if (c) return c
        }
        return ""
    }
    function layoutClassOf(key) { return ScreenHelpers.layoutClassOf(key) }
    property int contextStyleIndex: -1
    property string contextStyleId: ""
    property string contextStyleName: ""
    MenuCatcher {
        menu: styleContextMenu
    }
    DropdownPanel {
        id: styleContextMenu
        z: 50
        visible: false
        model: [
            { label: qsTr("Edit") },
            { label: qsTr("Duplicate") },
            { divider: true },
            { label: qsTr("Delete"), danger: true }
        ]
        onItemActivated: (label) => {
            styleContextMenu.visible = false
            if (root.contextStyleIndex < 0)
                return
            if (label === qsTr("Edit"))
                root.openEditStyle(root.contextStyleIndex)
            else if (label === qsTr("Duplicate"))
                StyleListModel.duplicateStyle(root.contextStyleIndex)
            else if (label === qsTr("Delete"))
                root.requestDeleteStyle(root.contextStyleIndex, root.contextStyleId,
                                        root.contextStyleName)
        }
    }

    property int deleteStyleIndex: -1
    property string deleteStyleName: ""
    property int deleteStyleUsers: 0
    property bool deleteStyleOnAir: false
    function requestDeleteStyle(index, styleId, name) {
        const users = root.styleUsage(styleId)
        if (users === 0) {
            StyleListModel.removeStyle(index)
            return
        }
        root.deleteStyleIndex = index
        root.deleteStyleName = name
        root.deleteStyleUsers = users
        root.deleteStyleOnAir = root.isOnAir(styleId)
        deleteStyleConfirm.open()
    }
    ConfirmDialog {
        id: deleteStyleConfirm
        title: qsTr("Delete “%1”?").arg(root.deleteStyleName)
        message: (root.deleteStyleUsers === 1
                      ? qsTr("1 output uses this style and will be set to None.")
                      : qsTr("%1 outputs use this style and will be set to None.").arg(root.deleteStyleUsers))
                 + (root.deleteStyleOnAir
                        ? "\n" + qsTr("It is ON AIR now — what is on screen will go blank.") : "")
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            if (root.deleteStyleIndex >= 0)
                StyleListModel.removeStyle(root.deleteStyleIndex)
            root.deleteStyleIndex = -1
            deleteStyleConfirm.close()
        }
        onDismissed: {
            root.deleteStyleIndex = -1
            deleteStyleConfirm.close()
        }
    }

    // Is this style ON the active output right now? (engine state, not the
    // roster's opinion — OutputListModel is the one that pushed the spec.)
    function isOnAir(styleId) {
        void root.modelsRev
        if (styleId === "" || !LiveOutputService.live)
            return false
        const rows = OutputListModel.rowCount()
        for (let i = 0; i < rows; ++i) {
            const out = OutputListModel.getOutput(i)
            if (out.styleId === styleId && out.isEnabled !== false)
                return true
        }
        return false
    }

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
    property int editStyleIndex: -1
    property string editStyleName: ""
    property string editStyleContentType: "shows"
    property string editStyleTemplateKey: ""
    property string editStyleBackgroundColor: "transparent"
    property string editStyleBackgroundImage: ""
    property bool editStyleClearOnText: false
    property bool editShowShows: false
    property bool editShowScripture: false
    property bool editShowTable: false
    property string editFamilyShows: ""
    property string editFamilyMedia: ""
    property string editFamilyScripture: ""
    property string editFamilyTable: ""
    property var editShowCategoryTemplates: []
    property string editCategoryTemplateTarget: ""
    // The family the Template row edits (chips switch it; defaults to the
    // style's whole-style family).
    property string editTemplateFamily: "shows"
    // Shows-only category (free text).
    property string editStyleCategory: ""
    readonly property string editActiveFamilyKey: {
        if (root.editTemplateFamily === "shows") return root.editFamilyShows
        if (root.editTemplateFamily === "media") return root.editFamilyMedia
        if (root.editTemplateFamily === "scripture") return root.editFamilyScripture
        return root.editFamilyTable
    }

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
        root.editShowScripture = data.showScripture
        root.editShowTable = data.showTable
        root.editFamilyShows = data.familyTemplateShows
        root.editFamilyMedia = data.familyTemplateMedia
        root.editFamilyScripture = data.familyTemplateScripture
        root.editFamilyTable = data.familyTemplateTable
        root.editShowCategoryTemplates = (data.showCategoryTemplates ?? []).map((entry) => ({
            category: String(entry.category ?? ""),
            templateKey: String(entry.templateKey ?? ""),
            overrideEnabled: !!entry.overrideEnabled
        }))
        const wanted = family || String(data.contentType || "")
        root.editTemplateFamily = (wanted === "shows" || wanted === "scripture"
                                   || wanted === "table") ? wanted : "shows"
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
        StyleListModel.setShowTemplate(root.editStyleIndex, "scripture", root.editShowScripture)
        StyleListModel.setShowTemplate(root.editStyleIndex, "table", root.editShowTable)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "shows", root.editFamilyShows)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "scripture", root.editFamilyScripture)
        StyleListModel.setFamilyTemplateKey(root.editStyleIndex, "table", root.editFamilyTable)
        StyleListModel.setCategory(root.editStyleIndex, root.editStyleCategory)
        StyleListModel.setShowCategoryTemplates(root.editStyleIndex, root.editShowCategoryTemplates)
        root.editStyleIndex = -1
    }

    function showCategoryTemplate(category) {
        return root.editShowCategoryTemplates.find((entry) => String(entry.category) === String(category))
            ?? { category: String(category), templateKey: "", overrideEnabled: false }
    }
    function setShowCategoryTemplate(category, templateKey, overrideEnabled) {
        const entries = root.editShowCategoryTemplates.filter((entry) => String(entry.category) !== String(category))
        entries.push({ category: String(category), templateKey: String(templateKey ?? ""),
                       overrideEnabled: !!overrideEnabled })
        root.editShowCategoryTemplates = entries
    }
    function openShowCategoryTemplatePicker(category) {
        const categoryNames = {}
        for (const c of TemplateLibraryService.categories) categoryNames[c.id] = c.name
        templatePicker.contentType = "shows"
        templatePicker.contentTypeLabel = qsTr("Shows")
        templatePicker.templates = TemplateLibraryService.designs().map((t) => ({
            key: t.id, name: t.name, color: t.color, category: t.category,
            categoryName: t.category ? (categoryNames[t.category] ?? t.category) : qsTr("Unlabeled")
        }))
        root.editCategoryTemplateTarget = String(category)
        templatePicker.selectedKey = root.showCategoryTemplate(category).templateKey
        templatePicker.open = true
    }

    function pickBackgroundImage() {
        Qt.callLater(root.applyPickedBackgroundImage)
    }
    function applyPickedBackgroundImage() {
        const path = StyleListModel.pickImageFile()
        if (path !== "")
            root.editStyleBackgroundImage = path
    }

    Flickable {
        id: flick
        objectName: "selfTestStylesFlick"
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.y + layout.height + Theme.space6   // the column's own top offset counts, or the last card sits on the window edge
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space6 * 2 + addStyleBtn.height
            width: flick.width - Theme.space6
            spacing: 16

            // ---- Styles card ----
            Rectangle {
                width: parent.width
                height: stylesCol.height + 28
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

                    Text {
                        visible: (root.modelsRev, StyleListModel.rowCount() === 0)
                        width: parent.width
                        text: qsTr("No styles yet.")
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
                            border.width: 1
                            border.color: Theme.borderSubtle

                            Rectangle {
                                id: swatch
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 40
                                height: 40
                                radius: Theme.radiusMd
                                clip: true
                                color: styleRow.isTransparent ? Theme.inset : styleRow.backgroundColor
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
                                            color: (Math.floor(index / 4) + (index % 4)) % 2 === 0 ? "#242633" : Theme.surface
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
                                        readonly property string layoutClass: {
                                            root.modelsRev
                                            root.templateCatalogRev
                                            const d = StyleListModel.getStyle(styleRow.index)
                                            const keys = [d.familyTemplateShows, d.familyTemplateMedia,
                                                          d.familyTemplateScripture, d.familyTemplateTable]
                                                         .filter(k => k !== "")
                                            if (keys.length === 0)
                                                keys.push(styleRow.templateKey)
                                            const kinds = new Set(keys.map(root.layoutClassOf).filter(c => c !== ""))
                                            return kinds.size === 0 ? "" : kinds.size > 1 ? "mixed" : Array.from(kinds)[0]
                                        }
                                        visible: layoutClass !== ""
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 18
                                        width: templateLabel.implicitWidth + 12
                                        radius: 4
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Theme.border

                                        Text {
                                            id: templateLabel
                                            anchors.centerIn: parent
                                            text: parent.layoutClass === "lower" ? qsTr("Lower Third")
                                                  : parent.layoutClass === "full" ? qsTr("Full Screen")
                                                  : qsTr("Mixed")
                                            color: Theme.textSecondary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }

                                    Rectangle {
                                        visible: root.isOnAir(styleRow.styleId)
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 18
                                        width: onAirLabel.implicitWidth + 12
                                        radius: 9
                                        color: Qt.alpha(Theme.liveBg, 0.2)
                                        border.color: Qt.alpha(Theme.liveBg, 0.4)
                                        border.width: 1

                                        Text {
                                            id: onAirLabel
                                            anchors.centerIn: parent
                                            text: qsTr("● ON AIR")
                                            color: Theme.dangerLight
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                        }
                                    }

                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 16

                                        Repeater {
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
                                                height: 20
                                                width: pillContent.implicitWidth
                                                radius: 0
                                                color: "transparent"
                                                border.width: 0
                                                opacity: modelData.on ? (pillHover.containsMouse ? 1 : 0.92) : 0.38

                                                Row {
                                                    id: pillContent
                                                    anchors.centerIn: parent
                                                    spacing: 6

                                                    Rectangle {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: 7
                                                        height: 7
                                                        radius: 3.5
                                                        color: familyPill.modelData.on ? familyPill.info.color : Theme.textMuted
                                                    }
                                                    Text {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        text: familyPill.info.label
                                                        color: familyPill.modelData.on ? Theme.textPrimary : Theme.textMuted
                                                        font.family: Theme.fontFamily
                                                        font.pixelSize: 12
                                                        font.weight: Font.Medium
                                                    }
                                                    Text {
                                                        visible: familyPill.overridden
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: Math.min(implicitWidth, 110)
                                                        elide: Text.ElideRight
                                                        text: familyPill.modelData.overrideName
                                                        color: Theme.textMuted
                                                        font.family: Theme.fontFamily
                                                        font.pixelSize: 11
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
                                        text: {
                                            root.modelsRev
                                            const u = root.styleUsage(styleRow.styleId)
                                            return "·  applied by " + u + (u === 1 ? " output" : " outputs")
                                        }
                                        color: root.isOnAir(styleRow.styleId) ? Theme.dangerLight : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: root.isOnAir(styleRow.styleId) ? Font.DemiBold : Font.Normal
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    root.contextStyleIndex = styleRow.index
                                    root.contextStyleId = styleRow.styleId
                                    root.contextStyleName = styleRow.name
                                    styleContextMenu.openAt(styleRow, mouse.x, mouse.y, root)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.space6 * 2 + addStyleBtn.height
        color: Theme.surface
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.verticalCenter: addStyleBtn.verticalCenter
        text: qsTr("Styles")
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXl
        font.weight: Font.DemiBold
    }

    Rectangle {
        id: addStyleBtn
        objectName: "selfTestAddStyle"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space6
        anchors.rightMargin: Theme.space6 + Theme.space2
        width: addStyleRow.width + Theme.space6 * 2
        height: 40
        radius: Theme.radiusMd
        color: addStyleArea.pressed ? Qt.darker(Theme.accent, 1.15)
             : addStyleArea.containsMouse ? Qt.lighter(Theme.accent, 1.1)
             : Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        Row {
            id: addStyleRow
            anchors.centerIn: parent
            spacing: 8

            Item {
                width: 11; height: 11
                anchors.verticalCenter: parent.verticalCenter
                Rectangle { anchors.centerIn: parent; width: 11; height: 1.6; radius: 0.8; color: Theme.controlInk }
                Rectangle { anchors.centerIn: parent; width: 1.6; height: 11; radius: 0.8; color: Theme.controlInk }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Add style")
                color: Theme.controlInk
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }

        MouseArea {
            id: addStyleArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: StyleListModel.addStyle()
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
    ModalCard {
        id: editStyleDialog
        shown: root.editStyleIndex >= 0
        title: qsTr("Edit Style")
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
                        { key: "scripture", label: qsTr("Scripture") },
                        { key: "table", label: qsTr("The Table") }
                    ]
                    delegate: SelectableChip {
                        required property var modelData
                        readonly property bool on: {
                            if (modelData.key === "shows") return root.editShowShows
                            if (modelData.key === "scripture") return root.editShowScripture
                            return root.editShowTable
                        }
                        label: modelData.label
                        selected: on
                        onPicked: {
                            const next = !on
                            if (modelData.key === "shows") root.editShowShows = next
                            else if (modelData.key === "scripture") root.editShowScripture = next
                            else root.editShowTable = next
                            if (next) {
                                root.editStyleContentType = modelData.key
                                root.editTemplateFamily = modelData.key
                            } else if (root.editTemplateFamily === modelData.key) {
                                const others = ["shows", "scripture", "table"]
                                    .filter((k) => k !== modelData.key
                                            && (k === "shows" ? root.editShowShows
                                                : k === "scripture" ? root.editShowScripture
                                                : root.editShowTable))
                                if (others.length > 0)
                                    root.editTemplateFamily = others[0]
                            }
                        }
                    }
                }
            }
        }

        SelectField {
            width: parent.width
            visible: root.editShowShows
            label: qsTr("Category (Shows only)")
            placeholder: qsTr("None")
            value: root.editStyleCategory
            options: [{ label: qsTr("None"), value: "" }].concat(
                ShowService.libraryCategories.map((c) => ({ label: c, value: c })))
            onValuePicked: (v) => root.editStyleCategory = v
        }

        Column {
            width: parent.width
            visible: root.editShowShows
            spacing: Theme.space2

            Text {
                text: qsTr("Shows templates by category")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Repeater {
                model: ShowService.libraryCategories
                delegate: Column {
                    required property string modelData
                    width: parent.width
                    spacing: 4
                    readonly property var entry: root.showCategoryTemplate(modelData)

                    Text {
                        text: modelData
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                    ValueRow {
                        width: parent.width
                        text: entry.templateKey === "" ? qsTr("No category template")
                                                         : root.templateNameFor(entry.templateKey)
                        dim: entry.templateKey === ""
                        PillButton {
                            visible: entry.templateKey !== ""
                            text: qsTr("Clear")
                            tone: "muted"
                            onClicked: root.setShowCategoryTemplate(modelData, "", false)
                        }
                        PillButton {
                            text: entry.templateKey === "" ? qsTr("Choose") : qsTr("Change")
                            onClicked: root.openShowCategoryTemplatePicker(modelData)
                        }
                    }
                    PillButton {
                        visible: entry.templateKey !== ""
                        text: entry.overrideEnabled ? qsTr("Override on") : qsTr("Override")
                        tone: entry.overrideEnabled ? "accent" : "muted"
                        onClicked: root.setShowCategoryTemplate(modelData, entry.templateKey,
                                                                !entry.overrideEnabled)
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                // Media is not in this map: it has no template, and its
                // family is no longer one the chips can target.
                text: qsTr("Template for ") + ({
                    "shows": qsTr("Shows"),
                    "scripture": qsTr("Scripture"), "table": qsTr("The Table")
                }[root.editTemplateFamily] ?? qsTr("Shows"))
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            ValueRow {
                width: parent.width
                text: root.editActiveFamilyKey === ""
                      ? qsTr("None") : root.templateNameFor(root.editActiveFamilyKey)
                dim: root.editActiveFamilyKey === ""

                PillButton {
                    visible: root.editActiveFamilyKey !== ""
                    text: qsTr("Clear")
                    tone: "muted"
                    onClicked: {
                        if (root.editTemplateFamily === "shows") root.editFamilyShows = ""
                        else if (root.editTemplateFamily === "media") root.editFamilyMedia = ""
                        else if (root.editTemplateFamily === "scripture") root.editFamilyScripture = ""
                        else root.editFamilyTable = ""
                    }
                }

                PillButton {
                    text: qsTr("Change")
                    onClicked: {
                        root.editCategoryTemplateTarget = ""
                        // The picker targets the PICKED FAMILY: its pick lands in that
                        // family's own slot (an override begins), not the whole-style key.
                        templatePicker.contentType = root.editTemplateFamily
                        templatePicker.contentTypeLabel = {
                            "shows": qsTr("Shows"), "media": qsTr("Media"), "scripture": qsTr("Scripture"),
                            "table": qsTr("The Table")
                        }[root.editTemplateFamily] ?? qsTr("Shows")
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

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Background")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            ValueRow {
                width: parent.width
                hasSwatch: true
                swatchColor: root.editStyleBackgroundColor === "transparent"
                             ? Theme.inset : root.editStyleBackgroundColor
                text: root.editStyleBackgroundColor === "transparent"
                      ? qsTr("Transparent") : root.editStyleBackgroundColor
                dim: root.editStyleBackgroundColor === "transparent"

                // Seeded with the style's CURRENT background — the palette highlights
                // it instead of a hardcoded default.
                PillButton {
                    text: qsTr("Change")
                    onClicked: styleBgModal.openWith(root.editStyleBackgroundColor)
                }
            }

            Column {
                width: parent.width
                spacing: Theme.space2

                Text {
                    text: qsTr("Background image")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }

                ValueRow {
                    width: parent.width
                    hasSwatch: true
                    swatchColor: Theme.card
                    swatchImage: root.editStyleBackgroundImage === ""
                                 ? "" : "file:///" + root.editStyleBackgroundImage
                    text: root.editStyleBackgroundImage === ""
                          ? qsTr("None")
                          : root.editStyleBackgroundImage.split("/").pop()
                    dim: root.editStyleBackgroundImage === ""

                    PillButton {
                        visible: root.editStyleBackgroundImage !== ""
                        text: qsTr("Remove")
                        tone: "muted"
                        onClicked: root.editStyleBackgroundImage = ""
                    }
                    PillButton {
                        text: root.editStyleBackgroundImage === "" ? qsTr("Pick") : qsTr("Change")
                        onClicked: root.pickBackgroundImage()
                    }
                }
            }

            Item {
                width: parent.width
                height: 34

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Let slides keep their own background")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                SettingsToggle {
                    id: clearOnTextToggle
                    anchors.right: parent.right
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
            if (root.editCategoryTemplateTarget !== "") {
                const category = root.editCategoryTemplateTarget
                const previous = root.showCategoryTemplate(category)
                root.setShowCategoryTemplate(category, tpl.key, previous.overrideEnabled)
                root.editCategoryTemplateTarget = ""
            } else {
                // The pick lands in the PICKED FAMILY's own slot — the only
                // place a general family template choice lives.
                if (root.editTemplateFamily === "shows") root.editFamilyShows = tpl.key
                else if (root.editTemplateFamily === "media") root.editFamilyMedia = tpl.key
                else if (root.editTemplateFamily === "scripture") root.editFamilyScripture = tpl.key
                else root.editFamilyTable = tpl.key
            }
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