import QtQuick
import QtQuick.Window

// Is the item it is put in ACTUALLY on screen right now?
//
// A device tap — a camera capture, an audio meter, a stream decoder — should be
// held only while something that SHOWS it is really visible. "Selected in a
// roster" is not "being shown": the board can list twenty inputs while six are
// on screen, and a Flickable keeps its delegates alive far outside its viewport,
// so neither `visible` nor the model answers this.
//
// THE GEOMETRY IS THE ANSWER. The target's mapped rect must intersect every
// clipping ancestor's viewport and the window itself, and no ancestor may be
// hidden. (`enabled` is deliberately NOT part of it: a greyed-out row is still
// showing its picture, and treating it as invisible would stop a device whose
// meters the user is watching.)
//
// HYSTERESIS IS THE OTHER HALF. Opening a device costs ~100-300 ms (WASAPI /
// Media Foundation), so a hard in/out gate would open-close-open while the user
// scrolls past a row. `inView` goes true as soon as the geometry says so and
// stays true for `releaseMs` after the item leaves — a fast scroll through a
// long list never starts a device it is already past.
//
// POLLED, not bound: the triggers are scroll positions, window state and
// ancestor visibility, and a binding chain that reads all of them would
// re-evaluate per frame on every row (the same churn this exists to avoid).
// Four cheap checks per second per probe is the whole cost.
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
            // A viewport is anything that scrolls its children: a clipping item,
            // or a Flickable — which is included even when it does NOT clip,
            // because Settings' board draws over its own bounds, so a row
            // scrolled past the viewport overlaps the window while being shown
            // to nobody.
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
