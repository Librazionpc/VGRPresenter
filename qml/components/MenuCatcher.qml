import QtQuick

// Invisible click-catcher under an open context menu: any click or scroll
// while the menu is open closes it instead of falling through to the page
// (or leaving the menu "stuck" floating over content that keeps scrolling
// under it). Declare it BEFORE the menu it serves (sibling order puts the
// menu on top), and raise the menu above it with z — the menu's own clicks
// land on the menu, everything else lands here and dismisses.
MouseArea {
    id: root

    // The DropdownPanel this catcher dismisses.
    property Item menu

    anchors.fill: parent
    acceptedButtons: Qt.AllButtons
    hoverEnabled: true
    visible: menu ? menu.visible : false
    enabled: visible

    onPressed: menu.visible = false
    onWheel: (wheel) => {
        wheel.accepted = true
        menu.visible = false
    }
}
