import QtQuick
import QtQuick.Controls

// "Choose template" — opens from EditStyleModal's Template row ("Change"),
// matching 1BBTIwaya/VGRPresenter_Settings_Outputs_Edit_Template_Pick.qml's
// picker_dialog.png reference (that file ships only flat PNGs, no element-
// level QML — rebuilt here as real, model-driven QML like every other
// picker in this app).
//
// `templates` is the one canonical registry of layout templates — both this
// picker and EditStyleModal's own Template-row summary read from it (the
// latter to resolve a stored templateKey back to a display name), so there
// is exactly one place template metadata lives, not two lists to keep in
// sync.
//
// Favorites are session-only (not persisted to StyleListModel or disk) —
// a real "starred templates" feature would need its own storage; this is
// just enough for the All/Favorites tabs to be genuinely functional today.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false
    // "shows" | "media" | "scripture" — which category this picker is
    // filtering for, set by the consumer right before opening.
    property string contentType: "shows"
    property string contentTypeLabel: "Shows"
    property string selectedKey: "lowerThird"

    // Just key + name — a template's NAME is what identifies it; it
    // doesn't need a separate description/tag pulling double duty saying
    // the same thing a different way. The preview pane (below) reads the
    // name generically too, not a per-key hardcoded layout.
    // Settable (not readonly): every consumer now hands the ENGINE's template
    // catalog in (StylesScreen included — its old default was a hardcoded
    // five-preset list that had nothing to do with the Template library the
    // rest of the app edits, and the engine's LayoutFor still accepts those
    // five keys as legacy presets). The list shape: { key, name, color?,
    // category?, categoryName? } — the extra fields light the category
    // dropdown + colour chip the catalog rows carry.
    property var templates: []

    // Looks up a template's display name for a stored key — what
    // EditStyleModal calls to show "Template: <name>" without needing its
    // own copy of the registry.
    function nameFor(key) {
        return root.templateFor(key).name
    }
    function templateFor(key) {
        for (let i = 0; i < root.templates.length; ++i) {
            if (root.templates[i].key === key)
                return root.templates[i]
        }
        // Not in the handed-in list. Two honest fallbacks: a legacy preset
        // key ("lowerThird"...) — the engine's built-in layouts, which are
        // real choices even though no design carries them — displays as its
        // own name; anything else resolves to an empty row, NEVER to
        // templates[0] (that silently renamed every unknown key to the first
        // template's name, and with the engine catalog handed in it also
        // crashed on an empty list before the catalog loaded).
        if (typeof key === "string" && key.length > 0)
            return { key: key, name: legacyPresetNames[key] ?? key }
        return { key: "", name: "" }
    }

    // The engine's built-in layout presets (StyleBuilder::LayoutFor's five
    // keys) — styles saved before the engine catalog was connected, and any
    // "legacy preset" choice, still carry these keys.
    readonly property var legacyPresetNames: ({
        lowerThird: qsTr("Lower Third"),
        title: qsTr("Title"),
        sidebar: qsTr("Sidebar"),
        bottomBar: qsTr("Bottom Bar"),
        fullscreen: qsTr("Fullscreen")
    })

    property var favoriteKeys: ["lowerThird", "sidebar"]
    function isFavorite(key) {
        return root.favoriteKeys.indexOf(key) >= 0
    }
    function toggleFavorite(key) {
        const i = root.favoriteKeys.indexOf(key)
        if (i >= 0) {
            const next = root.favoriteKeys.slice()
            next.splice(i, 1)
            root.favoriteKeys = next
        } else {
            root.favoriteKeys = root.favoriteKeys.concat([key])
        }
    }

    property string searchQuery: ""
    // "" = every category (the default: nothing here filters out a template until asked to). A consumer whose templates carry
    // `category`/`categoryName` (Scripture / The Table's engine catalog does) gets a dropdown of the ones actually present in
    // `templates`; one that doesn't (Settings' fixed layouts, with no such fields) just never shows the dropdown.
    property string categoryFilter: ""
    readonly property var categoryOptions: {
        const seen = {}
        const out = [{ id: "", name: qsTr("All categories") }]
        for (const t of root.templates) {
            if (!t.category || seen[t.category]) continue
            seen[t.category] = true
            out.push({ id: t.category, name: t.categoryName ?? t.category })
        }
        if (out.length > 1) out.sort((a, b) => a.id === "" ? -1 : b.id === "" ? 1 : a.name.localeCompare(b.name))
        return out
    }
    readonly property bool hasCategoryFilter: root.categoryOptions.length > 1
    readonly property string categoryFilterName: {
        for (const c of root.categoryOptions) if (c.id === root.categoryFilter) return c.name
        return qsTr("All categories")
    }

    // Visible only once there is something to narrow by - a search, or a chosen category - not a permanent list sitting open by
    // default; the dropdowns (category, and typing here) are what reveal it, same idea as a combobox's own results list.
    readonly property bool showResults: root.searchQuery.length > 0 || root.categoryFilter !== ""

    readonly property var filteredTemplates: root.templates.filter((t) => {
        if (root.categoryFilter !== "" && t.category !== root.categoryFilter)
            return false
        if (root.searchQuery.length > 0 && t.name.toLowerCase().indexOf(root.searchQuery.toLowerCase()) < 0)
            return false
        return true
    })

    readonly property var previewTemplate: root.templateFor(root.selectedKey)

    // Fired when "Use template" is clicked, carrying the picked template
    // object ({ key, name, description, tag }).
    signal applied(var template)
    signal cancelled()

    // Whoever instantiates this (a Settings screen filling the whole window, or - like The Table/Scripture tab - a pane nested under
    // the app's own persistent top bar and tab row) only hands it a LOCAL parent. Reparented to the ApplicationWindow's own overlay
    // layer instead, so `anchors.fill: parent` truly fills the whole window and the card centers on the whole app, not just
    // whatever smaller area its caller happens to occupy.
    parent: Overlay.overlay
    anchors.fill: parent
    visible: root.open

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    MenuCatcher { menu: categoryMenu }
    DropdownPanel {
        id: categoryMenu
        visible: false
        z: 25
        maxHeight: 280
        onItemActivated: (label) => {
            categoryMenu.visible = false
            const chosen = root.categoryOptions.find((c) => c.name === label)
            if (chosen) root.categoryFilter = chosen.id
        }
    }

    // A soft drop shadow behind the card - stacked, growing, fading rectangles (this app's own stand-in for a real blur effect,
    // the same trick its other floating panels already use), so the card visibly lifts off the dimmed backdrop instead of just
    // sitting flush with it.
    Rectangle {
        anchors.centerIn: card
        anchors.verticalCenterOffset: 10
        width: card.width + 24; height: card.height + 24; radius: card.radius + 6
        color: "#20000000"
    }
    Rectangle {
        anchors.centerIn: card
        anchors.verticalCenterOffset: 5
        width: card.width + 10; height: card.height + 10; radius: card.radius + 3
        color: "#30000000"
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 700
        // At least the header + a couple of rows + the footer, however little room a cramped parent (a tab's own content area, not
        // the full window Settings gets) leaves - short of that, the body below scrolls instead of the footer buttons being clipped
        // off with no way to reach them.
        height: Math.min(Math.max(360, parent.height - 40), content.height + 40)
        radius: 14
        color: "#13151c"
        border.color: "#232530"
        border.width: 1
        clip: true

        MouseArea { anchors.fill: parent; onClicked: {} }

        Flickable {
            id: pickerFlick
            anchors.fill: parent
            anchors.rightMargin: 10
            contentWidth: width
            contentHeight: content.height + 40
            clip: true
            boundsBehavior: Flickable.StopAtBounds

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
                        text: qsTr("Choose template")
                        color: "#f1f3f8"
                        font.family: "Segoe UI"
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        radius: 7
                        color: closeArea.containsMouse ? "#20222c" : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: "#8a94a6"
                            font.pixelSize: 14
                        }

                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Row {
                    spacing: 10

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        height: 20
                        width: catLabel.implicitWidth + 16
                        radius: 4
                        color: "#2a1c24"

                        Text {
                            id: catLabel
                            anchors.centerIn: parent
                            text: root.contentTypeLabel.toUpperCase() + " · " + root.templates.length
                            color: Theme.accent
                            font.family: "Segoe UI"
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Templates for ") + root.contentTypeLabel.toLowerCase()
                        color: "#8a94a6"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            Row {
                width: parent.width
                spacing: 20

                // ---- Preview pane ----
                Column {
                    width: (parent.width - 20) * 0.42
                    spacing: 8

                    Text {
                        text: qsTr("Preview")
                        color: "#5c6475"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                    }

                    Rectangle {
                        id: previewStage
                        width: parent.width
                        height: width * 9 / 16
                        radius: 8
                        color: "#0a0b0f"
                        border.color: "#262a38"
                        border.width: 1
                        clip: true

                        // The engine's own design for the selected key, when it has one (Scripture / The Table's real templates
                        // do) - the SAME renderer the Templates library's own cards and the Edit screen's slide thumbnails use, so
                        // this shows the template's real layout, not a placeholder. A caller with no such thing (Settings' fixed,
                        // not-yet-engine-backed layouts) falls back to the plain name-only visual underneath it.
                        readonly property var liveDesign: TemplateLibraryService.design(root.selectedKey)
                        readonly property bool hasRealPreview: previewStage.liveDesign.blocks !== undefined && previewStage.liveDesign.blocks.length > 0

                        DesignPreview {
                            anchors.fill: parent
                            visible: previewStage.hasRealPreview
                            blocks: previewStage.hasRealPreview ? previewStage.liveDesign.blocks : []
                            background: previewStage.liveDesign.background !== undefined ? previewStage.liveDesign.background : "transparent"
                        }

                        Text {
                            visible: !previewStage.hasRealPreview
                            anchors.centerIn: parent
                            text: root.previewTemplate.name
                            color: root.previewTemplate.color || "#9b8ff5"
                            font.family: "Segoe UI"
                            font.pixelSize: 21
                            font.weight: Font.DemiBold
                        }
                    }

                    Text {
                        text: root.previewTemplate.name
                        color: "#eef1f8"
                        font.family: "Segoe UI"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        height: 20
                        width: aspectLabel.implicitWidth + 14
                        radius: 4
                        color: "#1a1c26"
                        border.color: "#2a2f3a"
                        border.width: 1
                        Text { id: aspectLabel; anchors.centerIn: parent; text: "16:9"; color: "#8a94a6"; font.family: "Segoe UI"; font.pixelSize: 10 }
                    }
                }

                // ---- Template list ----
                Column {
                    width: parent.width - (parent.width - 20) * 0.42 - 20
                    spacing: 8

                    // Category, above Templates: which one is picked decides what Templates below even has to show.
                    Text {
                        visible: root.hasCategoryFilter
                        text: qsTr("Category")
                        color: "#5c6475"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                    }

                    // The category filter - only for a consumer whose templates carry more than one category. Defaults to "All
                    // categories" (nothing filtered out); picking one narrows the list to it. Click opens the dropdown.
                    Rectangle {
                        id: categoryBtn
                        visible: root.hasCategoryFilter
                        height: 32
                        width: parent.width
                        radius: 8
                        color: "#1a1c26"
                        border.color: "#2a2f3a"
                        border.width: 1

                        Text {
                            id: categoryLabel
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.categoryFilterName
                            color: "#c9cedd"
                            font.family: "Segoe UI"
                            font.pixelSize: 14
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: "▾"
                            color: "#5c6475"
                            font.pixelSize: 10
                        }

                        MouseArea {
                            id: categoryArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                categoryMenu.model = root.categoryOptions.map((c) => ({ label: c.name, trailing: c.id === root.categoryFilter ? "✓" : "" }))
                                categoryMenu.openAt(categoryBtn, 0, categoryBtn.height + 4, root)
                            }
                        }
                    }

                    Text {
                        text: qsTr("Templates")
                        color: "#5c6475"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                    }

                    Rectangle {
                        width: parent.width
                        height: 32
                        radius: 8
                        color: "#1a1c26"
                        border.color: "#2a2f3a"
                        border.width: 1

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            IconGlyph { anchors.verticalCenter: parent.verticalCenter; name: "search"; color: "#5c6475"; width: 12; height: 12 }

                            TextInput {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 24
                                color: "#c9cedd"
                                font.family: "Segoe UI"
                                font.pixelSize: 14
                                selectByMouse: true
                                onTextChanged: root.searchQuery = text

                                Text {
                                    visible: parent.text.length === 0
                                    text: qsTr("Search templates")
                                    color: "#5c6475"
                                    font.family: "Segoe UI"
                                    font.pixelSize: 14
                                }
                            }
                        }
                    }

                    Text {
                        visible: !root.showResults
                        width: parent.width
                        text: root.hasCategoryFilter
                              ? qsTr("Type to search, or pick a category, to see templates here.")
                              : qsTr("Type to search for a template.")
                        color: "#5c6475"
                        wrapMode: Text.Wrap
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                    }

                    Row {
                        visible: root.showResults
                        width: parent.width
                        height: Math.min(7, root.filteredTemplates.length) * 58
                        spacing: 6

                    Flickable {
                        id: listFlick
                        width: parent.width - listScrollBar.width - 6
                        height: parent.height
                        contentWidth: width
                        contentHeight: listCol.height
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        Column {
                            id: listCol
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: root.filteredTemplates

                                delegate: Rectangle {
                                    id: tplRow
                                    required property var modelData
                                    readonly property bool selected: root.selectedKey === tplRow.modelData.key

                                    width: listCol.width
                                    height: 52
                                    radius: 8
                                    color: tplRow.selected ? "#1a2240" : (rowArea.containsMouse ? "#191b24" : "#161823")
                                    border.width: tplRow.selected ? 1.5 : 1
                                    border.color: tplRow.selected ? Theme.accent : "#262a38"
                                    Behavior on color { ColorAnimation { duration: 100 } }

                                    // A plain generic mark, not a redundant tiny render of the design - the actual preview is the
                                    // big pane on the left; a 36px render of it here read as an unexplained blob of colour, not a
                                    // thumbnail anyone could read.
                                    Rectangle {
                                        x: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 36
                                        height: 36
                                        radius: 5
                                        color: "#0d0f16"
                                        border.color: "#262a38"
                                        border.width: 1

                                        // layoutTemplate is a hand-sized (non-24-grid) glyph — its
                                        // 9x9 native box has more empty margin than the grid24
                                        // icons IconGlyph can scale with `fit`. Explicitly centering
                                        // it in an Item exactly its own size (rather than leaving
                                        // it inside a bigger 16x16 IconGlyph box) removes that
                                        // extra margin so it reads centered instead of adrift in
                                        // the corner of its own bounding box.
                                        Item {
                                            anchors.centerIn: parent
                                            width: 18; height: 18
                                            IconGlyph {
                                                anchors.centerIn: parent
                                                name: "layoutTemplate"
                                                color: "#8a94a6"
                                                width: 18; height: 18
                                            }
                                        }
                                    }

                                    Text {
                                        x: 58
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 58 - 36
                                        text: tplRow.modelData.name
                                        color: "#eef1f8"
                                        font.family: "Segoe UI"
                                        font.pixelSize: 15
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        anchors.right: parent.right
                                        anchors.rightMargin: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.isFavorite(tplRow.modelData.key) ? "★" : "☆"
                                        color: root.isFavorite(tplRow.modelData.key) ? "#f5c26b" : "#5c6475"
                                        font.pixelSize: 15

                                        MouseArea {
                                            anchors.fill: parent
                                            anchors.margins: -6
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.toggleFavorite(tplRow.modelData.key)
                                        }
                                    }

                                    MouseArea {
                                        id: rowArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.selectedKey = tplRow.modelData.key
                                    }
                                }
                            }
                        }
                    }

                    AppScrollBar {
                        id: listScrollBar
                        flickable: listFlick
                        height: listFlick.height
                    }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 90
                    height: 34
                    radius: 9
                    color: cancelArea.containsMouse ? "#20222c" : "#1a1c26"
                    border.color: "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#c8cdd9"
                        font.family: "Segoe UI"
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
                    width: 120
                    height: 34
                    radius: 9
                    color: useArea.containsMouse ? Theme.accentLight : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Use template")
                        color: "#ffffff"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: useArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied(root.templateFor(root.selectedKey))
                    }
                }
            }
        }
        }

        // The catalog (now the WHOLE template library) overflows the modal on
        // a short window — wheel-only before. Sits in the rightMargin strip
        // the Flickable reserves.
        AppScrollBar {
            flickable: pickerFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 8; anchors.bottomMargin: 8
            anchors.right: parent.right; anchors.rightMargin: 2
        }
    }
}
