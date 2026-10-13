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
//   AppCursor.push(shape, owner, priority) — owner-scoped; re-pushing the same
//                                    owner replaces its entry (no dupes); the
//                                    highest priority wins (ties: later push)
//   AppCursor.pop(owner)           — remove just this owner's entry
// Owners are any stable object (the MouseArea/item driving the gesture) —
// they exist precisely to make pop() idempotent and leak-proof: forgetting
// to pop on one path can't poison the stack for everyone else.
//    // (Priorities: a non-empty stack's winner is _top above.)
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

    // [{ shape: int, owner: QtObject, priority: int }] — push order, but the
    // WINNER is the highest priority (ties: the later push). See push().
    property var _stack: []

    // ---- push priorities -----------------------------------------------------
    // Push ORDER alone was never enough, because every pusher re-pushes on each
    // pointer move: an item's content area (I-beam / pointing hand) and the
    // resize dot sitting on top of it are BOTH hovered at the same time, and
    // which one won came down to the order their Connections were created in —
    // which left the I-beam winning over the resize arrow on every corner dot
    // (user report: "the cursor doesn't change when moving or resize"). The
    // claim is explicit now:
    //   PRESSED  — a live drag owns the pointer outright.
    //   HANDLE   — a resize dot is the finer target than the body under it.
    //   HOVER    — content/bodies, pointing hands, the I-beam over a field.
    //   FALLBACK — a bare panel/canvas asserting the plain arrow over empty
    //              space, which must never shadow a real affordance.
    readonly property int priorityPressed: 2
    readonly property int priorityHandle: 1
    readonly property int priorityHover: 0
    readonly property int priorityFallback: -1

    // Flip to true (e.g. from a probe or a temporary edit) to trace every
    // push/pop/validate live on stderr — the tool for the next "cursor got
    // stuck" report that can't be reproduced on demand.
    property bool debug: false

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
    // False whenever the pointer is known to be outside the window (the
    // catcher's HoverHandler unhovers — one of the few hover transitions
    // that DOES deliver reliably). Position-driven consumers read this too,
    // so no hover wash outlives the pointer leaving the window even though
    // _point itself would still map inside.
    property bool windowHovered: true
    // Emitted on every pointer position update AND when the position is
    // cleared — the tick position-truth consumers (PositionHoverArea)
    // recompute from, replacing the enter/exit MouseArea events this build
    // delivers unreliably (enter arrives, exit never does).
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
    // Emitted when a blocker is registered or dropped, so the PositionHoverAreas
    // a newly-opened popup covers recompute AT ONCE — they otherwise recompute
    // only on pointer movement, which leaves a card washed while the pointer
    // sits still over the menu.
    signal blockersChanged()

    // The open popups/menus that CLAIM the pointer while visible (DropdownPanel
    // registers itself). hovered() above is pure geometry and cannot see that
    // something is painted ON TOP of an item, so without this an open context
    // menu over a design card left the CARD reporting hovered too (its border
    // lit, its action row showed) simply because the pointer was over its rect —
    // the menu did not behave as its own popup. A blocker's OWN rows are exempt
    // (a row is a descendant of its panel), so menus keep their row hover.
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

    // `priority` defaults to priorityHover — a plain hover claim. See the
    // priority block above for who should pass what.
    function push(shape, owner, priority) {
        if (!owner)
            return
        _validate()
        // A CLAIM FROM UNDER AN OPEN POPUP IS NOT A CLAIM. pushBlocker() marks the
        // popups that own the pointer (see blocked()), and while such a popup
        // covers an item the pointer is not on that item however its geometry
        // reads — so its cursor claim is dropped here, once, instead of every
        // hover site having to remember to ask. This is the general form of the
        // metadata palette report: the canvas object beneath the palette kept
        // pushing the I-beam (EditableCanvasLabel pushes from hovered() pure
        // geometry, and its area has no idea something is painted over it), so
        // the palette's own pointing hand never showed and the panel read as
        // "not really lifted above the canvas".
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
