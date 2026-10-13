pragma Singleton
import QtQuick

// The app runs at ONE scale — Main.qml's `uiScale`: every surface is drawn at
// its 1440x900 design size and scaled uniformly to the window (never up, so a
// window with the room keeps the design size exactly).
//
// This singleton is that number for the components that CANNOT inherit it from
// the layer they are drawn in: the modal cards, which are reparented to the
// window's overlay or declared inside a pane that itself sits in a scaled
// screen. It keeps the two rules a dialog must never get wrong in ONE place:
//
//   1. THE SCRIM IS NEVER SCALED. The dim backdrop is the window-sized
//      rectangle whose whole job is to darken everything behind the dialog; a
//      scale on the modal ROOT shrinks it away from the window's own edges, and
//      the dialog loses the black box that makes it read as modal. Only the
//      CARD is scaled.
//   2. THE CARD TAKES appScale MINUS ANY ANCESTOR SCALE, so a dialog opened
//      inside an already-scaled screen (or inside the scaled Settings card) is
//      not scaled twice.
//
// Main.qml registers the window once, at boot; every value here is a live
// binding on it, so resizing the window re-scales an open dialog in place.
QtObject {
    id: root

    // The window whose uiScale is the app scale (set once, by Main.qml).
    property var win: null
    function register(w) { root.win = w }

    // Main.qml's uiScale — or 1 before the window is registered (and in a
    // standalone probe that never registered one).
    readonly property real appScale: {
        const w = root.win
        return (w && w.uiScale !== undefined && w.uiScale > 0) ? w.uiScale : 1
    }

    // How much scale `item` already inherits from its ancestors — measured from
    // its parent's mapped corners (mapToItem folds in every transform above it)
    // rather than read from a property, so any ancestor's scale counts whatever
    // its origin. Falls back to 1 while the parent has no size yet.
    function inheritedScale(item) {
        const p = item ? item.parent : null
        if (!p || p.width === undefined || p.width <= 0)
            return 1
        const a = p.mapToItem(null, 0, 0)
        const b = p.mapToItem(null, p.width, 0)
        const s = Math.abs(b.x - a.x) / p.width
        return s > 0 ? s : 1
    }

    // The scale a modal CARD must carry so it ends up drawn at `appScale` on
    // screen, however much its own modal root already inherits. Never cached:
    // it re-evaluates with the window's size, so an open dialog follows a
    // resize.
    function cardScale(card) {
        return root.appScale / root.inheritedScale(card)
    }
}
