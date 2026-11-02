import QtQuick
import QtQuick.Controls
// For the app's Hotkeys singleton (Esc = cancel, Enter = apply) — this file
// draws its own literal chrome, but the keys are the app's.
import VGRPresenterUI

Item {
    id: root
    z: 31000

    property bool open: false

    // ---- Esc = cancel, Enter = the Apply button (qml/settings/Hotkeys.qml) --
    property int hotkeyScope: 0
    function accept() { root.applied(root.templateFor(root.selectedKey)) }
    onOpenChanged: {
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Choose template"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    // "shows" | "media" | "scripture" — which category this picker is
    // filtering for, set by the consumer right before opening.
    property string contentType: "shows"
    property string contentTypeLabel: "Shows"
    property string selectedKey: "lowerThird"

    property var templates: []

    function nameFor(key) {
        return root.templateFor(key).name
    }
    function templateFor(key) {
        for (let i = 0; i < root.templates.length; ++i) {
            if (root.templates[i].key === key)
                return root.templates[i]
        }
        if (typeof key === "string" && key.length > 0)
            return { key: key, name: legacyPresetNames[key] ?? key }
        return { key: "", name: "" }
    }

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

    parent: Overlay.overlay
    anchors.fill: parent
    visible: root.open

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        anchors.centerIn: card
        anchors.verticalCenterOffset: 10
        width: card.width + 24; height: card.height + 24; radius: card.radius + 6
        color: "#20000000"
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
    }
    Rectangle {
        anchors.centerIn: card
        anchors.verticalCenterOffset: 5
        width: card.width + 10; height: card.height + 10; radius: card.radius + 3
        color: "#30000000"
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        width: 700
        height: Math.min(Math.max(360, parent.height - 40), content.height + 40)
        radius: 14
        color: Theme.surface
        border.color: Theme.border
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
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXl
                        font.weight: Font.DemiBold
                    }

                    IconGlyph {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        name: "close"
                        color: Theme.textMuted
                        implicitWidth: 16
                        implicitHeight: 16

                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -8
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Row {
                    spacing: 10

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        height: 22
                        width: catLabel.implicitWidth + 16
                        radius: Theme.radiusSm
                        color: Qt.alpha(Theme.accent, 0.16)

                        Text {
                            id: catLabel
                            anchors.centerIn: parent
                            text: root.contentTypeLabel.toUpperCase() + " · " + root.templates.length
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            font.letterSpacing: 0.5
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Templates for ") + root.contentTypeLabel.toLowerCase()
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                width: parent.width
                spacing: 20

                // ---- Preview pane ----
                Column {
                    width: (parent.width - 20) * 0.42
                    spacing: 8

                    Text {
                        text: qsTr("Preview")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }

                    Rectangle {
                        id: previewStage
                        width: parent.width
                        height: width * 9 / 16
                        radius: 8
                        color: Theme.windowBg
                        border.color: Theme.borderSubtle
                        border.width: 1
                        clip: true

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
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXl
                            font.weight: Font.DemiBold
                        }
                    }

                    Text {
                        text: root.previewTemplate.name
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        height: 20
                        width: aspectLabel.implicitWidth + 14
                        radius: 4
                        color: Theme.inset
                        border.color: Theme.toggleOffTrack
                        border.width: 1
                        Text { id: aspectLabel; anchors.centerIn: parent; text: "16:9"; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 10 }
                    }
                }

                // ---- Template list ----
                Column {
                    width: parent.width - (parent.width - 20) * 0.42 - 20
                    spacing: 8

                    SelectField {
                        visible: root.hasCategoryFilter
                        width: parent.width
                        label: qsTr("Category")
                        value: root.categoryFilterName
                        options: root.categoryOptions.map((c) => ({ label: c.name, value: c.id }))
                        onValuePicked: (v) => root.categoryFilter = v
                    }

                    Text {
                        text: qsTr("Templates")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }

                    Rectangle {
                        width: parent.width
                        height: 36
                        radius: Theme.radiusMd
                        color: Theme.inset
                        border.color: searchInput.activeFocus ? Theme.accent : Theme.border
                        border.width: 1
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        IconGlyph {
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            name: "search"
                            color: Theme.textMuted
                            width: 13; height: 13
                        }

                        TextInput {
                            id: searchInput
                            x: 34
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 34 - 12
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            selectByMouse: true
                            clip: true
                            onTextChanged: root.searchQuery = text

                            Text {
                                visible: parent.text.length === 0
                                text: qsTr("Search templates")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }

                    // Nothing to show yet: a quiet empty state, the same inset surface as the fields.
                    Rectangle {
                        visible: !root.showResults
                        width: parent.width
                        height: 96
                        radius: Theme.radiusMd
                        color: Theme.inset
                        border.width: 1
                        border.color: Theme.borderSubtle

                        Column {
                            anchors.centerIn: parent
                            width: parent.width - 40
                            spacing: 8
                            IconGlyph {
                                anchors.horizontalCenter: parent.horizontalCenter
                                name: "search"
                                color: Theme.textMuted
                                width: 16; height: 16
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: root.hasCategoryFilter
                                      ? qsTr("Search, or pick a category, to see templates here.")
                                      : qsTr("Type to search for a template.")
                                color: Theme.textMuted
                                wrapMode: Text.Wrap
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }

                    // The list: rows run the full width of the field; the scrollbar floats in the right gutter and
                    // only takes room when the list actually overflows.
                    Item {
                        visible: root.showResults
                        width: parent.width
                        height: Math.min(7, root.filteredTemplates.length) * 42 - 4

                        Flickable {
                            id: listFlick
                            anchors.fill: parent
                            contentWidth: width
                            contentHeight: listCol.height
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds

                            Column {
                                id: listCol
                                width: parent.width - (listScrollBar.visible ? 14 : 0)
                                spacing: 4

                                Repeater {
                                    model: root.filteredTemplates

                                    delegate: Rectangle {
                                        id: tplRow
                                        required property var modelData
                                        readonly property bool selected: root.selectedKey === tplRow.modelData.key

                                        width: listCol.width
                                        height: 38
                                        radius: Theme.radiusMd
                                        color: tplRow.selected ? Qt.alpha(Theme.accent, 0.12)
                                                               : (rowArea.containsMouse ? Theme.card : Theme.inset)
                                        border.width: 1
                                        border.color: tplRow.selected ? Theme.accent : Theme.borderSubtle
                                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                        Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                                        // The Templates tab's own mark (the same icon the Media tab's Templates button wears),
                                        // on the row's optical centre; the real preview is the pane on the left.
                                        IconGlyph {
                                            x: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.verticalCenterOffset: 1
                                            name: "template"
                                            color: tplRow.selected ? Theme.accentLight : Theme.textMuted
                                            width: 16; height: 16
                                            fit: true
                                        }

                                        Text {
                                            x: 38
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: parent.width - 38 - 38
                                            text: tplRow.modelData.name
                                            color: Theme.textPrimary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.textSm
                                            font.weight: tplRow.selected ? Font.DemiBold : Font.Medium
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            anchors.right: parent.right
                                            anchors.rightMargin: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: root.isFavorite(tplRow.modelData.key) ? "★" : "☆"
                                            color: root.isFavorite(tplRow.modelData.key) ? Theme.warning : Theme.textMuted
                                            font.pixelSize: 14

                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -8
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
                            anchors.right: parent.right
                            height: parent.height
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                anchors.right: parent.right
                spacing: Theme.space3

                AppButton {
                    text: qsTr("Cancel")
                    variant: "secondary"
                    onClicked: root.cancelled()
                }
                AppButton {
                    text: qsTr("Use template")
                    variant: "primary"
                    onClicked: root.accept()
                }
            }
        }
        }

        AppScrollBar {
            flickable: pickerFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 8; anchors.bottomMargin: 8
            anchors.right: parent.right; anchors.rightMargin: 2
        }
    }
}