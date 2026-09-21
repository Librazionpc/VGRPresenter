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
        { label: "Shows",     icon: "presentation",    pane: "shows" },
        { label: "Media",     icon: "layoutDashboard", pane: "media" },
        { label: "Audio",     icon: "music",           pane: "soon" },
        { label: "Overlays",  icon: "layers",          pane: "soon" },
        { label: "Templates", icon: "layoutTemplate",  pane: "soon" },
        { label: "Scripture", icon: "book",            pane: "scripture" },
        { label: "The Table", icon: "book",            pane: "table" },
        { label: "Calendar",  icon: "calendar",        pane: "soon" },
        { label: "Functions", icon: "settings",        pane: "soon" }
    ]
    // Pane key of the current tab (\"shows\" | \"media\" | \"scripture\" |
    // \"table\" | \"soon\") — the host's switcher reads this.
    readonly property string currentPane: tabs[currentTab].pane

    color: "transparent"

    // Left-packed row — the tabs hug the LEFT edge of the bar (user call:
    // the justified experiment spread them rightward and clipped Functions
    // at the dock edge; "to the left" is the wanted arrangement).
    Flow {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 8
        anchors.topMargin: 8
        spacing: 2

        Repeater {
            model: root.tabs

            delegate: Rectangle {
                id: tabDelegate

                required property int index
                required property var modelData
                property bool selected: root.currentTab === index

                objectName: "selfTestTab_" + modelData.pane

                height: 31
                width: tabLabel.implicitWidth + 34
                topLeftRadius: 6
                topRightRadius: 6
                color: tabMouse.containsMouse && !selected ? "#1a1b23" : "transparent"

                IconGlyph {
                    name: tabDelegate.modelData.icon
                    color: tabDelegate.selected ? "#ff4d3d" : "#5c6475"
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 13; height: 13
                }
                Text {
                    id: tabLabel

                    x: 25
                    anchors.verticalCenter: parent.verticalCenter

                    color: tabDelegate.selected ? "#e2e8f0" : "#8a94a6"
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: tabDelegate.selected ? Font.DemiBold : Font.Medium
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
                MouseArea {
                    id: tabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.currentTab = tabDelegate.index
                }
            }
        }
    }
}
