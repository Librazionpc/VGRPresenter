import QtQuick
import VGRPresenterUI

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
    property bool dragging: false

    hoverEnabled: true
    function syncCursor() {
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
    onClicked: (mouse) => {
        if (dragMoved)
            return
        root.tapped(mouse)
    }
}