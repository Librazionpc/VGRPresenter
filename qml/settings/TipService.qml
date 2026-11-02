pragma Singleton
import QtQuick

QtObject {
    id: root

    // The line to show ("" = no tip). Consumers build it from the app's shared
    // vocabulary — see Db.channelStateText for a channel's state in words.
    property string text: ""
    // The item the bubble is placed against: the control itself, not its hover
    // area, so the bubble sits on the thing the pointer is actually over.
    property var owner: null
    property bool forceDelay: false
    // Emitted on any change, so the layer can re-read both above. A tip has to
    // be able to draw anywhere, so this is the only channel between the two.
    signal changed()

    function show(tipText, tipOwner, alwaysDelay) {
        if (!tipText || !tipOwner)
            return root.hide(tipOwner)
        if (text === String(tipText) && owner === tipOwner)
            return                      // same tip, same owner: nothing to redraw
        text = String(tipText)
        owner = tipOwner
        forceDelay = alwaysDelay === true
        changed()
    }

    // The owner's tip is over: the pointer left it, or it went away.
    function hide(tipOwner) {
        if (tipOwner && owner !== tipOwner)
            return                      // not ours to clear (see the header)
        text = ""
        owner = null
        changed()
    }

    // An owner about to be destroyed cannot call hide() reliably (a delegate
    // recycled mid-hover unloads while its tip is up).
    function forget(tipOwner) {
        if (owner === tipOwner)
            hide(tipOwner)
    }
}