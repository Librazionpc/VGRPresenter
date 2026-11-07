import QtQuick
import VGRPresenterUI

Rectangle {
    id: root
    objectName: "selfTestLibraryTabBar"

    property int currentTab: 0
    readonly property var tabs: [
        { label: "Shows",     icon: "shows",    pane: "shows" },
        { label: "Media",     icon: "media",    pane: "media" },
        { label: "Overlays",  icon: "overlay",  pane: "overlays" },
        { label: "Templates", icon: "template", pane: "templates" },
        { label: "Scripture", icon: "bible",    pane: "scripture" },
        { label: "The Table", icon: "book",     pane: "table" },
        { label: "Calendar",  icon: "calendar", pane: "soon" },
        { label: "Functions", icon: "wrench",   pane: "functions" }
    ]
    readonly property string currentPane: tabs[currentTab].pane

    // ---- Search, tied to the active tab ------------------------------------------------
    property var searches: ({})
    // The Functions pane drives flows (no search over them yet); the coming-
    // soon tabs have nothing at all to search.
    readonly property bool canSearch: currentPane !== "soon" && currentPane !== "functions"
    function setSearchFor(pane, text) {
        if (pane === "")
            return
        const next = Object.assign({}, root.searches)
        next[pane] = text
        root.searches = next
    }
    function setSearch(text) {
        root.setSearchFor(root.currentPane, text)
    }

    // ---- The typing beat, in ONE place ---------------------------------------
    property string pendingSearch: ""
    property string pendingPane: ""
    function delayedSearch(value) {
        root.pendingSearch = value
        root.pendingPane = root.currentPane
        filterDebounce.restart()
    }
    function flushSearch() {
        if (!filterDebounce.running && root.pendingPane === "")
            return
        filterDebounce.stop()
        const pane = root.pendingPane
        const text = root.pendingSearch
        root.pendingSearch = ""
        root.pendingPane = ""
        root.setSearchFor(pane, text)
    }
    Timer {
        id: filterDebounce
        interval: 150
        onTriggered: root.flushSearch()
    }

    // ---- Live autocomplete (Scripture / The Table) -------------------------------------
    property var suggesters: ({})
    function registerSuggester(pane, item) {
        const next = Object.assign({}, root.suggesters)
        next[pane] = item
        root.suggesters = next
    }
    readonly property var suggestions: (text) => {
        const s = root.suggesters[root.currentPane]
        if (!root.canSearch || !s || !s.paneSuggestions)
            return { rows: [], complete: "" }
        const res = s.paneSuggestions(text)
        return Array.isArray(res) ? { rows: res, complete: "" } : res
    }

    // ---- What the box STARTS with --------------------------------------------
    readonly property string searchSeed: {
        const s = root.suggesters[root.currentPane]
        if (!canSearch || !s || !s.paneSearchSeed)
            return ""
        return String(s.paneSearchSeed() || "")
    }

    color: "transparent"

    TabSearchBox {
        id: searchBox
        objectName: "selfTestSearchBox"
        visible: root.canSearch
        anchors.right: parent.right
        anchors.top: parent.top
        height: parent.height
        // At rest it keeps to the room the tabs leave; focused it opens up (see
        // TabSearchBox.editing) to its full line, over the tabs if it has to.
        z: searchBox.editing ? 10 : 0
        width: {
            const idle = Math.max(100, Math.min(searchBox.implicitWidth, root.width - tabsRow.childrenRect.width - 16))
            if (!searchBox.editing)
                return idle
            return Math.max(idle, Math.min(Math.max(searchBox.implicitWidth, searchBox.editMinWidth), root.width - 16))
        }
        Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        placeholder: qsTr("Search %1").arg(root.tabs[root.currentTab].label)
        // The passage the pane is on, ready to take a paragraph/verse number.
        seedText: root.searchSeed
        // It follows the pane (open another chapter, step a passage) until the
        // operator makes it their own query by typing in it.
        seedFollows: !root.searches[root.currentPane]
        function pushStored() {
            // The box is the source of truth while an edit of it is still waiting for its beat to
            // be stored: pushing the OLDER stored text back then wiped what had just been typed.
            if (filterDebounce.running || root.pendingPane !== "")
                return
            const want = root.searches[root.currentPane] !== undefined ? root.searches[root.currentPane] : ""
            if (searchBox.text !== want)
                searchBox.resetText(want)
        }
        Component.onCompleted: pushStored()
        Connections {
            target: root
            // (Qualified: a Connections handler's unqualified lookup skips intermediate
            // objects, so a bare pushStored() here threw "pushStored is not defined".)
            function onSearchesChanged() { searchBox.pushStored() }
        }
        Connections {
            target: root
            function onCurrentPaneChanged() {
                root.flushSearch()
                searchBox.userCleared = false   // another tab: its passage line may fill again
                searchBox.pushStored()
                Qt.callLater(function () {
                    if (searchBox.text === "")
                        searchBox.applySeed()
                })
            }
        }
        onEdited: (value) => {
            const s = root.suggesters[root.currentPane]
            // The place to come back to on Esc is taken BEFORE the typing jumps the
            // pane (see ReferencePane::beginSearch).
            if (s && s.beginSearch)
                s.beginSearch()
            if (s && s.jumpToTyped)
                s.jumpToTyped(value)
            root.delayedSearch(value)
        }
        // Esc leaves the search: the pane returns to the place the typing started
        // from, and the tab's stored query is cleared with it.
        onDismissed: {
            const s = root.suggesters[root.currentPane]
            root.flushSearch()
            root.setSearchFor(root.currentPane, "")
            if (s && s.endSearch)
                s.endSearch()
        }
        onAccepted: (value) => {
            root.flushSearch()
            const s = root.suggesters[root.currentPane]
            if (s && s.matchCursor >= 0 && s.takeMatch) {
                const cite = String(s.takeMatch() || "")
                if (cite !== "")
                    return
            }
            if (s && s.playPicked) {
                s.playPicked()
                const picked = s.selected !== undefined && s.selected !== null
                               && s.selected.length > 0
                if (picked && s.searchReturn !== undefined && s.searchReturn !== null) {
                    root.setSearchFor(root.currentPane, "")
                    searchBox.resetText("")
                }
            }
        }
        Connections {
            target: root.suggesters[root.currentPane] !== undefined ? root.suggesters[root.currentPane] : null
            ignoreUnknownSignals: true
            function onMatchTaken() {
                root.flushSearch()
                root.setSearchFor(root.currentPane, "")
            }
        }
        // The search box's Up/Down over the in-pane result list (The Table).
        onStepMatch: (delta) => {
            const s = root.suggesters[root.currentPane]
            if (s && s.stepInColumn)
                s.stepInColumn(delta)
            else if (s && s.stepMatch)
                s.stepMatch(delta)
        }
        // ... and its Left/Right across the pane's columns while that list is up.
        onStepColumn: (delta) => {
            const s = root.suggesters[root.currentPane]
            if (s && s.stepColumn)
                s.stepColumn(delta)
        }
        // The pane says whether its result list is showing; the box hands the arrows
        // over to the search the moment it is (and gives the caret back when it is not).
        listMode: {
            const s = root.suggesters[root.currentPane]
            return !!(s && s.searchMode)
        }
        // Enter on a chapter with no verse fills the verse in (Scripture).
        finishText: (text) => {
            const s = root.suggesters[root.currentPane]
            return (s && s.completeReference) ? String(s.completeReference(text) || "") : ""
        }
        // The active tab's live reference suggestions (null on plain-filter tabs).
        suggestions: root.suggestions
        // The Table's matches render IN THE PANE (matchesList): Up/Down walk them.
        matchesInPane: root.currentPane === "table"
    }

    Flow {
        id: tabsRow
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 8
        spacing: 10   // room between the tabs

        Repeater {
            model: root.tabs

            delegate: Rectangle {
                id: tabDelegate

                required property int index
                required property var modelData
                property bool selected: root.currentTab === index

                objectName: "selfTestTab_" + modelData.pane

                height: 31
                // 12 left | icon 15 | 7 gap | label | 13 right
                width: tabLabel.implicitWidth + 47
                topLeftRadius: 6
                topRightRadius: 6
                color: selected ? Theme.activeBg : "transparent"
                // Selection is state and still fades; hover is the app's shared
                // glow layer, so a tab lights exactly like a card or a row.
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                HoverGlow {
                    anchors.fill: parent
                    // The tab is squared at the bottom (it meets the bar's
                    // rule), so the hover wash has to be too.
                    radius: 0
                    topLeftRadius: parent.topLeftRadius
                    topRightRadius: parent.topRightRadius
                    hovered: tabHover.hovered && !parent.selected
                    glowColor: Theme.hoverBg
                }

                // 1.5px stroke, where a 24-grid glyph's 2 units come out as
                // 2 * 15/24 = 1.25px at this box, so they read heavier as well.
                // 1.25 here matches their weight, and 15 puts the row a step up.
                IconGlyph {
                    name: tabDelegate.modelData.icon
                    color: tabDelegate.selected ? Theme.accent : Theme.textSecondary
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    fit: true
                    strokeWidth: 1.25
                    width: 15; height: 15
                }
                Text {
                    id: tabLabel

                    x: 34
                    anchors.verticalCenter: parent.verticalCenter

                    // Heavier and lighter-on-dark than before: the tabs read as faint.
                    color: tabDelegate.selected ? Theme.textPrimary : Theme.iconChrome
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: tabDelegate.selected ? Font.Bold : Font.DemiBold
                    horizontalAlignment: Text.AlignLeft
                    text: tabDelegate.modelData.label
                    textFormat: Text.PlainText
                }
                Rectangle {
                    visible: tabDelegate.selected
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: -4
                    height: 2
                    radius: 1
                    color: Theme.accent
                }
                PositionHoverArea {
                    id: tabHover
                    anchors.fill: parent
                    onClicked: root.currentTab = tabDelegate.index
                }
                HoverDetail {
                    anchors.fill: parent
                    title: tabDelegate.modelData.label
                    detail: root.tabShortcut(tabDelegate.index)
                }
            }
        }
    }

    function tabShortcut(index) {
        const seqs = Hotkeys.sequences("libraryTabs")
        return index >= 0 && index < seqs.length ? seqs[index] : ""
    }
    Repeater {
        model: root.tabs
        delegate: Shortcut {
            required property int index
            sequence: root.tabShortcut(index)
            onActivated: root.currentTab = index
        }
    }
}