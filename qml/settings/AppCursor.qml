pragma Singleton
import QtQuick

// App-wide cursor override — one stack, many owners. Any gesture or hover
// that needs to FORCE a cursor (canvas resize handles, item dragging, the
// AV board's line-drag) pushes its shape here and pops it when done; the
// topmost entry wins.
//
// Why this exists: per-MouseArea `cursorShape` properties are unreliable in
// exactly the cases that matter. During a press-drag the shape that applied
// at hover time keeps getting lost (the OS cursor reverts to Arrow while
// the drag is live), and overlapping hoverEnabled MouseAreas re-arbitrate
// under the cursor in ways that shadow each other as items move/resized
// under the pointer. A single topmost catcher (see Main.qml) rendering
// `AppCursor.shape` removes the arbitration entirely: while the stack is
// non-empty, nothing under the pointer can override the pushed shape.
//
// Contract:
//   AppCursor.push(shape, owner)   — owner-scoped; re-pushing the same
//                                    owner replaces its entry (no dupes)
//   AppCursor.pop(owner)           — remove just this owner's entry
// Owners are any stable object (the MouseArea/item driving the gesture) —
// they exist precisely to make pop() idempotent and leak-proof: forgetting
// to pop on one path can't poison the stack for everyone else.
//
// SELF-HEALING: every push() first validates the WHOLE stack and drops any
// entry whose owner is no longer cursor-eligible — hidden, disabled, or
// parented under something hidden/disabled. An invisible/disabled MouseArea
// stops receiving hover events (the trap behind the "stuck I-beam" bug), so
// its own onVisibleChanged/onEnabledChanged pop is the ONLY cleanup it can
// perform; this validation catches every path where that pop never ran or
// never will — present pushers and future ones alike — the first time ANY
// other pusher touches the stack. Owners that are plain QtObjects (no
// visible/enabled to inspect) are kept and remain responsible for
// themselves.
//
// While the stack is empty the whole mechanism is inert (the catcher in
// Main.qml renders ArrowCursor and every component's own cursorShape
// bindings apply normally), so this changes nothing for plain hover cursors.
QtObject {
    id: root

    // [{ shape: int, owner: QtObject }] — bottom-to-top; last entry wins.
    property var _stack: []

    // Flip to true (e.g. from a probe or a temporary edit) to trace every
    // push/pop/validate live on stderr — the tool for the next "cursor got
    // stuck" report that can't be reproduced on demand.
    property bool debug: true

    // The window-scene pointer position, fed every event by the catcher's
    // permanently-latched HoverHandler (see AppCursorCatcher.qml). This is
    // POSITION TRUTH — unlike a freshly enabled/shown MouseArea's
    // containsMouse, which stays false until the mouse physically moves,
    // this is current even when the pointer is perfectly stationary. Push
    // sites use hovered() to decide "am I under the pointer" at the exact
    // moment they appear (edit starting, drag area re-shown after an edit,
    // handles re-enabled on select), which is when containsMouse-based
    // logic provably misses.
    property var _point: null
    function setPointerPos(pos) { _point = pos }

    // Is the current pointer position inside `item` (its local geometry)?
    // Deterministic, no event history involved. Destroyed/dead items read
    // as not-hovered (same defensive posture as _eligible).
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

    // Same position-truth source as hovered(), but returns WHERE inside
    // `item` (local coordinates), not just whether — lets a consumer that
    // tracks WHICH CHILD is hovered (not just hovered/not) recover its
    // state the moment it appears under an already-stationary pointer, the
    // same class of gap hovered() exists to close. null when unknown or the
    // point falls outside item's bounds.
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

    readonly property int shape: _stack.length > 0 ? _stack[_stack.length - 1].shape : Qt.ArrowCursor
    readonly property bool active: _stack.length > 0

    // An owner can only legitimately hold the cursor while it (and every
    // ancestor) is visible and enabled — the same conditions under which it
    // still receives the hover events that let it pop itself. A destroyed
    // owner (deleted without its onDestruction pop ever running) reads as
    // ineligible too — touching its properties throws, which lands here.
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

    function push(shape, owner) {
        if (!owner)
            return
        _validate()
        const kept = _stack.filter((e) => e.owner !== owner)
        kept.push({ shape: shape, owner: owner })
        _stack = kept
        if (root.debug)
            console.log("[AppCursor] push", shape, "depth", _stack.length, "owner", owner, "stack=", _stack.map((e) => e.owner))
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
