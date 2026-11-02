import QtQuick
import VGRPresenterUI

MouseArea {
    id: root

    property Item container: null
    property int hoveredIndex: -1

    property Item zoneRow: null

    hoverEnabled: true

    readonly property var _cells: {
        const result = []
        if (container) {
            for (let i = 0; i < container.children.length; ++i) {
                const c = container.children[i]
                if (c.width > 0 || c.height > 0)
                    result.push(c)
            }
        }
        return result
    }
    readonly property int count: _cells.length

    function zoneAt(x) {
        for (let i = 0; i < count; ++i) {
            if (x < _cells[i].x + _cells[i].width)
                return i
        }
        return count > 0 ? count - 1 : -1
    }

    function _rowLocalX(x) {
        if (!zoneRow || zoneRow === root)
            return x
        return x - root.mapFromItem(zoneRow, 0, 0).x
    }

    function _syncFromTruth() {
        const p = AppCursor.pointerPos(root)
        const z = p ? root.zoneAt(root._rowLocalX(p.x)) : -1
        if (z >= 0) {
            // A live zone: any pending "went out" verdict is obsolete.
            outDebounce.stop()
            if (z !== root.hoveredIndex)
                root.hoveredIndex = z
        } else if (root.hoveredIndex >= 0) {
            if (!outDebounce.running)
                outDebounce.restart()
        }
    }

    // Fires only when OUT has held for the whole window: re-derive once
    // more and reset for real if still outside.
    Timer {
        id: outDebounce
        interval: 120
        onTriggered: {
            const p = AppCursor.pointerPos(root)
            if (!p && root.visible)
                root.hoveredIndex = -1
        }
    }

    // Primary: every catcher point event re-derives the zone. Continuous,
    // event-history-free, and phantom-exit-proof by construction.
    Connections {
        target: AppCursor
        function onPointerMoved() { root._syncFromTruth() }
    }

    // Fallbacks for hosts without the catcher — same derivations through
    // this area's own Qt hover events.
    onEntered: root._syncFromTruth()
    onPositionChanged: root._syncFromTruth()

    onPressed: (mouse) => {
        if (root.hoveredIndex < 0)
            root._syncFromTruth()
    }

    onExited: {
        // Same debounce path as every other OUT reading — never an
        // immediate clear (that was the phantom-exit blink hole).
        root._syncFromTruth()
    }

    onVisibleChanged: {
        outDebounce.stop()
        if (!visible) {
            root.hoveredIndex = -1
            return
        }
        root._syncFromTruth()
    }
}