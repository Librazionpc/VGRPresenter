import QtQuick
import VGRPresenterUI

// Shared press/threshold/drag body for canvas objects with no editable
// text (the camera preview, the generic kind placeholders) — one copy of
// the press/threshold/delta mechanics EditableCanvasLabel uses internally,
// instead of a hand-rolled duplicate per placeholder.
//
// Reports per-event deltas (not total-since-press) with the same contract
// as EditableCanvasLabel's moveRequested, so both feed the consumer's
// applyCanvasMove(key, dx, dy, ...) accumulation logic identically
// (snapDisabled is true while Alt is held, matching that component too).
// `tapped` fires only for gestures that never exceeded the drag threshold,
// so a drag never also toggles selection.
MouseArea {
    id: root

    signal moved(real dx, real dy, bool snapDisabled)
    signal dragFinished()
    // Not "clicked" — MouseArea already declares that signal and a
    // redeclaration would shadow it.
    signal tapped(var mouse)

    property real pressMouseX: 0
    property real pressMouseY: 0
    property bool dragMoved: false
    // A press is live (cleared on release) — see EditableCanvasLabel's
    // dragArea: the grab must end with the button, while dragMoved latches for
    // the rest of the gesture so a drag never also counts as a tap.
    property bool dragging: false

    hoverEnabled: true
    // Cursor via the AppCursor override stack (see EditableCanvasLabel's
    // dragArea for the full why — scale-transformed canvas kills plain
    // cursorShape). Identical shape as the fallback binding below.
    function syncCursor() {
        // AppCursor.hovered(root) — position truth from the window catcher —
        // covers the appearance-under-a-stationary-pointer cases where a
        // fresh area has no hover history yet, and is the ONLY hover signal
        // besides `pressed`: containsMouse latches in this build (hover-exit
        // never delivers — KNOWN_ISSUES.md), so gating on it left the hand
        // cursor stuck on after leaving the item.
        // `pressed` grabs immediately and outranks a hover claim: the drag cursor
        // no longer waits for the 4px threshold (see EditableCanvasLabel).
        if (visible && enabled && (pressed || AppCursor.hovered(root)))
            AppCursor.push((pressed && root.dragging) ? Qt.ClosedHandCursor : Qt.PointingHandCursor,
                           root, pressed ? AppCursor.priorityPressed : AppCursor.priorityHover)
        else
            AppCursor.pop(root)
    }
    cursorShape: root.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
    // Recompute from the pointer-position stream (every move) instead of
    // onContainsMouseChanged — same mechanism as PositionHoverArea.
    Connections {
        target: AppCursor
        function onPointerMoved() { root.syncCursor() }
    }
    onEnabledChanged: syncCursor()
    Component.onDestruction: AppCursor.pop(root)

    onPressed: (mouse) => {
        dragging = true
        dragMoved = false
        syncCursor()
        const g = mapToItem(null, mouse.x, mouse.y)
        pressMouseX = g.x
        pressMouseY = g.y
    }
    onPositionChanged: (mouse) => {
        if (!pressed)
            return
        const g = mapToItem(null, mouse.x, mouse.y)
        const dx = g.x - pressMouseX
        const dy = g.y - pressMouseY
        if (dragMoved || Math.abs(dx) > 4 || Math.abs(dy) > 4) {
            dragMoved = true
            root.syncCursor() // closed hand once the drag is live
            root.moved(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
            pressMouseX = g.x
            pressMouseY = g.y
        }
    }
    onReleased: {
        dragging = false   // before the re-claim: the grab ends with the button
        syncCursor()
        root.dragFinished()
    }
    // MouseArea has no onTap handler — the click signal is onClicked. A
    // gesture that dragged already reported moved(), so a click here only
    // counts when the press never became a drag.
    onClicked: (mouse) => {
        if (dragMoved)
            return
        root.tapped(mouse)
    }
}
