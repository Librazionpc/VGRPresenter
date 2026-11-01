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
    // WHAT KIND OF LAYOUT A TEMPLATE IS — "full" or "lower" — from where its main text
    // block sits on the stage: a block in the bottom half that is no taller than half the
    // stage is a lower third; anything else fills the screen. (Measured on the shipped
    // and saved designs: full-screen ones put their text at 0-8% from the top and 83-100%
    // tall; lower thirds at 70%+ from the top and 10-29% tall, so nothing sits near the
    // line.) A legacy preset key has no design to measure: "lowerThird" is a lower third,
    // any other is unknown. "" = no layout to speak of.
    function layoutClassOf(key) {
        if (typeof key !== "string" || key === "")
            return ""
        if (key.indexOf("tpl-") !== 0)
            return key === "lowerThird" ? "lower" : ""
        const blocks = TemplateLibraryService.design(key).blocks ?? []
        let best = null
        let bestArea = 0
        for (let i = 0; i < blocks.length; ++i) {
            const b = blocks[i]
            if (b.kind !== undefined && b.kind !== "text")
                continue
            const area = Number(b.width) * Number(b.height)
            if (area > bestArea) {
                bestArea = area
                best = b
            }
        }
        if (!best)
            return ""
        return (Number(best.y) >= StageGrid.h * 0.5 && Number(best.height) <= StageGrid.h * 0.5)
               ? "lower" : "full"
    }
    // DELETE A STYLE: straight away when nothing wears it, after a confirmation when
    // outputs do (they go back to "None"; the one on air is named, because deleting its
    // style blanks what is on screen).
    // The row the right-click menu was opened on.
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
        // EVERY ENABLED OUTPUT THAT WEARS THE STYLE, while the show is live — not only
        // the one output the app has selected. It used to answer for the selected output
        // alone, so two styles worn by two enabled outputs (the HDMI screen and the NDI
        // lower third) read differently — one "ON AIR", the other not — and it said
        // "ON AIR" for the selected output's style even when nothing was live.
        const rows = OutputListModel.rowCount()
        for (let i = 0; i < rows; ++i) {
            const out = OutputListModel.getOutput(i)
            if (out.styleId === styleId && out.isEnabled !== false)
                return true
        }
        return false
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
    property string editStyleTemplateKey: ""
    property string editStyleBackgroundColor: "transparent"
    property string editStyleBackgroundImage: ""
    property bool editStyleClearOnText: false
    // The three content chips (shows/scripture/table) — buffer the EDITED flags,
    // loaded from the row on open. The dialog starts them INACTIVE so a brand-new
    // style's chips read as an empty slate (the row itself carries the same
    // default; the open overwrites these anyway).
    //
    // MEDIA AND OVERLAY ARE NOT HERE ANY MORE. Those two gates moved to the
    // Routing canvas — the Overlays and Media cards at the head of the STYLES
    // column — because they are allow-list facts about an output, and the canvas
    // is where every other allow-list already lives. Their values still ride this
    // dialog's save path untouched (nothing here writes them), so a style's
    // stored gates are never clobbered by opening its editor.
    property bool editShowShows: false
    property bool editShowScripture: false
    property bool editShowTable: false
    // PER-FAMILY template picks — each active content type carries its OWN
    // template ("" = none for that family). NO INHERIT: a family only ever
    // renders a template picked for IT (Shows alone falls back to the Bible
    // template, baked engine-side).
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
    // The picked family's template key: its own pick when it has one, else
    // the whole-style key (what the Template row shows and edits).
    // The picked family's OWN template key ("" = no template for it — the
    // row shows None). No inherit: a family never falls back to the
    // whole-style key.
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
        // A clicked card pill pre-targets ITS family in the dialog. MEDIA has no
        // template (its gate moved to Routing, and media never wore a family
        // template), so a media style opens on Shows rather than a family the
        // Template row cannot edit.
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
        // The MEDIA and OVERLAY gates are NOT written here: they belong to the
        // Routing canvas's two gate cards now, and writing a value this dialog
        // never edited is how a stale buffer would silently undo a card click.
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

    // Native file picker for the style background image (same engine dialogs
    // ShowService/ScriptureService use). Non-image files simply fail to
    // decode later and the colour shows instead — a second dialog would be
    // kinder but this keeps the flow one click.
    //
    // DEFERRED on purpose (fixed 2026-10-10 — the reported crash "the app
    // crashes when i update the styles and go back to output"): the picker is a
    // NATIVE modal dialog, so it runs its own nested event loop inside this
    // call. Run straight from the button's click handler, that handler sat on
    // the stack for the whole loop, and anything the loop tears down takes the
    // handler's own object with it — engine.log carried exactly that verdict:
    // "Object 0x… destroyed while one of its QML signal handlers is in
    // progress … StylesScreen.qml:1166: function() { [native code] }", one
    // line before the process died of an access violation. A section switch
    // (Settings · Outputs) destroys this page, and the section Loader can run
    // while the nested loop is still pumping events. Through Qt.callLater the
    // click handler has already RETURNED when the picker opens, so there is no
    // QML handler for the teardown to collide with — and if this page is gone
    // by the time the deferred call comes round, Qt simply drops it instead of
    // writing to a dead object.
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
            // Below the fixed title/Add-style header (pinned to root, same
            // as Outputs' own header) so the styles card flows underneath
            // it instead of scrolling the title away with it.
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

                    // Empty state — the roster starts empty by design.
                    // root.modelsRev is read (not used) purely to establish
                    // a reactive dependency — rowCount() is a plain
                    // Q_INVOKABLE call, so this wouldn't otherwise notice a
                    // style being added or removed (see modelsRev's own
                    // header comment).
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
                            border.width: 1
                            border.color: Theme.borderSubtle

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

                                // Inline-editable name — looks like a label,
                                // commits on Enter / focus loss. Quick
                                // renames stay inline; content type/template/
                                // background need the full Edit Style dialog
                                // (right-click the row, then Edit).
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

                                    // (No content-type chip any more: it repeated one of the
                                    // family pills below under a different colour and showed a
                                    // different family on every row — "The Table" here,
                                    // "Shows" there. The pills already say, for every family,
                                    // whether the style serves it and with which template.)
                                    // The template as a CHIP wearing the ENGINE
                                    // design's own accent (its category colour),
                                    // not a grey text label — the row now reads
                                    // "this exact engine design" at a glance.
                                    // THE KIND OF LAYOUT THIS STYLE IS, not a template name: "Full Screen"
                                    // when the templates it uses fill the screen (Big Bold, Scripture,
                                    // Table), "Lower Third" when they sit in the bottom band, "Mixed"
                                    // when they disagree, and no chip when it has none. Worked out from
                                    // where each template puts its text (layoutClassOf), so it follows
                                    // the templates instead of a stored preset name that no longer
                                    // meant anything (every seeded style carried "Lower Third").
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

                                    // ON AIR — engine truth (this style is the
                                    // active output's): a live-red chip, not a
                                    // dot the eye has to hunt for.
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
                                        spacing: 16

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
                                                // QUIET, not boxed: a coloured dot and the family's name, with its
                                                // template in muted text. Four filled, bordered pills per row were the
                                                // loudest thing on the page.
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
                                        color: root.isOnAir(styleRow.styleId) ? Theme.dangerLight : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: root.isOnAir(styleRow.styleId) ? Font.DemiBold : Font.Normal
                                    }
                                }
                            }

                            // RIGHT-CLICK MENU (Edit / Duplicate / Delete) — the same drop-down
                            // the Screens list and the slide rows use, so the row keeps the whole
                            // width for its pills instead of sharing it with three buttons.
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

    // The header's own ground — without this, a scrolled-up card would show
    // straight through the pinned title/button (see Outputs' own header,
    // which this mirrors).
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.space6 * 2 + addStyleBtn.height
        color: Theme.surface
    }

    // "Styles" title — pinned alongside the Add-style button (sits above
    // the Flickable in paint order, same as that button) so the header
    // stays put while the roster scrolls underneath it.
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

    // "+ Add style" pinned top-right — always visible no matter how far the
    // roster scrolls. Same drawn-plus-glyph button Outputs uses, not the
    // ghost AppButton this used to be (that lived inside the card as a
    // secondary action; pinned beside the title it is the page's one
    // primary call-to-action, same as "+ Add Output").
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
        //
        // MEDIA AND OVERLAY LEFT THIS ROW: they are gates an OUTPUT reads, so
        // they moved to Settings · Routing's two gate cards at the top of the
        // STYLES column (Overlays, Media) — one allow-list, one place to read it.
        // Media has no template, so its family is not one the Template row edits.
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

        // SHOWS-ONLY CATEGORY — a dropdown over the shows library's own
        // categories (was free text: a typo silently filed a style under a
        // category that never matched any real show). "None" clears it.
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

        // TEMPLATE — PER CONTENT TYPE: the row edits the family picked by the
        // chips above and shows THAT family's own pick (None when it has
        // none). Picking sets the family's own template; the None chip
        // clears it back to no template (Shows then falls back to the Bible
        // template).
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

                // Clear: visible when the picked family carries its own template;
                // clicking drops it back to no template. Picking via Change sets the
                // family's own pick. Every styling choice is explicit — nothing is
                // inherited.
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

            // FreeShow's clearStyleBackgroundOnText: slides that carry their own
            // background keep it when this style is on air; plain text slides
            // get the style's background. SettingsToggle is the pill only —
            // The dialog owns the state; the pill reports its value.
            // FreeShow's clearStyleBackgroundOnText toggle: the dialog owns the state; the
            // pill reports its value.
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
