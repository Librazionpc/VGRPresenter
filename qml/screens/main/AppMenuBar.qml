import QtQuick
import VGRPresenterUI
import "../../components"

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

    signal settingsRequested(string section)

    signal newShowRequested()
    // Show file actions — the engine reads/writes the .vgr (see ShowSession.qml).
    signal openShowRequested()
    signal saveShowRequested()
    signal saveShowAsRequested()
    signal quickSearchRequested()
    // File > Import: the Import screen (every file format the engine reads).
    signal importRequested()
    signal recordToggled()
    function activateItem(menuName, label) {
        closeMenu()
        if (label === "Settings" || label === "Preferences…")
            root.settingsRequested("general")
        else if (label === "New show")
            root.newShowRequested()
        else if (label === "Open project…")
            root.openShowRequested()
        else if (label === "Save")
            root.saveShowRequested()
        else if (label === "Save As…")
            root.saveShowAsRequested()
        else if (label === "Quick search…")
            root.quickSearchRequested()
        else if (label === "Import…")
            root.importRequested()
        else if (label === "Record…")
            root.recordToggled()
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
        { label: "Settings", trailing: "›" },
        { label: "About VGRPresenter" },
        { label: "Exit" }
    ]

    readonly property var settingsSubmenuItems: [
        { label: "General", key: "general" },
        { label: "Smart Config", key: "smart" },
        // Screens are left out of Settings for now (see NavRail.qml).
        // { label: "Screens", key: "outputs" },
        { label: "Styles", key: "styles" },
        { label: "Audio & Video", key: "av" },
        { label: "Routing", key: "routing" },
        { label: "Recording", key: "recording" },
        { label: "Plugins", key: "plugins" }
    ]
    property bool settingsRowHovered: false
    onSettingsRowHoveredChanged: recomputeSettingsFlyout()
    property bool settingsFlyoutHovered: false
    onSettingsFlyoutHoveredChanged: recomputeSettingsFlyout()
    // The row item from the last "Settings" hover — lets the click-pin
    // path position the flyout without re-deriving the row's geometry.
    property var lastSettingsRow: null
    property bool settingsFlyoutOpen: false
    property bool flyoutRecomputing: false
    property bool flyoutRecomputeQueued: false
    function recomputeSettingsFlyout() {
        if (flyoutRecomputing) {
            flyoutRecomputeQueued = true
            return
        }
        flyoutRecomputing = true
        let settle = 0
        do {
            flyoutRecomputeQueued = false
            settingsFlyoutOpen = root.openMenu === "logo"
                && (root.settingsPinned
                    || root.settingsRowHovered
                    || root.settingsFlyoutHovered
                    || (root.settingsFlyoutVisible && AppCursor.hovered(settingsFlyout)))
        } while (flyoutRecomputeQueued && ++settle < 8)
        flyoutRecomputing = false
    }
    property bool settingsPinned: false
    onSettingsPinnedChanged: recomputeSettingsFlyout()

    property bool settingsFlyoutVisible: false
    onSettingsFlyoutOpenChanged: {
        if (settingsFlyoutOpen) {
            settingsCloseTimer.stop()
            root.settingsFlyoutVisible = true
        } else {
            settingsCloseTimer.restart()
        }
    }
    onOpenMenuChanged: {
        if (root.openMenu !== "logo") {
            settingsCloseTimer.stop()
            root.settingsFlyoutVisible = false
            root.settingsPinned = false
            root.settingsRowHovered = false
            root.settingsFlyoutHovered = false
        }
        root.recomputeSettingsFlyout()
    }
    Timer {
        id: settingsCloseTimer
        interval: 300
        onTriggered: root.settingsFlyoutVisible = false
    }
    // Pointer moves re-derive the keep-open state from geometry — the
    // deterministic path that never depends on enter/exit events.
    Connections {
        target: AppCursor
        function onPointerMoved() { root.recomputeSettingsFlyout() }
    }

    function positionSettingsFlyout(rowItem) {
        const p = rowItem.mapToItem(root, rowItem.width, 0)
        settingsFlyout.x = p.x
        settingsFlyout.y = p.y
    }

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
        { label: "Screens", trailing: "›" }
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
        { label: "About VGRPresenter", trailing: qsTr("v%1").arg(SettingsService.appVersion) }
    ]

    MouseArea {
        id: outsideCatcher
        anchors.fill: parent
        enabled: root.openMenu !== ""
        hoverEnabled: false
        onClicked: root.closeMenu()
    }

    Rectangle {
        id: header_left

        x: 16
        y: 4

        height: 23
        width: sys_menu.x + sys_menu.width

        color: "transparent"

        Item {
            id: logoButton
            // Self-test handle: the logo is what opens the menu whose
            // "Settings" row the binding-loop scenario hovers.
            objectName: "selfTestMenuLogo"
            width: logoText.implicitWidth
            height: parent.height

            readonly property bool isOpen: root.openMenu === "logo"
            readonly property bool isHover: logoHover.hovered

            Rectangle {
                anchors.fill: parent
                anchors.margins: -4
                // Radius + accent tint read the Theme singleton now (see
                // logoText below for why this was literal before).
                radius: 6 // Theme.radiusSm
                color: logoButton.isOpen || logoButton.isHover
                       ? Qt.alpha(Theme.accent, 0.1)
                       : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            }

            Row {
                id: logoText
                height: 23
                spacing: 0
                Text {
                    height: 23
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.letterSpacing: 0.09
                    font.pixelSize: 21
                    font.weight: Font.ExtraBold
                    horizontalAlignment: Text.AlignLeft
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("VGR")
                    textFormat: Text.PlainText
                }
                Text {
                    height: 23
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.letterSpacing: 0.09
                    font.pixelSize: 21
                    font.weight: Font.ExtraBold
                    horizontalAlignment: Text.AlignLeft
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Presenter")
                    textFormat: Text.PlainText
                }
            }

            PositionHoverArea {
                id: logoHover
                anchors.fill: parent
                anchors.margins: -4
                onClicked: root.toggleMenu("logo")
            }
        }

        Rectangle {
            id: sys_menu

            // Was a fixed 138 (a Figma-export pixel-pin): the logo's own font size grew and started overlapping it. Placed
            // relative to the logo's ACTUAL rendered width instead, so a font (or text) change here can never collide with it again.
            x: logoButton.width + 24
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
                // Position truth — see logoButton.isHover above.
                readonly property bool isHover: hoverArea.hovered

                height: 15
                width: labelWidth > 0 ? labelWidth : label.implicitWidth

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -4
                    // Radius + the textPrimary hover washes read the Theme
                    // singleton (see logoText above).
                    radius: 6 // Theme.radiusSm
                    color: labelRoot.isOpen
                           ? Qt.alpha(Theme.textPrimary, 0.1)
                           : labelRoot.isHover
                           ? Qt.alpha(Theme.textPrimary, 0.05)
                           : "transparent"
                    // Same soft fade as the logo pill above.
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                }

                Text {
                    id: label
                    anchors.verticalCenter: parent.verticalCenter
                    // Reads Theme.textPrimary — see logoText above.
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignLeft
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignTop
                }

                PositionHoverArea {
                    id: hoverArea
                    anchors.fill: parent
                    anchors.margins: -4
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
        id: logoPanel
        x: 16
        y: header_left.y + logoButton.y + logoButton.height + 4
        fitContentWidth: true
        visible: root.openMenu === "logo"
        headerTitle: "VGRPresenter"
        headerSubtitle: qsTr("v%1").arg(SettingsService.appVersion)
        model: root.logoMenuItems
        onItemActivated: (label) => {
            if (label === "Settings") {
                if (root.lastSettingsRow)
                    root.positionSettingsFlyout(root.lastSettingsRow)
                root.settingsPinned = !root.settingsPinned
                return
            }
            root.activateItem("logo", label)
        }
        onItemHovered: (label, hovering, rowItem) => {
            if (label !== "Settings")
                return
            if (hovering) {
                root.lastSettingsRow = rowItem
                root.positionSettingsFlyout(rowItem)
            }
            root.settingsRowHovered = hovering
        }
    }

    DropdownPanel {
        id: settingsFlyout
        // Self-test handle: the scenario reads this panel's own `visible` as
        // evidence the hover sequence really flew the flyout out.
        objectName: "selfTestSettingsFlyout"
        visible: root.settingsFlyoutVisible
        fitContentWidth: true
        model: root.settingsSubmenuItems
        onItemActivated: (label) => {
            const item = root.settingsSubmenuItems.find((i) => i.label === label)
            root.closeMenu()
            if (item)
                root.settingsRequested(item.key)
        }
        onItemHovered: (label, hovering) => root.settingsFlyoutHovered = hovering
    }
    Shortcut {
        sequence: "Esc"
        enabled: root.openMenu === "logo" && root.settingsPinned
        onActivated: root.settingsPinned = false
    }
    DropdownPanel {
        x: header_left.x + sys_menu.x + file.x
        y: header_left.y + sys_menu.y + file.y + file.height + 4
        fitContentWidth: true
        visible: root.openMenu === "file"
        model: root.fileMenuItems
        onItemActivated: (label) => root.activateItem("file", label)
    }
    DropdownPanel {
        x: header_left.x + sys_menu.x + edit.x
        y: header_left.y + sys_menu.y + edit.y + edit.height + 4
        fitContentWidth: true
        visible: root.openMenu === "edit"
        model: root.editMenuItems
        onItemActivated: (label) => root.activateItem("edit", label)
    }
    DropdownPanel {
        x: header_left.x + sys_menu.x + view.x
        y: header_left.y + sys_menu.y + view.y + view.height + 4
        fitContentWidth: true
        visible: root.openMenu === "view"
        model: root.viewMenuItems
        onItemActivated: (label) => root.activateItem("view", label)
    }
    DropdownPanel {
        x: header_left.x + sys_menu.x + help.x
        y: header_left.y + sys_menu.y + help.y + help.height + 4
        fitContentWidth: true
        visible: root.openMenu === "help"
        model: root.helpMenuItems
        onItemActivated: (label) => root.activateItem("help", label)
    }
}