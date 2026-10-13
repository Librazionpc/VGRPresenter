pragma Singleton
import QtQuick

// The app's ONE hover-tooltip source: WHO wants a tip, and WHAT it says.
//
// Why a service instead of a tip drawn inside the control: the controls that
// need one are all nested and all clipped — the L/R pills live inside cards
// inside the Media pane, the gain knob inside the settings dialogs' channel
// rows — so a bubble drawn as their child is cut off at the card edge the
// moment it needs to say anything. The text is published here and rendered by
// ONE window-level layer (components/ToolTipLayer.qml, beside the drag layer in
// Main.qml), which also owns the only thing the controls cannot know: where the
// window edge is, so the bubble is pulled inside it instead of hanging off.
//
// Driven by POSITION truth, never by hover events: this build delivers
// hover-ENTER but never hover-EXIT (see PositionHoverArea's header), so a tip
// armed from a MouseArea's containsMouse would stay standing after the pointer
// left. Owners call show()/hide() from a PositionHoverArea's entered/exited,
// which recompute from the pointer stream in both directions.
//
// owner-scoped exactly like AppCursor's stack: hide() only clears the tip its
// own owner raised, so a leaving control can never cancel the tip the control
// under the pointer is raising in the same move.
QtObject {
    id: root

    // The line to show ("" = no tip). Consumers build it from the app's shared
    // vocabulary — see Db.channelStateText for a channel's state in words.
    property string text: ""
    // The item the bubble is placed against: the control itself, not its hover
    // area, so the bubble sits on the thing the pointer is actually over.
    property var owner: null
    // Emitted on any change, so the layer can re-read both above. A tip has to
    // be able to draw anywhere, so this is the only channel between the two.
    signal changed()

    function show(tipText, tipOwner) {
        if (!tipText || !tipOwner)
            return root.hide(tipOwner)
        if (text === String(tipText) && owner === tipOwner)
            return                      // same tip, same owner: nothing to redraw
        text = String(tipText)
        owner = tipOwner
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
