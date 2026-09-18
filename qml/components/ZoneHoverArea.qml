import QtQuick

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
// Usage: size this to exactly cover the row of items (anchors.fill: theRow
// as a SIBLING of the row, not a child of it — Row/Column/Grid don't allow
// their own children to use anchors.fill/left/right/etc, so this can't live
// inside the positioner it's tracking). Point `container` at that same row,
// read `hoveredIndex` for which one to highlight, and use onClicked
// (already a MouseArea signal, since this type IS one) with hoveredIndex to
// know which one was clicked.
MouseArea {
    id: root

    property Item container: null
    property int hoveredIndex: -1

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
    function zoneAt(x) {
        for (let i = 0; i < count; ++i) {
            if (x < _cells[i].x + _cells[i].width)
                return i
        }
        return count > 0 ? count - 1 : -1
    }

    // Both onEntered and onPositionChanged, not just the latter: entering
    // the area and immediately stopping (no further move) wouldn't
    // otherwise update hoveredIndex off its stale -1 from the last exit.
    onEntered: root.hoveredIndex = root.zoneAt(mouseX)
    onPositionChanged: (mouse) => root.hoveredIndex = root.zoneAt(mouse.x)
    onExited: root.hoveredIndex = -1
}
