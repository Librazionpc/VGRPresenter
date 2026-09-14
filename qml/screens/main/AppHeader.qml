import QtQuick

// The 48px header strip (Show/Edit/Stage tabs + Connected/window controls).
// Shared by every screen — instantiated once in Main.qml instead of once
// per screen, so there's exactly one ViewTabs/HeaderStatus in the tree
// rather than a separate copy behind each screen.
//
// The logo + File/Edit/View/Help menu (AppMenuBar) is deliberately NOT
// drawn here: it needs to sit above every screen (for its dropdowns and
// click-outside catcher to work regardless of which screen is showing), so
// Main.qml instantiates it separately as the topmost layer.
//
// Literal colors, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml at this nesting depth.
Rectangle {
    id: root

    height: 48
    width: 1440

    border.color: "#232530"
    border.width: 1
    color: "#12131a"

    property string activeTab: "show"
    signal tabSelected(string tab)

    ViewTabs {
        x: 639.50
        y: 8
        activeTab: root.activeTab
        onTabSelected: (tab) => root.tabSelected(tab)
    }
    HeaderStatus {
        x: 1260
        y: 17
    }
}
