import QtQuick
import VGRPresenterUI

// Hover state from POINTER POSITION, not hover events — the position-truth
// replacement for a hoverEnabled MouseArea's containsMouse/entered/exited.
//
// WHY THIS EXISTS: this build delivers hover-ENTER to MouseAreas but never
// hover-EXIT (probe-proven with an in-process QWindowSystemInterface input
// driver; all five menu labels latched containsMouse=true across 1000
// pointer events while the window-root catcher's HoverHandler tracked every
// position). containsMouse latches true, so every hover wash sticks
// forever — the "clicked the menu and the hover refused to go away" report.
// The AppCursorCatcher's HoverHandler, in contrast, tracks pointer position
// flawlessly on every event, and this codebase already treats that stream
// as position truth (AppCursor.hovered/pointerPos — the same mechanism the
// Edit canvas uses for stuck-cursor-proof hover decisions). So this
// component derives hover from that stream:
//
//   hovered = window hovered && chain fully visible+enabled && point inside
//
// recomputed on every pointer move, on own show/enable transitions, on a
// popup opening/closing (AppCursor.blockersChanged — how a closing dropdown
// clears its rows), on a slow re-validate beat while a hover is live (so an
// ancestor hidden under a still pointer still clears it), and at construction
// — which also makes "freshly shown under a stationary pointer" work, the case
// containsMouse provably misses. The ancestor test is a FUNCTION (chainShown),
// deliberately not a binding: as a binding it was reported as a loop at boot
// (see chainShown's own comment).
//
// It also owns the pointing-hand cursor via AppCursor push/pop
// (owner-scoped, self-healing): MouseArea.cursorShape suffers the same
// stuck-exit bug.
//
// Clicks deliver reliably, so a click-sink MouseArea is included; it never
// enables hover (its containsMouse would just latch uselessly).
//
// Arbitration note: like the MouseAreas it replaces, recompute reads the
// point independently — two SIBLING areas would both report hovered. Every
// surface rewired onto this (menu labels, logo, tabs, gear, dropdown rows)
// is non-overlapping by construction, so nothing regresses. Areas that are
// merely STACKED (a card with an open menu over it) are settled by
// AppCursor.blocked() below: an open popup claims the pointer, so the surface
// under it stops reporting hovered and the menu reads as its own popup.
Item {
    id: root

    // True while the pointer is over the area. Bind visuals to this exactly
    // like the old containsMouse.
    property bool hovered: false
    // Walk the ancestor chain for hidden/disabled ancestors (see chainShown).
    // Leave true on top-level surfaces (dropdown rows, tabs). Turn FALSE for a
    // hover area nested inside a parent whose visibility IS a hover state (a
    // card's action row shown while the card is hovered): there the walk would
    // read the very hover that hides the row, feeding itself.
    property bool checkAncestors: true
    // Cursor pushed to AppCursor while hovered. Set showCursor: false on a
    // hover-only surface that must not claim the cursor.
    property bool showCursor: true
    property int cursorShape: Qt.PointingHandCursor
    // The claim this area's cursor asserts in AppCursor's priority order.
    // Default: an ordinary hover claim (priorityHover), which EQUAL-priority
    // areas settle by push order — and because every area re-pushes on each
    // pointer move, two equal claims over the same point alternate and the
    // cursor visibly FLASHES between the two shapes. A frame grip / handle
    // that overlaps ordinary content must say priorityHandle (or
    // priorityPressed) so its shape wins outright instead of alternating.
    property int cursorPriority: AppCursor.priorityHover
    // Hover-only overlays should let the actual control below them receive clicks.
    property bool clickEnabled: true

    signal entered()
    signal exited()
    signal clicked(var mouse)
    // A quick second press+release (MouseArea's own detection) — the
    // deliberate-activation gesture for surfaces where a stray single
    // click must do nothing (the Media pane's input cards take/clear on
    // DOUBLE-click; single clicks are ignored there).
    signal doubleClicked(var mouse)

    // A FUNCTION, not a binding — and that is a fix, not a simplification.
    //
    // It used to be `readonly property bool shownChain: { ...walk... }`, chosen
    // so the binding would SUBSCRIBE to every ancestor's visible/enabled and
    // clear the hover the moment a dropdown closed or a pane switched tabs.
    // That walk is exactly what QML reported as
    //
    //   QML PositionHoverArea: Binding loop detected for property "shownChain"
    //
    // at boot (five times, with HoverDetail as the parent: its content is a
    // control whose label is sized FROM that state, so evaluating the walk
    // re-entered it). The answer is still needed — a hover area inside a hidden
    // or disabled ancestor must not report hovered — so it is computed on demand
    // here, and what replaces the lost subscription is `_recompute` being called
    // on pointer movement, on blocker changes, on this area's own show/enable
    // transitions, and by the re-validate timer below.
    function chainShown() {
        if (!root.visible || root.enabled === false)
            return false
        if (!checkAncestors)
            return true
        let o = root.parent
        while (o) {
            if (o.visible === false || o.enabled === false)
                return false
            o = o.parent
        }
        return true
    }

    function _recompute() {
        const inside = AppCursor.windowHovered && root.chainShown()
                       && AppCursor.hovered(root) && !AppCursor.blocked(root)
        if (inside !== root.hovered) {
            root.hovered = inside
            if (inside)
                root.entered()
            else
                root.exited()
        }
    }

    onVisibleChanged: _recompute()
    onEnabledChanged: _recompute()

    // SELF-HEALING RE-VALIDATE (see chainShown): this build delivers no
    // hover-exit event and there is no longer a binding to invalidate, so an
    // ancestor hiding under a perfectly still pointer would otherwise leave
    // this area reporting hovered. One walk up the parents, on a slow beat, and
    // ONLY while a hover is live — it stops as soon as the hover clears.
    Timer {
        interval: 250
        repeat: true
        running: root.hovered
        onTriggered: root._recompute()
    }

    onHoveredChanged: _syncCursor()
    onShowCursorChanged: _syncCursor()
    onCursorShapeChanged: _syncCursor()
    function _syncCursor() {
        if (root.hovered && root.showCursor)
            AppCursor.push(root.cursorShape, root, root.cursorPriority)
        else
            AppCursor.pop(root)
    }

    Connections {
        target: AppCursor
        function onPointerMoved() { root._recompute() }
        // A popup opening/closing over this area must settle it immediately —
        // the pointer may be perfectly still over the menu.
        function onBlockersChanged() { root._recompute() }
    }

    Component.onCompleted: _recompute()
    Component.onDestruction: AppCursor.pop(root)

    MouseArea {
        anchors.fill: parent
        enabled: root.clickEnabled
        hoverEnabled: false
        onClicked: (mouse) => root.clicked(mouse)
        onDoubleClicked: (mouse) => root.doubleClicked(mouse)
    }
}
