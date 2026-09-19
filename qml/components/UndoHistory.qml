import QtQuick
import VGRPresenterUI

// Generic undo/redo history — the reusable surface adapter over the real
// engine's undo stack (bps::project::UndoRedoManager, via the EngineBridge
// service; see src/EngineBridge.h). Any surface in the app (EditScreen's
// canvas, a future slide-roster history, a properties dialog...)
// instantiates one UndoHistory and teaches it two things:
//
//   capture — function() -> snapshot. Reads the surface's CURRENT state and
//             returns it as PLAIN DATA (deep-copy anything that will keep
//             changing — live objects must be frozen into values, and in
//             particular must not leave QML value-type gadgets like `color`
//             in the snapshot: it gets captured inside a closure that
//             crosses into C++ — EngineBridge.pushCommand stores the
//             pending do/undo closures as QJSValue inside the engine's own
//             std::vector-backed stack, well outside QML's object lifetime
//             tracking — and a `color` gadget doesn't reliably survive that
//             round trip. Stringify colors etc. before returning them; see
//             SlideCanvasStore.snapshotItems for the pattern and the exact
//             failure this avoids).
//   apply   — function(snapshot). Puts a snapshot back.
//
// afterRestore (optional) runs after every undo()/redo() — clearing
// transient state that may reference objects apply() just replaced (e.g. a
// selection).
//
// NOTE: every UndoHistory instance shares the SAME underlying stack — the
// engine has one bps::project::UndoRedoManager for the whole process, the
// same way any real editor has one Ctrl+Z history, not an isolated one per
// panel. Multiple surfaces each instantiating their own UndoHistory is
// still the right pattern (each just teaches the shared stack how to
// capture/apply ITS OWN state) — they aren't independently undoable from
// each other.
//
// Usage — three ways to bracket an edit, depending on what signal the
// caller actually has:
//   push(label)                 — one-shot edits (nudge, delete, recolor):
//                                  commits automatically via Qt.callLater
//                                  once the current synchronous edit (and
//                                  any same-tick follow-up writes) finishes.
//   push(label, false) + commit() later
//                                — gestures spanning multiple real events (a
//                                  drag, a resize, a multi-keystroke typing
//                                  session): push at the gesture's start,
//                                  call commit() explicitly once it truly
//                                  ends.
//   push() with no arguments    — settle-debounced: commits after a short
//                                  quiet period (600ms) with no further
//                                  push() calls. For callers that only have
//                                  a "gesture started" signal with no
//                                  matching "ended" signal to hook (e.g. a
//                                  slider component that fires onDragStarted
//                                  but reports no dragFinished back here).
// A push() call while one is already pending is a no-op (beyond possibly
// extending the settle window) — that's what collapses a whole gesture
// into a single undo entry.
QtObject {
    id: history

    property var capture: null
    property var apply: null
    property var afterRestore: null

    readonly property bool canUndo: EngineBridge.canUndo
    readonly property bool canRedo: EngineBridge.canRedo

    // The "before" snapshot for the undo step currently being gathered —
    // null when no edit is in progress.
    property var _pendingBefore: null
    property string _pendingLabel: ""

    property Timer _settleTimer: Timer {
        interval: 600
        onTriggered: history.commit()
    }

    function push(label, deferred) {
        if (!history.capture)
            return
        if (history._pendingBefore !== null) {
            if (label === undefined)
                history._settleTimer.restart()
            return
        }
        history._pendingBefore = history.capture()
        history._pendingLabel = label !== undefined ? label : qsTr("Edit")
        if (label === undefined)
            history._settleTimer.restart()
        else if (deferred !== false)
            Qt.callLater(history.commit)
    }

    function commit() {
        if (history._pendingBefore === null || !history.capture || !history.apply)
            return
        const before = history._pendingBefore
        const after = history.capture()
        const label = history._pendingLabel
        history._pendingBefore = null
        EngineBridge.pushCommand(label,
            function () { history.apply(after) },
            function () { history.apply(before) })
        // No "message" key — see EventBus.h's convention: this stays silent
        // telemetry (NotificationCenter only turns a payload into a toast
        // when it carries one), so every nudge/drag doesn't spam a toast,
        // while still being a real, subscribable event for anything that
        // wants to react to undo activity project-wide (an activity log, a
        // status line, ...) without coupling to EngineBridge or any one
        // screen's UndoHistory instance directly.
        EventBus.publish("undo.pushed", { label: label })
    }

    function undo() {
        EngineBridge.undo()
        if (history.afterRestore)
            history.afterRestore()
        // redoLabel() is correct here, not undoLabel(): undo() just moved
        // this entry from the undo stack onto the redo stack, so it's now
        // "the thing redo() would redo" — exactly the entry that was undone.
        EventBus.publish("undo.undone", {
            label: EngineBridge.redoLabel(),
            canUndo: EngineBridge.canUndo, canRedo: EngineBridge.canRedo
        })
    }

    function redo() {
        EngineBridge.redo()
        if (history.afterRestore)
            history.afterRestore()
        // Symmetric with undo() above: redo() just moved the entry back
        // onto the undo stack, so undoLabel() names what was just redone.
        EventBus.publish("undo.redone", {
            label: EngineBridge.undoLabel(),
            canUndo: EngineBridge.canUndo, canRedo: EngineBridge.canRedo
        })
    }

    // Drops the WHOLE app-wide history (see the shared-stack note above) —
    // call when the underlying surface is replaced wholesale (a new
    // document, loading a show file), not after normal edits.
    function clear() {
        EngineBridge.clearHistory()
        history._pendingBefore = null
    }
}
