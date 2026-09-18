import QtQuick
import VGRPresenterUI

// The Settings dialog's title bar: logo, search box, close button.
//
// The search is a command palette: typing filters `searchIndex` (the
// app-wide list of settings entries, supplied by ModalShell) and opens a
// suggestions dropdown under the box — each row shows the entry's label
// plus a tag chip naming the section it lives in, exactly like the
// reference. Rows are keyboard-navigable (↑↓ move, Enter opens, Escape
// closes — the footer hints say so) and the list SCROLLS via the shared
// AppScrollBar once results exceed the visible row count: it's a list,
// so it gets the same scrollbar treatment as every other list in the app.
//
// Selecting a row emits sectionRequested(key) — ModalShell forwards that
// to its own sectionSelected, so the nav rail highlight and the content
// loader both follow.
Rectangle {
    id: root

    property string searchPlaceholder: "Search settings"
    property alias searchText: searchInput.text
    // The searchable index: [{ label, section, key }] — label is what's
    // matched and shown, section is the tag chip's text, key is the
    // NavRail section key emitted on selection.
    property var searchIndex: []

    signal sectionRequested(string key)
    signal closeRequested()

    // Dropdown state. searchOpen only while there's a query with results.
    property bool searchOpen: false
    property var results: []
    property int selectedIndex: 0
    readonly property int visibleRows: 6

    function closeSearch() {
        root.searchOpen = false
        root.selectedIndex = 0
    }

    function runSearch() {
        const q = searchInput.text.trim().toLowerCase()
        if (q === "") {
            root.results = []
            root.closeSearch()
            return
        }
        const out = []
        for (let i = 0; i < root.searchIndex.length; ++i) {
            const e = root.searchIndex[i]
            if (String(e.label).toLowerCase().indexOf(q) >= 0
                    || String(e.section).toLowerCase().indexOf(q) >= 0)
                out.push(e)
        }
        root.results = out
        root.selectedIndex = 0
        root.searchOpen = out.length > 0
    }

    function acceptSelected() {
        if (!root.searchOpen || root.selectedIndex < 0 || root.selectedIndex >= root.results.length)
            return
        const key = root.results[root.selectedIndex].key
        root.sectionRequested(key)
        searchInput.text = ""
        root.closeSearch()
    }

    implicitHeight: 56
    color: Theme.surface

    Rectangle {
        id: logo
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        width: 26
        height: 26
        radius: Theme.radiusMd
        color: Theme.accent
    }

    Rectangle {
        id: searchBox
        anchors.right: closeButton.left
        anchors.rightMargin: Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        width: 200
        height: 30
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: searchInput.activeFocus ? Theme.accent : Theme.border
        Behavior on border.color { ColorAnimation { duration: 100 } }

        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "⌕"
            color: Theme.textMuted
            font.pixelSize: Theme.textSm
        }

        TextInput {
            id: searchInput
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.right: parent.right
            anchors.rightMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            color: Theme.textPrimary
            clip: true

            Text {
                visible: !searchInput.text.length && !searchInput.activeFocus
                text: root.searchPlaceholder
                font: searchInput.font
                color: Theme.textMuted
            }

            onTextChanged: root.runSearch()

            // Keyboard palette behavior — only intercept while the
            // dropdown is open, so normal caret movement is untouched
            // otherwise. Escape closes AND clears.
            Keys.onUpPressed: {
                if (!root.searchOpen)
                    return
                root.selectedIndex = Math.max(0, root.selectedIndex - 1)
                event.accepted = true
            }
            Keys.onDownPressed: {
                if (!root.searchOpen)
                    return
                root.selectedIndex = Math.min(root.results.length - 1, root.selectedIndex + 1)
                event.accepted = true
            }
            Keys.onReturnPressed: {
                if (root.searchOpen) {
                    root.acceptSelected()
                    event.accepted = true
                }
            }
            Keys.onEnterPressed: {
                if (root.searchOpen) {
                    root.acceptSelected()
                    event.accepted = true
                }
            }
            Keys.onEscapePressed: {
                if (root.searchOpen) {
                    searchInput.text = ""
                    root.closeSearch()
                    event.accepted = true
                }
            }
        }
    }

    // ---- Suggestions dropdown ----
    // A child of the TitleBar so it can float below the bar over the nav
    // rail; ModalShell raises the whole TitleBar (z: 10) so this paints
    // above the rail and content rather than under its later siblings.
    Rectangle {
        id: suggestionsPanel
        visible: root.searchOpen
        x: searchBox.x + searchBox.width - width
        y: root.height + 6
        width: 300
        height: suggestionsCol.height + 12
        radius: Theme.radiusLg
        color: Theme.surface
        border.color: Theme.border
        border.width: 1

        // Keeps the dropdown up when a click lands inside it (the shell's
        // outside-click closer only fires on genuine outside clicks).
        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: suggestionsCol
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 0

            Text {
                leftPadding: 8
                bottomPadding: 6
                text: qsTr("SUGGESTIONS")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.Bold
            }

            Flickable {
                id: resultsFlick
                width: parent.width
                // Fixed visible window — extra results scroll (the shared
                // scrollbar below), it's a list like any other.
                height: Math.min(root.results.length, root.visibleRows) * 34
                contentWidth: width
                contentHeight: root.results.length * 34
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Repeater {
                    model: root.results
                    delegate: Rectangle {
                        id: resultRow
                        required property var modelData
                        required property int index
                        readonly property bool selected: resultRow.index === root.selectedIndex

                        y: resultRow.index * 34
                        width: resultsFlick.width
                        height: 34
                        radius: Theme.radiusMd
                        color: resultRow.selected ? "#266C5CE7"
                            : rowArea.containsMouse ? Theme.chip : "transparent"

                        Row {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 5
                                height: 5
                                radius: 2.5
                                color: resultRow.selected ? Theme.accentLight : Theme.textMuted
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 170
                                text: resultRow.modelData.label
                                color: resultRow.selected ? "#ffffff" : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                                elide: Text.ElideRight
                            }
                        }

                        // Section tag chip — where this entry lives.
                        Rectangle {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: chipLabel.width + 14
                            height: 18
                            radius: 9
                            color: resultRow.selected ? "#3d6C5CE7" : Theme.chip

                            Text {
                                id: chipLabel
                                anchors.centerIn: parent
                                text: resultRow.modelData.section
                                color: resultRow.selected ? "#ffffff" : Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 9
                            }
                        }

                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onPositionChanged: root.selectedIndex = resultRow.index
                            onClicked: root.acceptSelected()
                        }
                    }
                }
            }

            AppScrollBar {
                x: resultsFlick.width - width
                y: 0
                height: resultsFlick.height
                flickable: resultsFlick
            }

            Text {
                visible: root.results.length === 0
                leftPadding: 8
                height: 34
                verticalAlignment: Text.AlignVCenter
                text: qsTr("No matches")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }

            Text {
                leftPadding: 8
                topPadding: 6
                text: qsTr("↑↓ navigate    ↵ open    esc close")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 9
            }
        }
    }

    Rectangle {
        id: closeButton
        anchors.right: parent.right
        anchors.rightMargin: Theme.space5
        anchors.verticalCenter: parent.verticalCenter
        width: 34
        height: 34
        radius: Theme.radiusMd
        color: closeArea.containsMouse ? Theme.chip : Theme.inset

        Text {
            anchors.centerIn: parent
            text: "✕"
            color: Theme.textSecondary
            font.pixelSize: Theme.textMd
        }

        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.closeRequested()
        }
    }
}
