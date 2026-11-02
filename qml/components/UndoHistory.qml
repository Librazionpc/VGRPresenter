import QtQuick
import VGRPresenterUI

QtObject {
    id: history

    property var capture: null
    property var apply: null
    property var afterRestore: null

    signal committed()

    readonly property bool canUndo: EngineBridge.canUndo
    readonly property bool canRedo: EngineBridge.canRedo

    // The "before" snapshot for the undo step currently being gathered —
    // null when no edit is in progress.
    property var _pendingBefore: null
    property string _pendingLabel: ""

    property int _epoch: 0

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
        const label = history._pendingLabel
        history._pendingBefore = null
        const epoch = history._epoch
        Qt.callLater(function () {
            // Abandoned/cleared while queued (see _epoch) — drop it.
            if (epoch !== history._epoch || !history.capture)
                return
            const after = history.capture()
            EngineBridge.pushCommand(label,
                function () { history.apply(after) },
                function () { history.apply(before) })
            EventBus.publish("undo.pushed", { label: label })
            history.committed()
        })
    }

    function commitNow() {
        if (history._pendingBefore === null || !history.capture || !history.apply)
            return false
        const before = history._pendingBefore
        const label = history._pendingLabel
        history._pendingBefore = null
        history._pendingLabel = ""
        history._settleTimer.stop()
        // Invalidates a commit() already queued for this same bracket (see
        // _epoch), so it cannot push a second command for one edit.
        history._epoch++
        const after = history.capture()
        // Snapshots are plain data by contract (see `capture`'s note: colors
        // are stringified before they get here), so this compare is exact.
        if (JSON.stringify(before) === JSON.stringify(after))
            return false
        EngineBridge.pushCommand(label,
            function () { history.apply(after) },
            function () { history.apply(before) })
        EventBus.publish("undo.pushed", { label: label })
        history.committed()
        return true
    }

    function abandonPending() {
        history._epoch++
        history._pendingBefore = null
        history._pendingLabel = ""
        history._settleTimer.stop()
    }

    function undo() {
        history.abandonPending()
        EngineBridge.undo()
        if (history.afterRestore)
            history.afterRestore()
        EventBus.publish("undo.undone", {
            label: EngineBridge.redoLabel(),
            canUndo: EngineBridge.canUndo, canRedo: EngineBridge.canRedo
        })
        history.committed()
    }

    function redo() {
        history.abandonPending()
        EngineBridge.redo()
        if (history.afterRestore)
            history.afterRestore()
        // Symmetric with undo() above: redo() just moved the entry back
        // onto the undo stack, so undoLabel() names what was just redone.
        EventBus.publish("undo.redone", {
            label: EngineBridge.undoLabel(),
            canUndo: EngineBridge.canUndo, canRedo: EngineBridge.canRedo
        })
        history.committed()
    }

    function clear() {
        EngineBridge.clearHistory()
        history.abandonPending()
    }
}