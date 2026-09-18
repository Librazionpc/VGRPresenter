import QtQuick

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
    readonly property var templates: [
        { key: "lowerThird", name: qsTr("Lower Third") },
        { key: "title", name: qsTr("Title") },
        { key: "sidebar", name: qsTr("Sidebar") },
        { key: "bottomBar", name: qsTr("Bottom Bar") },
        { key: "fullscreen", name: qsTr("Fullscreen") }
    ]

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
        return root.templates[0]
    }

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
    property string activeTab: "all" // "all" | "favorites"

    readonly property var filteredTemplates: root.templates.filter((t) => {
        if (root.activeTab === "favorites" && !root.isFavorite(t.key))
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

    anchors.fill: parent
    visible: root.open

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 700
        height: Math.min(parent.height - 60, content.height + 40)
        radius: 14
        color: "#13151c"
        border.color: "#232530"
        border.width: 1
        clip: true

        MouseArea { anchors.fill: parent; onClicked: {} }

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
                        font.family: "Inter"
                        font.pixelSize: 18
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
                            font.pixelSize: 12
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
                            color: "#ff4d3d"
                            font.family: "Inter"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Templates for ") + root.contentTypeLabel.toLowerCase()
                        color: "#8a94a6"
                        font.family: "Inter"
                        font.pixelSize: 12
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
                        font.family: "Inter"
                        font.pixelSize: 9
                        font.weight: Font.Bold
                    }

                    Rectangle {
                        width: parent.width
                        height: width * 9 / 16
                        radius: 8
                        color: "#0a0b0f"
                        border.color: "#262a38"
                        border.width: 1
                        clip: true

                        Rectangle {
                            x: 8; y: 8
                            width: liveLabel.implicitWidth + 12
                            height: 16
                            radius: 3
                            color: "#1c3b2a"

                            Text {
                                id: liveLabel
                                anchors.centerIn: parent
                                text: qsTr("LIVE")
                                color: "#2ed573"
                                font.family: "Inter"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                            }
                        }

                        // One generic placeholder visual, not a per-key
                        // hardcoded layout — no real slide rendering exists
                        // to preview yet (same status as this app's other
                        // placeholder visuals), and the template's name is
                        // what identifies it, not a bespoke preview shape.
                        Text {
                            anchors.centerIn: parent
                            text: root.previewTemplate.name
                            color: "#9b8ff5"
                            font.family: "Inter"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                    }

                    Text {
                        text: root.previewTemplate.name
                        color: "#eef1f8"
                        font.family: "Inter"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        height: 20
                        width: aspectLabel.implicitWidth + 14
                        radius: 4
                        color: "#1a1c26"
                        border.color: "#2a2f3a"
                        border.width: 1
                        Text { id: aspectLabel; anchors.centerIn: parent; text: "16:9"; color: "#8a94a6"; font.family: "Inter"; font.pixelSize: 9 }
                    }
                }

                // ---- Template list ----
                Column {
                    width: parent.width - (parent.width - 20) * 0.42 - 20
                    spacing: 8

                    Text {
                        text: qsTr("Templates")
                        color: "#5c6475"
                        font.family: "Inter"
                        font.pixelSize: 9
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

                            Text { anchors.verticalCenter: parent.verticalCenter; text: "⌕"; color: "#5c6475"; font.pixelSize: 12 }

                            TextInput {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 24
                                color: "#c9cedd"
                                font.family: "Inter"
                                font.pixelSize: 12
                                selectByMouse: true
                                onTextChanged: root.searchQuery = text

                                Text {
                                    visible: parent.text.length === 0
                                    text: qsTr("Search templates")
                                    color: "#5c6475"
                                    font.family: "Inter"
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }

                    Row {
                        spacing: 6

                        Repeater {
                            model: [
                                { key: "all", label: qsTr("All") },
                                { key: "favorites", label: "★ " + qsTr("Favorites") }
                            ]
                            delegate: Rectangle {
                                id: tabBtn
                                required property var modelData
                                readonly property bool active: root.activeTab === tabBtn.modelData.key

                                height: 26
                                width: tabLabel.implicitWidth + 20
                                radius: 13
                                color: tabBtn.active ? "#296c5ce7" : (tabArea.containsMouse ? "#1e2029" : "transparent")
                                border.color: tabBtn.active ? "#6c5ce7" : "transparent"
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    id: tabLabel
                                    anchors.centerIn: parent
                                    text: tabBtn.modelData.label
                                    color: tabBtn.active ? "#9b8ff5" : "#8a94a6"
                                    font.family: "Inter"
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                }

                                MouseArea {
                                    id: tabArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.activeTab = tabBtn.modelData.key
                                }
                            }
                        }
                    }

                    Flickable {
                        id: listFlick
                        width: parent.width
                        height: Math.min(5, root.filteredTemplates.length) * 54
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
                                    height: 48
                                    radius: 8
                                    color: tplRow.selected ? "#1a2240" : (rowArea.containsMouse ? "#191b24" : "#161823")
                                    border.width: tplRow.selected ? 1.5 : 1
                                    border.color: tplRow.selected ? "#6c5ce7" : "#262a38"
                                    Behavior on color { ColorAnimation { duration: 100 } }

                                    Rectangle {
                                        x: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 34
                                        height: 34
                                        radius: 5
                                        color: "#0d0f16"
                                        border.color: "#262a38"
                                        border.width: 1
                                    }

                                    Text {
                                        x: 54
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 54 - 36
                                        text: tplRow.modelData.name
                                        color: "#eef1f8"
                                        font.family: "Inter"
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        anchors.right: parent.right
                                        anchors.rightMargin: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.isFavorite(tplRow.modelData.key) ? "★" : "☆"
                                        color: root.isFavorite(tplRow.modelData.key) ? "#f5c26b" : "#5c6475"
                                        font.pixelSize: 13

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
                        font.family: "Inter"
                        font.pixelSize: 12
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
                    color: useArea.containsMouse ? "#ff6b5a" : "#ff4d3d"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Use template")
                        color: "#ffffff"
                        font.family: "Inter"
                        font.pixelSize: 12
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
}
