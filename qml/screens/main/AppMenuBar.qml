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

    // Emitted for the two menu items that open the Settings dialog (logo
    // menu's "Settings", Edit menu's "Preferences…") — Main.qml owns the
    // dialog and shows it on this signal. `section` is a ModalShell nav key
    // ("general", "smart", …) — the plain click and Preferences… both still
    // just land on "general"; the Settings row's hover flyout (below) is
    // the only thing that ever passes a specific one.
    signal settingsRequested(string section)

    // Menu actions are presentation-only for now (the underlying commands
    // don't exist yet) — except Settings/Preferences, which are live, and
    // "New show" (both menus list it), which funnels into the same deck
    // reset as the Show screen's New show buttons.
    signal newShowRequested()
    // Show file actions — the engine reads/writes the .vgr (see ShowSession.qml).
    signal openShowRequested()
    signal saveShowRequested()
    signal saveShowAsRequested()
    signal quickSearchRequested()
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

    // Same seven sections ModalShell's own nav rail lists (kept in sync by
    // hand — there's no shared source both a QML nav list and this menu's
    // plain {label,key} shape can pull from without one depending on the
    // other's internal structure). Hovering "Settings" flies this out
    // instead of making every settings section a two-step trip through the
    // dialog's own nav rail.
    readonly property var settingsSubmenuItems: [
        { label: "General", key: "general" },
        { label: "Smart Config", key: "smart" },
        { label: "Outputs", key: "outputs" },
        { label: "Styles", key: "styles" },
        { label: "Audio & Video", key: "av" },
        { label: "Recording", key: "recording" },
        { label: "Plugins", key: "plugins" }
    ]
    // Open while EITHER the "Settings" row or the flyout itself is
    // hovered — OR the flyout is PINNED by clicking the "Settings" row
    // (see onItemActivated below). Hover alone was too fragile to rely on:
    // the row and the flyout are two separate, non-overlapping items, so
    // the cursor crosses a real gap moving from one to the other — a
    // diagonal move (the natural way to reach a lower item) samples a
    // mouse position outside BOTH areas for at least a frame. That flips
    // both flags false, which used to hide the flyout immediately; once
    // hidden, its rows stop receiving hover events at all, so it could
    // never recover even once the cursor actually arrived.
    // settingsFlyoutVisible below is the same signal with a short grace
    // period on the way to false — and the click-pin is the deterministic
    // path that doesn't depend on hover timing at all.
    property bool settingsRowHovered: false
    property bool settingsFlyoutHovered: false
    // The row item from the last "Settings" hover — lets the click-pin
    // path position the flyout without re-deriving the row's geometry.
    property var lastSettingsRow: null
    readonly property bool settingsFlyoutOpen: root.openMenu === "logo"
                                                && (root.settingsPinned
                                                    || root.settingsRowHovered
                                                    || root.settingsFlyoutHovered)
    // Clicking "Settings" pins the flyout open (click again to unpin) —
    // so the section list is selectable at leisure, not in a race against
    // the hover grace timer. Hover still previews it.
    property bool settingsPinned: false

    property bool settingsFlyoutVisible: false
    onSettingsFlyoutOpenChanged: {
        if (root.settingsFlyoutOpen) {
            settingsCloseTimer.stop()
            root.settingsFlyoutVisible = true
        } else {
            settingsCloseTimer.restart()
        }
    }
    // The whole menu bar closing (outside click, an item activated) drops
    // the flyout instantly AND clears every hover/pin flag: hiding a panel
    // swallows the hover-exit events its rows would have delivered (the
    // same invisible-item trap the cursor stack had), so a flag left true
    // here would instantly re-show the flyout — unpositioned — the next
    // time the logo menu opened.
    onOpenMenuChanged: if (root.openMenu !== "logo") {
        settingsCloseTimer.stop()
        root.settingsFlyoutVisible = false
        root.settingsPinned = false
        root.settingsRowHovered = false
        root.settingsFlyoutHovered = false
    }
    Timer {
        id: settingsCloseTimer
        interval: 300
        onTriggered: root.settingsFlyoutVisible = false
    }

    // Positions the flyout just to the right of the hovered row, in THIS
    // item's coordinate space (where settingsFlyout.x/y actually apply) —
    // plain assignment, not a binding, so it only moves on hover-enter
    // rather than fighting `visible`'s own binding below.
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
    // without intercepting input at all when no menu is open. Click-only:
    // hover delivery is broken app-wide (KNOWN_ISSUES.md) and this catcher
    // needs no hover state of its own.
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
        // Top-anchored, not vertically centered: the logo + File/Edit/View/
        // Help row belongs at the window's top edge (standard menu-bar
        // position — user call), not floating mid-strip beside the tabs.
        y: 4

        height: 23
        width: 271

        color: "transparent"

        Item {
            id: logoButton
            width: logoText.implicitWidth
            height: parent.height

            readonly property bool isOpen: root.openMenu === "logo"
            // Position truth, not containsMouse — hover-exit never delivers
            // to MouseAreas in this build (KNOWN_ISSUES.md), so a containsMouse
            // wash would stick forever. PositionHoverArea derives hover from
            // the AppCursorCatcher's pointer-position stream, which exits
            // flawlessly the moment the pointer moves off.
            readonly property bool isHover: logoHover.hovered

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

            PositionHoverArea {
                id: logoHover
                anchors.fill: parent
                anchors.margins: -4
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
                // Position truth — see logoButton.isHover above.
                readonly property bool isHover: hoverArea.hovered

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
        x: 16; y: 48
        visible: root.openMenu === "logo"
        headerTitle: "VGRPresenter"
        headerSubtitle: "v1.0.5-beta.2"
        model: root.logoMenuItems
        onItemActivated: (label) => {
            // "Settings" doesn't activate — it PINS the section-list flyout
            // open (hover still previews it). Activating straight into the
            // dialog bypassed the list entirely, so a click could never
            // reach e.g. Outputs or Styles without hover gymnastics first.
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

    // Settings' hover flyout — a second, independent DropdownPanel (not
    // nested inside the logo menu's) so it isn't clipped by that panel's
    // own `clip: true`; positioned imperatively via positionSettingsFlyout.
    DropdownPanel {
        id: settingsFlyout
        visible: root.settingsFlyoutVisible
        model: root.settingsSubmenuItems
        onItemActivated: (label) => {
            const item = root.settingsSubmenuItems.find((i) => i.label === label)
            root.closeMenu()
            if (item)
                root.settingsRequested(item.key)
        }
        onItemHovered: (label, hovering) => root.settingsFlyoutHovered = hovering
    }
    // Keyboard affordance matching the pinned flyout: Escape (with the
    // logo menu open) unpins first, closes the menu only on a second
    // press — so pinning never feels like a trap.
    Shortcut {
        sequence: "Esc"
        enabled: root.openMenu === "logo" && root.settingsPinned
        onActivated: root.settingsPinned = false
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
