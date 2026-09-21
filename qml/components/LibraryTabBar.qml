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

    // Index into `tabs` of the selected tab; the host binds its pane
    // switcher to this. Shows is the landing tab (the Figma export froze
    // Scripture selected; the app opens on the shows library).
    property int currentTab: 0
    readonly property var tabs: [
        { label: "Shows",     icon: "play",            pane: "shows" },
        { label: "Media",     icon: "layoutDashboard", pane: "media" },
        { label: "Overlays",  icon: "layers",          pane: "overlays" },
        { label: "Templates", icon: "layoutTemplate",  pane: "templates" },
        { label: "Scripture", icon: "bookOpen",        pane: "scripture" },
        { label: "The Table", icon: "bookOpen",        pane: "table" },
        { label: "Calendar",  icon: "calendar",        pane: "soon" },
        { label: "Functions", icon: "wrench",          pane: "soon" }
    ]
    // Pane key of the current tab (\"shows\" | \"media\" | \"overlays\" |
    // \"scripture\" | \"table\" | \"soon\") — the host's switcher reads this.
    readonly property string currentPane: tabs[currentTab].pane

    // ---- Search, tied to the active tab ------------------------------------------------
    // One query per tab (keyed by its pane), so a tab keeps its search while you look at another and
    // each pane filters only its own content: Shows the shows, Media the media... A pane reads its own
    // query as `searches.<pane>` ("" until something is typed). Tabs with nothing to search (the
    // coming-soon ones) get no box.
    property var searches: ({})
    readonly property bool canSearch: currentPane !== "soon"
    function setSearch(text) {
        const next = Object.assign({}, root.searches)
        next[root.currentPane] = text
        root.searches = next
    }

    color: "transparent"

    // The far right of the bar, in the tabs' own row (the FreeShow sample:
    // the tab pack hugs the left, Search sits at the bar's right edge with
    // the same red underline).
    TabSearchBox {
        visible: root.canSearch
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 8
        height: 31
        // Whatever the tabs leave: up to 160 px, never less than 120.
        width: Math.max(120, Math.min(160, root.width - tabsRow.childrenRect.width - 8 - 8))
        placeholder: qsTr("Search")
        focusedPlaceholder: qsTr("Search %1").arg(root.tabs[root.currentTab].label.toLowerCase())
        text: root.searches[root.currentPane] !== undefined ? root.searches[root.currentPane] : ""
        onEdited: (value) => root.setSearch(value)
    }

    // Left-packed row — the tabs hug the LEFT edge of the bar (user call:
    // the justified experiment spread them rightward and clipped Functions
    // at the dock edge; "to the left" is the wanted arrangement).
    Flow {
        id: tabsRow
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 8
        anchors.topMargin: 8
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
                // 12 left | icon 13 | 6 gap | label | 13 right
                width: tabLabel.implicitWidth + 44
                topLeftRadius: 6
                topRightRadius: 6
                // Hover from pointer position (PositionHoverArea): a MouseArea's containsMouse never clears in this
                // build, which left tabs you had merely passed over lit.
                color: tabHover.hovered && !selected ? "#1a1b23" : "transparent"

                IconGlyph {
                    name: tabDelegate.modelData.icon
                    color: tabDelegate.selected ? "#ff4d3d" : "#8a94a6"
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    fit: true
                    width: 13; height: 13
                }
                Text {
                    id: tabLabel

                    x: 31
                    anchors.verticalCenter: parent.verticalCenter

                    // Heavier and lighter-on-dark than before: the tabs read as faint.
                    color: tabDelegate.selected ? "#f1f5f9" : "#b4bccb"
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: tabDelegate.selected ? Font.Bold : Font.DemiBold
                    horizontalAlignment: Text.AlignLeft
                    text: tabDelegate.modelData.label
                    textFormat: Text.PlainText
                }
                // The reference's selected-tab red underline.
                Rectangle {
                    visible: tabDelegate.selected
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    radius: 1
                    color: "#ff4d3d"
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
