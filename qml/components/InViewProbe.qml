import QtQuick
import QtQuick.Window

Item {
    id: probe

    // The item whose visibility is judged. Defaults to the item this probe is
    // declared inside — put it in the delegate/card that shows the device.
    property Item target: parent
    // How long `inView` stays true after the item leaves (see the note above).
    property int releaseMs: 1000
    // How often the geometry is re-checked.
    property int pollMs: 250
    // True while the owner may hold its device.
    property bool inView: false

    implicitWidth: 0
    implicitHeight: 0
    visible: false

    // The raw geometric answer, before hysteresis (used by the poll and by
    // tests/diagnostics).
    function geometryHits() {
        const t = probe.target
        if (!t || t.width <= 0 || t.height <= 0)
            return false
        for (let p = t; p; p = p.parent) {
            if (p.visible === false)
                return false
            const viewport = p.clip === true
                || (p.contentY !== undefined && p.contentHeight !== undefined)
            if (viewport) {
                const tl = t.mapToItem(p, 0, 0)
                if (tl.x + t.width <= 0 || tl.x >= p.width
                    || tl.y + t.height <= 0 || tl.y >= p.height)
                    return false
            }
        }
        const win = t.Window.window
        if (!win || !win.visible || win.visibility === Window.Minimized)
            return false
        const wl = t.mapToItem(null, 0, 0)
        if (wl.x + t.width <= 0 || wl.x >= win.width
            || wl.y + t.height <= 0 || wl.y >= win.height)
            return false
        return true
    }

    function evaluate() {
        if (probe.geometryHits()) {
            releaseTail.stop()
            if (!probe.inView)
                probe.inView = true
            return
        }
        if (probe.inView && !releaseTail.running)
            releaseTail.restart()
    }

    Timer {
        interval: probe.pollMs
        repeat: true
        running: true
        onTriggered: probe.evaluate()
    }
    Timer {
        id: releaseTail
        interval: probe.releaseMs
        onTriggered: probe.inView = false
    }
    // First answer now, not one poll from now: whoever holds a device off this
    // waits for that tick otherwise.
    Component.onCompleted: probe.evaluate()
    onTargetChanged: probe.evaluate()
}