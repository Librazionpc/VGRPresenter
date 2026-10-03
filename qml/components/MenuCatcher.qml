import QtQuick

// Invisible full-window click/wheel catcher under an open menu: any click or
// scroll OUTSIDE the menu closes it (or forwards the wheel for field-attached
// selects) instead of falling through to the page or leaving the menu stuck.
//
// RE-parenting twin of DropdownPanel: openAt() lifts the menu to the window's
// contentItem so nothing clips it and it follows its anchor while pages
// scroll — this catcher lifts itself alongside it. Parented in-place it only
// covered its owner's rect; as a popup's dismiss layer it must cover the
// whole window. Sibling order is normalized by z: catcher (menu.z - 1) below
// the menu — clicks/wheels ON the menu land on the menu, everything else on
// the catcher.
MouseArea {
    id: root

    // The DropdownPanel this catcher dismisses.
    property Item menu

    // FIELD-ATTACHED dropdowns (SelectField's combobox-style menus) set
    // closesOnWheel: false — scrolling the page then scrolls the page (the
    // menu rides along with its field, like a native combobox) instead of
    // insta-closing on the first wheel tick. Right-click CONTEXT menus keep
    // the default true: their anchor is a click point, not a live field, so
    // scroll-away-dismiss is correct for them.
    property bool closesOnWheel: true

    // Resolved from the MENU'S ANCHOR chain (the field the menu belongs to),
    // not from the catcher's own parents: the catcher is reparented to the
    // window root alongside the menu, where no Flickable exists.
    property Flickable wheelTarget: _findFlickable(menu ? menu.anchorItem : null)

    // In-place parent, restored whenever the menu isn't popped out.
    property Item originalParent: null
    Component.onCompleted: originalParent = root.parent
    parent: (menu && menu.parent) ? menu.parent : originalParent
    z: menu ? menu.z - 1 : 0

    function _findFlickable(item) {
        let o = item ? item.parent : null
        while (o) {
            if (o instanceof Flickable)
                return o
            o = o.parent
        }
        return null
    }

    anchors.fill: parent
    acceptedButtons: Qt.AllButtons
    // Click-and-wheel ONLY — deliberately NOT hoverEnabled. This catcher
    // covers the whole window UNDER the open menu, and the window-root
    // AppCursorCatcher (the app's single pointer-position truth source,
    // which every PositionHoverArea reads) sits below it. A hoverEnabled
    // MouseArea is a hover TARGET: hover delivery stops at it, so the
    // catcher below stops receiving point updates and AppCursor's point
    // FREEZES the moment a menu opens over the pointer — the dropdown's own
    // rows then never light (position-truth hover reads a stale point).
    // This is exactly why AppMenuBar's outsideCatcher is click-only; the
    // menus that use THIS catcher (the design library's card/category menus,
    // SelectField dropdowns) were the ones whose rows showed no hover tint.
    // Presses and wheels do not need hover, so nothing here regresses.
    hoverEnabled: false
    visible: menu ? menu.visible : false
    enabled: visible

    onPressed: menu.visible = false
    onWheel: (wheel) => {
        // Wheels OVER THE MENU ITSELF are the menu's business (its own list
        // scroll) — never forward them to the page/dialog behind it. The
        // menu's inner Flickable handles what it can; anything it declines
        // (short list that fits, header/padding strip, list already at its
        // end) stops here instead of falling through to the dialog — that
        // fall-through is what made the edit popup scroll while scrolling
        // the device dropdown.
        if (menu && menu.visible) {
            const mp = mapToItem(menu, wheel.x, wheel.y)
            if (mp.x >= 0 && mp.y >= 0 && mp.x < menu.width && mp.y < menu.height) {
                wheel.accepted = true
                return
            }
        }
        // Wheels over the ANCHOR CONTROL (the open combobox's value box)
        // scroll the menu's options — native combobox behavior. The box
        // itself sits below this full-window catcher, so without this branch
        // those wheels would fall through to `wheelTarget` and scroll the
        // dialog the field lives in (the "parent popup scrolls too" bug).
        if (menu && menu.anchorItem) {
            const ap = mapToItem(menu.anchorItem, wheel.x, wheel.y)
            if (ap.x >= 0 && ap.y >= 0 && ap.x < menu.anchorItem.width
                    && ap.y < menu.anchorItem.height) {
                wheel.accepted = true
                // Raw delta — wheel-down is negative and must scroll the
                // menu downward (scrollList subtracts; see its sign
                // convention note). The old extra negation inverted it.
                menu.scrollList(wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y
                                                         : wheel.angleDelta.y / 3)
                return
            }
        }
        if (closesOnWheel || !wheelTarget) {
            wheel.accepted = true
            menu.visible = false
            return
        }
        // Scroll the anchor's page/dialog manually — Flickable has no public
        // scroll() API, and a synthetic flick would fight the user's input.
        // Wheel ticks (angleDelta.y, ±120 per notch) map to a notch of ~40px.
        // Both axes honored so shift-wheel / trackpads behave. The menu then
        // FOLLOWS its moving anchor (DropdownPanel's follow logic).
        wheel.accepted = true
        const step = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y
                                              : wheel.angleDelta.y / 3
        const maxY = Math.max(0, wheelTarget.contentHeight - wheelTarget.height)
        wheelTarget.contentY = Math.max(0, Math.min(maxY, wheelTarget.contentY - step))
        const stepX = wheel.pixelDelta.x !== 0 ? wheel.pixelDelta.x
                                               : wheel.angleDelta.x / 3
        const maxX = Math.max(0, wheelTarget.contentWidth - wheelTarget.width)
        if (maxX > 0)
            wheelTarget.contentX = Math.max(0, Math.min(maxX, wheelTarget.contentX - stepX))
    }
}
