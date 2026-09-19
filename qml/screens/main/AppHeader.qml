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
    // The gear — the entry point to the Settings dialog (Main.qml owns the
    // dialog itself and shows it on this signal).
    signal settingsClicked()

    ViewTabs {
        x: 639.50
        y: 8
        activeTab: root.activeTab
        onTabSelected: (tab) => root.tabSelected(tab)
    }

    // Settings gear — sits between the tabs and the status/window-controls
    // cluster, the standard top-right slot. Hover-washed round button with
    // the same Lucide gear path data IconGlyph already carries.
    Rectangle {
        x: 1218
        y: 10
        width: 28
        height: 28
        radius: 14
        // Position truth — containsMouse latches forever in this build
        // (KNOWN_ISSUES.md). showCursor: false: the wash itself is the
        // feedback; no pointing hand over a settings gear.
        color: gearHover.hovered ? "#1a6c5ce7" : "transparent"
        Behavior on color { ColorAnimation { duration: 100 } }

        // IconGlyph is a plain Item: implicitWidth/Height alone leave it
        // 0x0, and everything anchored inside it collapses to its top-left
        // corner — the gear rendered visibly off-center (down-right) inside
        // this circle. Explicit width/height actually size it; centerIn
        // then centers it.
        IconGlyph {
            anchors.centerIn: parent
            name: "settings"
            color: gearHover.hovered ? "#e2e8f0" : "#8a94a6"
            width: 14
            height: 14
        }

        PositionHoverArea {
            id: gearHover
            anchors.fill: parent
            onClicked: root.settingsClicked()
        }
    }

    HeaderStatus {
        x: 1260
        y: 17
    }
}
