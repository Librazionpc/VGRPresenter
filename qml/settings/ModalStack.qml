pragma Singleton
import QtQuick

// One shared stacking counter AND blur-source resolver for every ModalCard
// (and the two hand-rolled dialogs, NewShowDialog/ImportDialog, that predate
// it but share its scrim). Two jobs:
//
// 1. Z-ORDER. A flat `z: 30000` on every dialog meant two SIMULTANEOUSLY
//    open ModalCards (e.g. "Manage groups" left open, then "Manage actions"
//    opened on top of it from the same settings screen) stacked by
//    DECLARATION ORDER in their .qml file, never by which one the operator
//    actually opened last. ModalCard.open() calls nextZ() the moment it
//    opens, so the most-recently-opened dialog is always the topmost one —
//    the same rule any window manager applies when a window is brought to
//    front.
//
// 2. BLUR SOURCE. Every dialog's scrim should blur whatever is DIRECTLY
//    behind IT specifically — which depends on where it was opened from and
//    what else is open, not something a dialog declared three files deep
//    (ManageActionsModal, nested inside AppearanceActionsScreen, nested
//    inside the Settings Loader) can work out for itself. `stack` is the
//    list of currently-open dialog CARDS, bottom to top; a dialog asks
//    blurSourceFor(itsOwnCard) and gets back the card directly below it, or
//    the PAGE LAYER (Settings' own shell while Settings is open, else
//    whichever of Show/Edit is on screen) when there is nothing below it.
//    One rule, so a dialog opened over Settings blurs Settings, a dialog
//    opened over THAT dialog blurs it in turn, and a dialog opened with
//    nothing else up blurs the real page — without any call site having to
//    say which case it is.
QtObject {
    id: root
    readonly property int base: 30000
    property int top: base
    function nextZ() {
        root.top += 1
        return root.top
    }

    // Settings' own card (Main.qml's settingsShell) — published once at
    // startup (Component.onCompleted there). null until Main.qml finishes
    // constructing it, and irrelevant whenever Settings itself isn't open
    // (see blurSourceFor's own visible check).
    property Item settingsShell: null
    // A function, not a cached Item: Show and Edit are mutually exclusive
    // and swap which one is on screen, so this is re-asked every time
    // rather than going stale the moment the operator switches screens.
    // Published once by Main.qml (ModalStack.pageLayer = function() {...}).
    property var pageLayer: null

    // The open-dialog stack, bottom to top — each entry is a dialog's own
    // CARD item (not its root), pushed by open() and popped by close()/
    // destruction.
    property var stack: []
    function push(cardItem) {
        if (!cardItem || root.stack.indexOf(cardItem) >= 0)
            return
        root.stack = root.stack.concat([cardItem])
    }
    function pop(cardItem) {
        const i = root.stack.indexOf(cardItem)
        if (i < 0)
            return
        const next = root.stack.slice()
        next.splice(i, 1)
        root.stack = next
    }
    function blurSourceFor(cardItem) {
        const i = root.stack.indexOf(cardItem)
        if (i > 0)
            return root.stack[i - 1]
        if (root.settingsShell && root.settingsShell.visible)
            return root.settingsShell
        return root.pageLayer ? root.pageLayer() : null
    }
}
