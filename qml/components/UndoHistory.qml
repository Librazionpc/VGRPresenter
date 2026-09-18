import QtQuick

// Generic snapshot-based undo/redo history — the mechanism extracted out of
// EditScreen.qml so any surface in the app can own an undo stack instead of
// each growing a bespoke one. A "surface" is whatever unit of state should
// undo as a whole: EditScreen's canvas ({ items, background }), a future
// slide-roster history, a properties dialog, etc. Each surface instantiates
// one UndoHistory and teaches it two things:
//
//   capture — function() -> snapshot. Reads the surface's CURRENT state and
//             returns it as a plain value. Deep-copy anything that will keep
//             changing after the push: live objects must be frozen into
//             plain data (exactly why SlideCanvasStore.snapshotItems copies
//             field values out instead of remembering object references).
//   apply   — function(snapshot). Puts a snapshot back. Called by undo()/
//             redo() AFTER the outgoing state has been captured, so it can
//             freely destroy/replace live objects.
//
// afterRestore (optional) runs after every restore — clearing transient
// state that may reference objects apply() just replaced (e.g. a selection).
//
// Usage contract: consumers push() BEFORE mutating (the snapshot must be
// the pre-change state) and bracket whole gestures themselves — one push
// per drag, per slider gesture, per typing session, never per intermediate
// value. That policy stays with the consumer because only it knows where
// its gestures begin and end; this component deliberately knows nothing
// about gestures.
QtObject {
    id: history

    property var capture: null
    property var apply: null
    property var afterRestore: null

    // History cap — oldest entries fall off once exceeded.
    property int maxDepth: 50

    property var undoStack: []
    property var redoStack: []

    // Toolbar buttons / menu items / Shortcut.enabled bind enablement to
    // these instead of poking at the stacks directly.
    readonly property bool canUndo: undoStack.length > 0
    readonly property bool canRedo: redoStack.length > 0

    // Remembers the current state as the next undo step and invalidates the
    // redo branch — standard undo semantics: a new edit after an undo ends
    // that redo timeline.
    function push() {
        if (!capture)
            return
        undoStack = undoStack.concat([capture()])
        if (undoStack.length > maxDepth)
            undoStack = undoStack.slice(undoStack.length - maxDepth)
        redoStack = []
    }

    function undo() {
        if (undoStack.length === 0 || !apply)
            return
        const snap = undoStack[undoStack.length - 1]
        undoStack = undoStack.slice(0, -1)
        redoStack = redoStack.concat([capture()])
        apply(snap)
        if (afterRestore)
            afterRestore()
    }

    function redo() {
        if (redoStack.length === 0 || !apply)
            return
        const snap = redoStack[redoStack.length - 1]
        redoStack = redoStack.slice(0, -1)
        undoStack = undoStack.concat([capture()])
        apply(snap)
        if (afterRestore)
            afterRestore()
    }

    // Drops all history — call when the underlying surface is replaced
    // wholesale (a new document, loading a show file), not after normal
    // edits.
    function clear() {
        undoStack = []
        redoStack = []
    }
}
