import QtQuick
import VGRPresenterUI

// A single MouseArea that reports which child of `container` (a Row/Grid/
// etc, sized to exactly cover with anchors.fill) the cursor is currently
// over, via hoveredIndex (-1 when the cursor is outside it entirely). For
// any row of items that needs hover-highlight tracking — a "+" content-type
// picker, a segmented style selector, a tab strip, etc.
//
// The problem this solves: the naive approach is one hoverEnabled MouseArea
// per item. That's fragile exactly at the boundary between two adjacent
// items — however precisely each item's hit area is sized, there's a
// moment crossing from one to the next where either both or neither
// reports containsMouse, which reads as the highlight blinking off between
// items. One MouseArea computing "which item is under the cursor" has no
// boundary for two separate areas to race at.
//
// zoneAt reads each child's actual x/width from `container` rather than
// dividing the MouseArea's own width evenly by count — a row with spacing
// between items (or non-equal item widths) does NOT divide evenly, and
// pretending it does drifts the computed boundary away from the item's
// real edge with every item, eventually landing the boundary inside an
// item's own visible bounds instead of in the gap after it. Right at that
// misaligned point, ordinary mouse jitter flips hoveredIndex back and forth
// even while the cursor sits still over one item, reading as the highlight
// rapidly blinking on and off.
//
// PRIMARY TRUTH SOURCE — AppCursor's point stream, not this area's own
// event history. The window-root AppCursorCatcher feeds AppCursor the
// pointer position on every hover event; deriving hoveredIndex from
// AppCursor.pointerPos(this) makes the highlight a pure function of where
// the pointer IS. That matters because Qt's per-item hover delivery has a
// proven phantom-exit failure mode under synthetic/odd input: a
// WM_MOUSELEAVE can arrive while the pointer is demonstrably still parked
// inside the area (caught live by the self-test probe: entered → exited in
// the same millisecond with pointer truth inside — see KNOWN_ISSUES.md).
// Trusting that exit would blink the highlight off; deriving from position
// truth instead makes any such phantom exit self-heal on the spot.
//
// The Qt hover handlers (entered/positionChanged/exited) are kept as a
// fallback for hosts without the catcher (standalone probes), and the
// exited handler itself refuses to clear the index while pointer truth
// still says inside.
//
// Usage: size this to exactly cover the hover REGION (see zoneRow below —
// normally a parent menu/card that fully contains the row, so the padding
// around the row stays live hover territory instead of dead zones), point
// `container` and `zoneRow` at the row itself, read `hoveredIndex` for which
// one to highlight, and use onClicked (already a MouseArea signal, since
// this type IS one) with hoveredIndex to know which one was clicked. If the
// area is sized to the row exactly, zoneRow can stay null and everything
// behaves as before.
MouseArea {
    id: root

    property Item container: null
    property int hoveredIndex: -1

    // The row the zones are computed from, when this area covers MORE than
    // the row (e.g. the whole popover menu). Why the extra coverage matters:
    // an area sized exactly to the row makes the positioner's own padding
    // (6px above/below the row inside its menu) hover DEAD ZONES — the
    // moment the pointer's truth position crosses the row's top/bottom edge
    // it reads outside → hoveredIndex −1 → highlight blinks off — then back
    // on one pixel lower. That is precisely the "first hover flickers"
    // report. With zoneRow set, points anywhere over the covering area are
    // mapped into the row's coordinate frame for the x zone computation
    // (y is deliberately ignored: zones are columnar — one x always maps to
    // exactly one chip), so the highlight stays continuously engaged from
    // menu edge to menu edge and only resets when the pointer truly leaves.
    property Item zoneRow: null

    hoverEnabled: true

    // container.children includes more than just the visible row items —
    // a Row built from a Repeater also carries the Repeater item itself as
    // one of its children (zero-sized, since it has no visual bounds of its
    // own). Filtering to non-zero-sized children is what keeps `count` and
    // index numbering matching the actual visible items 1:1 regardless of
    // where that extra entry falls in the list.
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

    // First child whose right edge is past x — so a gap between two
    // children (e.g. Row spacing) falls to the child right after it, same
    // as landing just short of that child's own left edge. No point maps
    // to zero or more than one index: continuous, seamless coverage.
    // x must already be in the ROW's coordinate frame (see _rowLocalX).
    function zoneAt(x) {
        for (let i = 0; i < count; ++i) {
            if (x < _cells[i].x + _cells[i].width)
                return i
        }
        return count > 0 ? count - 1 : -1
    }

    // Map an x from THIS AREA's frame into the row's frame: the row's
    // origin in area coords (mapFromItem(zoneRow,0,0)) is the offset to
    // SUBTRACT — a row-local x_r sits at area-local x_r + origin, so the
    // inverse drops it. Getting the sign backwards shifts every computed
    // zone right by twice the origin (the row is centered, origin ~14px →
    // the highlight landed one chip right of where the pointer actually
    // was — caught live by the self-test sweep: cursor on Shape, Timer
    // lit). Constant for unrotated ancestors, which is all this UI uses.
    function _rowLocalX(x) {
        if (!zoneRow || zoneRow === root)
            return x
        return x - root.mapFromItem(zoneRow, 0, 0).x
    }

    // The one derivation everything funnels into. p is this area's local
    // pointer position from the catcher's truth stream — null when the
    // pointer is unknown or genuinely elsewhere, which is exactly the "no
    // highlight" case.
    function _syncFromTruth() {
        const p = AppCursor.pointerPos(root)
        // Over the covering area at all → always a live zone (x mapped into
        // the row's frame; y ignored — see zoneRow). Only a pointer truly
        // off the area reads as "no highlight".
        const z = p ? root.zoneAt(root._rowLocalX(p.x)) : -1
        if (z >= 0) {
            // A live zone: any pending "went out" verdict is obsolete.
            outDebounce.stop()
            if (z !== root.hoveredIndex)
                root.hoveredIndex = z
        } else if (root.hoveredIndex >= 0) {
            // Truth says OUT while a highlight is showing. Do NOT clear
            // immediately: a single-frame phantom OUT (WM_MOUSELEAVE-class
            // misfires are a proven failure mode on this platform — see
            // KNOWN_ISSUES.md) would blink the highlight off, and at SLOW
            // hand speed the next position event comes late enough that
            // the blink is plainly visible (fast hands masked it, which is
            // why the synthetic sweeps always looked clean). Require OUT
            // to hold for a few event-loop turns before resetting; a real
            // exit still clears within 120ms — imperceptible — while a
            // phantom self-heals on the next derivation.
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

    // A press can arrive while hoveredIndex is still stale -1 when the
    // preceding hover delivery was skipped entirely (a single-jump cursor
    // move, a synthetic click, or the area appearing under a stationary
    // pointer — see the onVisibleChanged note below). Re-derive BEFORE the
    // consumer's onClicked runs, or its hoveredIndex<0 guard swallows the
    // click.
    onPressed: (mouse) => {
        if (root.hoveredIndex < 0)
            root._syncFromTruth()
    }

    onExited: {
        // Same debounce path as every other OUT reading — never an
        // immediate clear (that was the phantom-exit blink hole).
        root._syncFromTruth()
    }

    // This area is typically hidden until some popover/menu opens (see
    // EditScreen.qml's content-type picker) — containsMouse only updates on
    // a real mouse event, so if the pointer is already resting over a chip
    // the instant the menu opens, hoveredIndex stays stuck at -1 (no
    // highlight) until the mouse physically moves. Re-derive from truth the
    // moment this area becomes visible — same fix shape as every
    // AppCursor.pointerPos consumer in this codebase for the identical
    // "appeared under a stationary pointer" gap.
    onVisibleChanged: {
        outDebounce.stop()
        if (!visible) {
            root.hoveredIndex = -1
            return
        }
        root._syncFromTruth()
    }
}
