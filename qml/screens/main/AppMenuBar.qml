import QtQuick
import VGRPresenterUI
import "../../components"

// The VGRPresenter logo button plus the File / Edit / View / Help menu bar,
// each with its own dropdown panel. Extracted out of VGRPresenterMainScreen.qml
// so the interactive menu behavior lives in one small, focused file instead of
// being scattered through the generated screen.
//
// Expects to be sized to fill the whole window (anchors.fill: parent) so the
// click-outside-to-close catcher covers the full screen and the dropdown
// panels land at their intended screen coordinates.
Item {
    id: root

    // "" | "logo" | "file" | "edit" | "view" | "help"
    property string openMenu: ""

    function toggleMenu(name) {
        openMenu = (openMenu === name) ? "" : name
    }

    function closeMenu() {
        openMenu = ""
    }

    // Menu actions are presentation-only for now (the underlying commands
    // don't exist yet); the menu just closes.
    function activateItem(menuName, label) {
        closeMenu()
    }

    readonly property var logoMenuItems: [
        { label: "New show", trailing: "Ctrl+N" },
        { label: "Quick search…", trailing: "Ctrl+K" },
        { label: "Open project…", trailing: "Ctrl+O" },
        { label: "Save", trailing: "Ctrl+S" },
        { divider: true },
        { label: "Import…" },
        { label: "Export…" },
        { divider: true },
        { label: "Settings" },
        { label: "About VGRPresenter" },
        { label: "Exit" }
    ]

    readonly property var fileMenuItems: [
        { label: "New show", trailing: "Ctrl+N" },
        { label: "Open project…", trailing: "Ctrl+O" },
        { label: "Save", trailing: "Ctrl+S" },
        { label: "Save As…", trailing: "Ctrl+Shift+S" },
        { divider: true },
        { label: "Import…", trailing: "Ctrl+I" },
        { label: "Export…", trailing: "Ctrl+E" },
        { divider: true },
        { label: "Record…", trailing: "Ctrl+R" },
        { label: "Emergency Stop", trailing: "Ctrl+Shift+E", danger: true },
        { divider: true },
        { label: "Exit" }
    ]

    readonly property var editMenuItems: [
        { label: "Undo", trailing: "Ctrl+Z" },
        { label: "Redo", trailing: "Ctrl+Y" },
        { divider: true },
        { label: "Quick search…", trailing: "Ctrl+K" },
        { label: "Transpose song…", trailing: "Ctrl+T" },
        { label: "Set performance key…" },
        { label: "Reorder sections…" },
        { divider: true },
        { label: "Bible translation…" },
        { label: "Highlights…" },
        { label: "Notes…" },
        { divider: true },
        { label: "Preferences…" }
    ]

    readonly property var viewMenuItems: [
        { label: "Show mode", trailing: "Ctrl+1" },
        { label: "Edit mode", trailing: "Ctrl+2" },
        { label: "Stage mode", trailing: "Ctrl+3" },
        { divider: true },
        { label: "Projects panel", trailing: "Ctrl+P" },
        { label: "Media dock" },
        { label: "Preview monitors" },
        { divider: true },
        { label: "Fullscreen", trailing: "F11" },
        { label: "Reset layout" },
        { divider: true },
        { label: "Outputs", trailing: "›" }
    ]

    readonly property var helpMenuItems: [
        { label: "Quick search…", trailing: "Ctrl+K" },
        { label: "Keyboard shortcuts", trailing: "Ctrl+/" },
        { label: "Documentation", trailing: "F1" },
        { divider: true },
        { label: "Production health…" },
        { label: "System diagnostics…" },
        { label: "View engine logs…" },
        { divider: true },
        { label: "Check for updates" },
        { label: "Report a problem" },
        { divider: true },
        { label: "About VGRPresenter", trailing: "v1.0.5-beta" }
    ]

    // Swallows the next click anywhere on screen to close an open menu,
    // without intercepting input at all when no menu is open.
    MouseArea {
        anchors.fill: parent
        enabled: root.openMenu !== ""
        onClicked: root.closeMenu()
    }

    Rectangle {
        id: header_left

        x: 16
        y: 12.50

        height: 23
        width: 271

        color: "transparent"

        Item {
            id: logoButton
            width: logoText.implicitWidth
            height: parent.height

            readonly property bool isOpen: root.openMenu === "logo"
            readonly property bool isHover: logoArea.containsMouse

            Rectangle {
                anchors.fill: parent
                anchors.margins: -4
                // Literals, not Theme.radiusSm / Theme.accent — see logoText below.
                radius: 6
                color: logoButton.isOpen || logoButton.isHover
                       ? "#1a6c5ce7"
                       : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }
            }

            Text {
                id: logoText
                height: 23
                // Literal, not Theme.accent: at this nesting depth (Main ->
                // VGRPresenterMainScreen -> AppMenuBar) Qt 6.11.1's AOT
                // compiler cannot resolve the Theme singleton at all here,
                // even from an imperative Component.onCompleted reassignment
                // — same root cause as the x/y-binding workaround elsewhere
                // in this codebase. Value matches Theme.accent.
                color: "#6C5CE7"
                font.family: "Outfit"
                font.letterSpacing: 0.09
                font.pixelSize: 18
                font.weight: Font.ExtraBold
                horizontalAlignment: Text.AlignLeft
                text: qsTr("VGRPresenter")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
            }

            MouseArea {
                id: logoArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.toggleMenu("logo")
            }
        }

        Rectangle {
            id: sys_menu

            x: 138
            y: 4

            height: 15
            width: 133

            color: "transparent"

            component MenuLabel: Item {
                id: labelRoot
                property string menuName: ""
                property alias text: label.text
                property int labelWidth: 0

                readonly property bool isOpen: root.openMenu === menuName
                readonly property bool isHover: mouseArea.containsMouse

                height: 15
                width: labelWidth > 0 ? labelWidth : label.implicitWidth

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -4
                    // Literals, not Theme.radiusSm / Theme.textPrimary — see logoText above.
                    radius: 6
                    color: labelRoot.isOpen
                           ? "#1ae2e8f0"
                           : labelRoot.isHover
                           ? "#0de2e8f0"
                           : "transparent"
                    Behavior on color { ColorAnimation { duration: 100 } }
                }

                Text {
                    id: label
                    anchors.verticalCenter: parent.verticalCenter
                    // Literal, not Theme.textPrimary — see logoText above.
                    color: "#e2e8f0"
                    font.family: "Inter"
                    font.pixelSize: 12
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignLeft
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                }

                MouseArea {
                    id: mouseArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggleMenu(labelRoot.menuName)
                }
            }

            MenuLabel { id: file; labelWidth: 21; text: qsTr("File"); menuName: "file" }
            MenuLabel { id: edit; x: 32; labelWidth: 23; text: qsTr("Edit"); menuName: "edit" }
            MenuLabel { id: view; x: 66; labelWidth: 29; text: qsTr("View"); menuName: "view" }
            MenuLabel { id: help; x: 106; labelWidth: 28; text: qsTr("Help"); menuName: "help" }
        }
    }

    DropdownPanel {
        x: 16; y: 48
        visible: root.openMenu === "logo"
        headerTitle: "VGRPresenter"
        headerSubtitle: "v1.0.5-beta.2"
        model: root.logoMenuItems
        onItemActivated: (label) => root.activateItem("logo", label)
    }
    DropdownPanel {
        x: 154; y: 48
        visible: root.openMenu === "file"
        model: root.fileMenuItems
        onItemActivated: (label) => root.activateItem("file", label)
    }
    DropdownPanel {
        x: 186; y: 48
        visible: root.openMenu === "edit"
        model: root.editMenuItems
        onItemActivated: (label) => root.activateItem("edit", label)
    }
    DropdownPanel {
        x: 220; y: 48
        visible: root.openMenu === "view"
        model: root.viewMenuItems
        onItemActivated: (label) => root.activateItem("view", label)
    }
    DropdownPanel {
        x: 260; y: 48
        visible: root.openMenu === "help"
        model: root.helpMenuItems
        onItemActivated: (label) => root.activateItem("help", label)
    }
}
