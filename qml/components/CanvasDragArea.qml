import QtQuick

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

    hoverEnabled: true
    cursorShape: root.dragMoved ? Qt.ClosedHandCursor : Qt.PointingHandCursor

    onPressed: (mouse) => {
        const g = mapToItem(null, mouse.x, mouse.y)
        pressMouseX = g.x
        pressMouseY = g.y
        dragMoved = false
    }
    onPositionChanged: (mouse) => {
        if (!pressed)
            return
        const g = mapToItem(null, mouse.x, mouse.y)
        const dx = g.x - pressMouseX
        const dy = g.y - pressMouseY
        if (dragMoved || Math.abs(dx) > 4 || Math.abs(dy) > 4) {
            dragMoved = true
            root.moved(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
            pressMouseX = g.x
            pressMouseY = g.y
        }
    }
    onReleased: root.dragFinished()
    // MouseArea has no onTap handler — the click signal is onClicked. A
    // gesture that dragged already reported moved(), so a click here only
    // counts when the press never became a drag.
    onClicked: (mouse) => {
        if (dragMoved)
            return
        root.tapped(mouse)
    }
}
