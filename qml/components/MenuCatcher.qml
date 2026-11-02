import QtQuick

MouseArea {
    id: root

    // The DropdownPanel this catcher dismisses.
    property Item menu

    property bool closesOnWheel: true

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
    hoverEnabled: false
    visible: menu ? menu.visible : false
    enabled: visible

    onPressed: (mouse) => {
        if (!menu || !menu.visible)
            return
        const point = root.mapToItem(menu, mouse.x, mouse.y)
        if (point.x >= 0 && point.y >= 0 && point.x < menu.width && point.y < menu.height) {
            mouse.accepted = true
            return
        }
        menu.visible = false
    }
    onWheel: (wheel) => {
        if (menu && menu.visible) {
            const mp = mapToItem(menu, wheel.x, wheel.y)
            if (mp.x >= 0 && mp.y >= 0 && mp.x < menu.width && mp.y < menu.height) {
                wheel.accepted = true
                return
            }
        }
        if (menu && menu.anchorItem) {
            const ap = mapToItem(menu.anchorItem, wheel.x, wheel.y)
            if (ap.x >= 0 && ap.y >= 0 && ap.x < menu.anchorItem.width
                    && ap.y < menu.anchorItem.height) {
                wheel.accepted = true
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