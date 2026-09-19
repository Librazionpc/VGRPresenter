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
// app-wide; when it empties, this layer releases the cursor entirely and
// normal per-item cursors resume.
//
// HoverHandler with blocking: false — NOT a hoverEnabled MouseArea — is
// what lets this coexist with everything below: a non-blocking handler
// participates in hover delivery without consuming it, so hover washes,
// chip highlights, and the canvas components' own containsMouse-driven
// push/pop all keep updating while an override is showing. MouseArea would
// have swallowed hover the moment it became visible, freezing every
// containsMouse below it (and stranding pushed cursors).
//
// ALWAYS ENABLED, shape set IMPERATIVELY — the conclusion of three failed
// designs, each proven with a standalone probe or the AOT codegen dump:
//
//  * Catcher-as-MouseArea: worked until an override was showing, then it
//    swallowed hover and froze every containsMouse below it.
//
//  * HoverHandler with `enabled: AppCursor.active`: every push/pop flips
//    the handler off and on MID-HOVER, and a handler enabled under a
//    stationary pointer doesn't observe an enter transition, so it doesn't
//    assert its shape until the next real mouse event — the I-beam
//    intermittently not appearing at edit start.
//
//  * HoverHandler always enabled with `cursorShape: active ? shape :
//    undefined` BINDING: the qmlcachegen AOT dump of the compiled binding
//    shows the undefined branch is CONVERTED TO AN INT (0) and assigned
//    through setCursorShape() — which per Qt CLAIMS the cursor
//    (isCursorShapeExplicitlySet() = true). Net effect: the catcher forced
//    ArrowCursor app-wide whenever the stack was empty, and every plain
//    cursorShape MouseArea below it (menu rows, buttons — their pointing
//    hands) never applied. An interpreted qmltestrunner probe cannot see
//    this: there the engine routes binding-undefined to the property's
//    RESET; the AOT path does not.
//
// The working form assigns undefined from IMPERATIVE JS: that routes
// through QQmlPropertyPrivate::write, which honors a resettable property's
// RESET method (resetCursorShape() → cursorSet = false → the handler stops
// claiming the cursor and items below take over). qtdeclarative's own docs
// (qquickpointerhandler.cpp): "This property can be reset to the same
// initial condition by setting it to undefined." Assignments of a real
// shape claim it for exactly as long as the stack is non-empty. The
// handler itself is never disabled, so hover stays latched from startup
// and every transition applies immediately — including under a stationary
// pointer.
Item {
    anchors.fill: parent

    HoverHandler {
        id: catcherHandler
        blocking: false
        // Window-level hover transitions DO deliver reliably (unlike
        // MouseArea containsMouse — see KNOWN_ISSUES.md). Mirror them into
        // AppCursor: windowHovered gates every position-driven hover wash,
        // and a cleared point can never read as "still over something".
        onHoveredChanged: {
            AppCursor.windowHovered = hovered
            if (!hovered)
                AppCursor.clearPointerPos()
        }
        // Feeds AppCursor the pointer position on every event so push sites
        // can ask AppCursor.hovered(item) — position truth even when the
        // pointer is stationary (containsMouse can't answer that for a
        // freshly shown/enabled area; see AppCursor._point's note).
        // scenePosition: window-scene coordinates, the canonical frame the
        // stack stores (hovered() maps items into it via mapFromItem(null)).
        onPointChanged: AppCursor.setPointerPos(point.scenePosition)
    }

    function syncShape() {
        // Assign undefined (the documented RESET) when the stack is empty —
        // NEVER a fallback int, or this layer would claim the arrow and
        // shadow every pointing-hand below it.
        catcherHandler.cursorShape = AppCursor.active ? AppCursor.shape
                                                      : undefined
    }
    Component.onCompleted: syncShape()
    Connections {
        target: AppCursor
        function onActiveChanged() { syncShape() }
        function onShapeChanged() {
            if (AppCursor.active)
                syncShape()
        }
    }
}
