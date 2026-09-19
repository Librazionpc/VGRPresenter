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
// recomputed on every pointer move, on own show/enable transitions, on any
// ANCESTOR show/enable transition (shownChain is a binding, so it
// subscribes to every ancestor's visible/enabled — a closing dropdown
// clears its rows), and at construction — which also makes "freshly shown
// under a stationary pointer" work, the case containsMouse provably misses.
//
// It also owns the pointing-hand cursor via AppCursor push/pop
// (owner-scoped, self-healing): MouseArea.cursorShape suffers the same
// stuck-exit bug.
//
// Clicks deliver reliably, so a click-sink MouseArea is included; it never
// enables hover (its containsMouse would just latch uselessly).
//
// Arbitration note: like the MouseAreas it replaces, recompute reads the
// point independently — two overlapping areas would both report hovered.
// Every surface rewired onto this (menu labels, logo, tabs, gear, dropdown
// rows) is non-overlapping by construction, so nothing regresses.
Item {
    id: root

    // True while the pointer is over the area. Bind visuals to this exactly
    // like the old containsMouse.
    property bool hovered: false
    // Cursor pushed to AppCursor while hovered. Set showCursor: false on a
    // hover-only surface that must not claim the cursor.
    property bool showCursor: true
    property int cursorShape: Qt.PointingHandCursor

    signal entered()
    signal exited()
    signal clicked(var mouse)

    // A BINDING (not a function call) deliberately: its initial evaluation
    // walks the ancestor chain and subscribes to every visible/enabled it
    // reads, so hiding or disabling ANY ancestor (a dropdown panel closing,
    // a modal scrim appearing) re-evaluates this and clears the hover —
    // without needing the hover-exit events this build never delivers.
    readonly property bool shownChain: {
        let o = root
        while (o) {
            if (o.visible === false || o.enabled === false)
                return false
            o = o.parent
        }
        return true
    }

    function _recompute() {
        const inside = AppCursor.windowHovered && root.shownChain
                       && AppCursor.hovered(root)
        if (inside !== root.hovered) {
            root.hovered = inside
            if (inside)
                root.entered()
            else
                root.exited()
        }
    }

    onShownChainChanged: _recompute()
    onVisibleChanged: _recompute()
    onEnabledChanged: _recompute()

    onHoveredChanged: _syncCursor()
    onShowCursorChanged: _syncCursor()
    onCursorShapeChanged: _syncCursor()
    function _syncCursor() {
        if (root.hovered && root.showCursor)
            AppCursor.push(root.cursorShape, root)
        else
            AppCursor.pop(root)
    }

    Connections {
        target: AppCursor
        function onPointerMoved() { root._recompute() }
    }

    Component.onCompleted: _recompute()
    Component.onDestruction: AppCursor.pop(root)

    MouseArea {
        anchors.fill: parent
        hoverEnabled: false
        onClicked: (mouse) => root.clicked(mouse)
    }
}
