// The media-resource dock's tab bar — all nine library tabs share ONE
// delegate (same icon box, label metrics, selected red underline + hover
// tint: design consistency by construction, replacing the Figma export's
// seven hand-pinned per-tab blocks that could never stay identical). The
// eighth tab, THE TABLE, is the sermon app — it rides the same
// Scripture-architecture browser through its JSON export file.
import QtQuick
import VGRPresenterUI

Rectangle {
    id: root
    objectName: "selfTestLibraryTabBar"

    // Index into `tabs` of the selected tab; the host binds its pane
    // switcher to this. Shows is the landing tab (the Figma export froze
    // Scripture selected; the app opens on the shows library).
    property int currentTab: 0
    readonly property var tabs: [
        // Shows / Media / Templates / Scripture / The Table / Calendar carry the
        // app's own icon assets (IconGlyph "shows" / "media" / "overlay" /
        // "template" / "bible" / "book" / "calendar", from qml/assets/shows
        // icon.svg, media icon.svg, overlay.svg, template.svg, bible .svg,
        // book.svg and calendar-event.svg) instead of the generic play / layout
        // / open-book glyphs they used to borrow. Overlays was the last tab
        // still wearing the shared hand-sized "layers" glyph.
        { label: "Shows",     icon: "shows",    pane: "shows" },
        { label: "Media",     icon: "media",    pane: "media" },
        { label: "Overlays",  icon: "overlay",  pane: "overlays" },
        { label: "Templates", icon: "template", pane: "templates" },
        { label: "Scripture", icon: "bible",    pane: "scripture" },
        { label: "The Table", icon: "book",     pane: "table" },
        { label: "Calendar",  icon: "calendar", pane: "soon" },
        { label: "Functions", icon: "wrench",   pane: "functions" }
    ]
    // Pane key of the current tab (\"shows\" | \"media\" | \"overlays\" |
    // \"scripture\" | \"table\" | \"functions\" | \"soon\") — the host's
    // switcher reads this.
    readonly property string currentPane: tabs[currentTab].pane

    // ---- Search, tied to the active tab ------------------------------------------------
    // One query per tab (keyed by its pane), so a tab keeps its search while you look at another and
    // each pane filters only its own content: Shows the shows, Media the media... A pane reads its own
    // query as `searches.<pane>` ("" until something is typed). Tabs with nothing to search (the
    // coming-soon ones) get no box.
    property var searches: ({})
    // The Functions pane drives flows (no search over them yet); the coming-
    // soon tabs have nothing at all to search.
    readonly property bool canSearch: currentPane !== "soon" && currentPane !== "functions"
    function setSearch(text) {
        const next = Object.assign({}, root.searches)
        next[root.currentPane] = text
        root.searches = next
    }

    // ---- Live autocomplete (Scripture / The Table) -------------------------------------
    // Panes with reference suggestions register a provider under their pane key
    // (registerSuggester); `suggestions` wraps the ACTIVE tab's provider as one function
    // (text) => rows — read fresh at every call, so each keystroke gets current rows. A
    // picked row is routed to the pane that offered it via suggestionPicked(paneKey, ref).
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
    signal suggestionPicked(string pane, var ref)
    // A suggestion row was hovered / un-hovered — relayed to the pane that offered
    // the rows (the pane previews the row's content while the hover lasts).
    signal suggestionHovered(string pane, int index, bool hovering)

    color: "transparent"

    // The far right of the bar, in the tabs' own row (the FreeShow sample:
    // the tab pack hugs the left, Search sits at the bar's right edge with
    // the same red underline).
    TabSearchBox {
        id: searchBox
        objectName: "selfTestSearchBox"
        visible: root.canSearch
        anchors.right: parent.right
        anchors.top: parent.top
        // Fills the bar's full height rather than 31-in-39-centered: its icon/text are already
        // centered internally, so this lands them exactly where the (also-centered) tab labels
        // sit, and its own red underline - drawn at ITS bottom - lands right on the bar's border
        // instead of floating short of it.
        height: parent.height
        // Hugs its own placeholder text (its implicitWidth) - clamped so a long "Search <tab>" hint still can't
        // run into the tabs, and a very narrow window still leaves it usable.
        width: Math.max(100, Math.min(searchBox.implicitWidth, root.width - tabsRow.childrenRect.width - 16))
        placeholder: qsTr("Search")
        focusedPlaceholder: qsTr("Search %1").arg(root.tabs[root.currentTab].label.toLowerCase())
        // Per-tab queries are pushed in imperatively (TabSearchBox.resetText) instead of
        // bound — the autofill's programmatic writes would silently break a `text:` binding
        // and tab switches would stop restoring that tab's query. Every keystroke also
        // re-fires onSearchesChanged (setSearch rebuilds the map), so both handlers only
        // push when the stored value DIFFERS from the box — the user's own typing is a no-op.
        function pushStored() {
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
            function onCurrentPaneChanged() { searchBox.pushStored() }
        }
        onEdited: (value) => root.setSearch(value)
        // The active tab's live reference suggestions (null on plain-filter tabs).
        suggestions: root.suggestions
        // The Table's matches render IN THE PANE (matchesList) — the floating popup
        // never opens there (user call: it covered the search box and the preview).
        popupSuppressed: root.currentPane === "table"
        onSuggestionPicked: (index, payload) => {
            const s = root.suggesters[root.currentPane]
            if (s && payload && payload.ref)
                root.suggestionPicked(root.currentPane, payload.ref)
        }
        onSuggestionHovered: (index, hovering) => {
            const s = root.suggesters[root.currentPane]
            if (s)
                root.suggestionHovered(root.currentPane, index, hovering)
        }
    }

    // Left-packed row — the tabs hug the LEFT edge of the bar (user call:
    // the justified experiment spread them rightward and clipped Functions
    // at the dock edge; "to the left" is the wanted arrangement).
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
                // Hover from pointer position (PositionHoverArea): a MouseArea's containsMouse never clears in this
                // build, which left tabs you had merely passed over lit.
                // Selected tabs wear the app's active-pill wash (Theme.activeBg
                // — the user call: selection must read at a glance, same
                // treatment as the sidebar's selected row); the underline +
                // accent icon stay. Hover keeps a fainter lift.
                color: selected ? Theme.activeBg : (tabHover.hovered ? Theme.hoverBg : "transparent")
                Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }

                // Every tab icon is the SAME 15px box and the same optical
                // weight (user call: the row read as mixed sizes). Two things
                // used to break that: `layers` and `calendar` are hand-sized
                // glyphs (10 and 9x10 of natural ink) which `fit` cannot scale -
                // it fits the Lucide 24 grid to the box, and those two are not
                // drawn on that grid - so they stayed at their natural size
                // while the rest grew; and they drew with the IconGlyph default
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
                    font.family: "Segoe UI"
                    font.pixelSize: 15
                    font.weight: tabDelegate.selected ? Font.Bold : Font.DemiBold
                    horizontalAlignment: Text.AlignLeft
                    text: tabDelegate.modelData.label
                    textFormat: Text.PlainText
                }
                // The reference's selected-tab red underline. The delegate sits vertically
                // centered in the 39px bar now (4px short of its bottom on each side), so the
                // underline needs to reach 4px past its own parent's bottom to land on the bar's
                // border line - a negative bottomMargin off its own parent, not an anchor to a
                // distant ancestor (that flipped it to the TOP - anchoring to a non-parent/sibling
                // item across several levels of nesting resolved the wrong way here).
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
            }
        }
    }
}
