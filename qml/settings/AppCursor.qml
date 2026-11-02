pragma Singleton
import QtQuick

QtObject {
    id: root

    // [{ shape: int, owner: QtObject, priority: int }] — push order, but the
    // WINNER is the highest priority (ties: the later push). See push().
    property var _stack: []

    // ---- push priorities -----------------------------------------------------
    readonly property int priorityPressed: 2
    readonly property int priorityHandle: 1
    readonly property int priorityHover: 0
    readonly property int priorityFallback: -1

    property bool debug: false

    property var _point: null
    readonly property var point: _point
    property bool windowHovered: true
    signal pointerMoved()
    function setPointerPos(pos) {
        _point = pos
        windowHovered = true
        pointerMoved()
    }
    // Pointer left the window (or hover was otherwise lost): drop position
    // truth so nothing can read a stale point as "still hovered".
    function clearPointerPos() {
        _point = null
        windowHovered = false
        pointerMoved()
    }

    // ---- pointer blockers (open popups claim the pointer) --------------------
    signal blockersChanged()

    property var _blockers: []
    function pushBlocker(item) {
        if (!item || _blockers.indexOf(item) >= 0)
            return
        _blockers = _blockers.concat([item])
        blockersChanged()
    }
    function popBlocker(item) {
        const i = _blockers.indexOf(item)
        if (i < 0)
            return
        const kept = _blockers.slice()
        kept.splice(i, 1)
        _blockers = kept
        blockersChanged()
    }
    // Is `item` itself, or one of its ancestors, `top`?
    function _isSelfOrAncestorOf(top, item) {
        let o = item
        while (o) {
            if (o === top)
                return true
            o = o.parent
        }
        return false
    }
    // True when a VISIBLE blocker covers the pointer and `item` is not part of
    // it — neither the blocker itself nor one of its descendants.
    function blocked(item) {
        if (!item || _blockers.length === 0)
            return false
        for (let i = 0; i < _blockers.length; ++i) {
            const b = _blockers[i]
            // Effectively shown, not merely visible: true — a panel left open
            // inside a hidden pane must not claim the pointer for its rect.
            if (!b || !_eligible(b))
                continue
            if (_isSelfOrAncestorOf(b, item) || _isSelfOrAncestorOf(item, b))
                continue
            if (hovered(b))
                return true
        }
        return false
    }

    function hovered(item) {
        if (!_point || !item)
            return false
        try {
            const p = item.mapFromItem(null, _point.x, _point.y)
            return p.x >= 0 && p.y >= 0 && p.x <= item.width && p.y <= item.height
        } catch (err) {
            return false
        }
    }

    function pointerPos(item) {
        if (!_point || !item)
            return null
        try {
            const p = item.mapFromItem(null, _point.x, _point.y)
            if (p.x < 0 || p.y < 0 || p.x > item.width || p.y > item.height)
                return null
            return p
        } catch (err) {
            return null
        }
    }

    // The winning entry: highest priority, and among equals the LATER push.
    readonly property var _top: {
        let best = null
        for (let i = 0; i < _stack.length; ++i) {
            const e = _stack[i]
            const p = e.priority === undefined ? 0 : e.priority
            if (best === null || p >= (best.priority === undefined ? 0 : best.priority))
                best = e
        }
        return best
    }
    readonly property int shape: _top ? _top.shape : Qt.ArrowCursor
    readonly property bool active: _stack.length > 0

    function _eligible(owner) {
        try {
            let o = owner
            while (o) {
                if (o.visible === false || o.enabled === false)
                    return false
                o = o.parent
            }
            return true
        } catch (err) {
            return false
        }
    }

    function _validate() {
        if (_stack.length === 0)
            return
        const kept = _stack.filter((e) => {
            // No visible/enabled properties to inspect (plain QtObject):
            // keep it, it polices itself.
            if (e.owner && e.owner.visible === undefined)
                return true
            return _eligible(e.owner)
        })
        if (kept.length !== _stack.length) {
            if (root.debug)
                console.log("[AppCursor] validate dropped",
                            _stack.length - kept.length, "stale entr(s)")
            _stack = kept
        }
    }

    // `priority` defaults to priorityHover — a plain hover claim. See the
    // priority block above for who should pass what.
    function push(shape, owner, priority) {
        if (!owner)
            return
        _validate()
        if (blocked(owner))
            return
        const kept = _stack.filter((e) => e.owner !== owner)
        kept.push({ shape: shape, owner: owner,
                    priority: priority === undefined ? root.priorityHover : priority })
        _stack = kept
        if (root.debug)
            console.log("[AppCursor] push", shape, "prio", priority === undefined ? 0 : priority,
                        "depth", _stack.length, "owner", owner, "stack=", _stack.map((e) => e.owner))
    }

    function pop(owner) {
        if (!owner)
            return
        if (root.debug)
            console.log("[AppCursor] pop attempt", "owner", owner, "wasInStack", _stack.some((e) => e.owner === owner))
        const before = _stack.length
        _stack = _stack.filter((e) => e.owner !== owner)
        if (root.debug && _stack.length !== before)
            console.log("[AppCursor] pop", "depth", _stack.length)
    }
}