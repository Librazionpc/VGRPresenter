pragma Singleton
import QtQuick

// 1. Z-ORDER. A flat `z: 30000` on every dialog meant two SIMULTANEOUSLY
// 2. BLUR SOURCE. Every dialog's scrim should blur whatever is DIRECTLY
QtObject {
    id: root
    readonly property int base: 30000
    property int top: base
    function nextZ() {
        root.top += 1
        return root.top
    }

    property Item settingsShell: null
    property var pageLayer: null

    property var stack: []
    function push(cardItem) {
        if (!cardItem || root.stack.indexOf(cardItem) >= 0)
            return
        root.stack = root.stack.concat([cardItem])
    }
    function pop(cardItem) {
        const i = root.stack.indexOf(cardItem)
        if (i < 0)
            return
        const next = root.stack.slice()
        next.splice(i, 1)
        root.stack = next
    }
    function blurSourceFor(cardItem) {
        const i = root.stack.indexOf(cardItem)
        if (i > 0)
            return root.stack[i - 1]
        if (root.settingsShell && root.settingsShell.visible)
            return root.settingsShell
        return root.pageLayer ? root.pageLayer() : null
    }
}