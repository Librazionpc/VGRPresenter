import QtQuick
import VGRPresenterUI

// Topmost cursor layer for the whole window: renders AppCursor.shape while
// the override stack is non-empty; when the stack is empty it is cursor-
// NEUTRAL — every component's own hover/click behavior keeps working
// untouched.
//
// Why a window-root layer at all: the Edit canvas sits under a `scale:`
// transform (zoom), and per-MouseArea `cursorShape` silently stops applying
// under scaled ancestors — hover events still deliver (containsMouse fires,
// verified by probe) but the OS cursor never changes. The AV board's
// line-drag CrossCursor works only because that screen is unscaled. Forcing
// the shape from ONE layer outside any transform removes the arbitration
// problem entirely: while the stack is non-empty, the pushed shape wins
// app-wide; when it empties, this layer stops asserting any shape at all
// and normal per-item cursors resume.
//
// HoverHandler with blocking: false — NOT a hoverEnabled MouseArea — is
// what lets this coexist with everything below: a non-blocking handler
// participates in hover delivery without consuming it, so hover washes,
// chip highlights, and the canvas components' own containsMouse-driven
// push/pop all keep updating while an override is showing. MouseArea would
// have swallowed hover the moment it became visible, freezing every
// containsMouse below it (and stranding pushed cursors).
//
// ALWAYS ENABLED, cursor-neutral when idle — this exact combination is the
// conclusion of two earlier designs, each of which failed differently:
//
//  * Catcher-as-MouseArea: worked until an override was showing, then it
//    swallowed hover and froze every containsMouse below it.
//
//  * HoverHandler with `enabled: AppCursor.active`: don't do this. Every
//    push/pop flips the handler off and on MID-HOVER, and a handler that
//    gets enabled while the pointer is already stationary inside the window
//    doesn't observe an enter transition, so it doesn't begin asserting its
//    cursorShape until the next real mouse event. Visible symptom: the
//    I-beam intermittently not appearing when a text edit opens (or the
//    pointer re-enters the box) — exactly the "cursor doesn't show
//    throughout while editing" report. Keep the handler enabled from
//    startup so hover is permanently latched and every shape change is a
//    live cursorShape re-assert (this codebase's resize-ring handles prove
//    those apply while hovered — they morph I-beam↔hand↔size shapes under
//    a stationary pointer with no extra mouse move needed).
//
//  * (rejected) always-enabled with a CONSTANT shape: a handler with a set
//    cursorShape re-asserts from the window root, which would shadow plain
//    cursorShape MouseAreas app-wide (buttons losing their pointing hand).
//
// With `cursorShape: <shape> : undefined` neither failure mode exists:
// per Qt, a PointerHandler whose cursorShape is unset never modifies the
// cursor at all — the OS keeps whatever item below would set. The only
// shapes that ever come out of this layer are ones the AppCursor stack
// explicitly pushed. Null-shape transition warnings ("QML HoverHandler:
// cursorShape cannot be reset to an unknown or null shape") are benign
// noise — the shape stays the last pushed one during that tick; the stack
// itself is the source of truth and pops are validated separately.
Item {
    anchors.fill: parent

    HoverHandler {
        id: catcherHandler
        blocking: false
        // Feeds AppCursor the pointer position on every event so push sites
        // can ask AppCursor.hovered(item) — position truth even when the
        // pointer is stationary (containsMouse can't answer that for a
        // freshly shown/enabled area; see AppCursor._point's note).
        // scenePosition: window-scene coordinates, the canonical frame the
        // stack stores (hovered() maps items into it via mapFromItem(null)).
        onPointChanged: AppCursor.setPointerPos(point.scenePosition)
        cursorShape: AppCursor.active ? AppCursor.shape : undefined
    }
}
