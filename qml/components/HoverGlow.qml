import QtQuick

// THE hover layer — the one shape every hoverable surface in the app lights up
// through, so hover reads the same on a card, a row, a pill and a button.
//
// Usage: a surface sits on its resting colour (which NEVER animates) and drops
// this in as a child, filling it, above the background and below the content:
//
//   Rectangle {
//       color: Theme.rowBg
//       HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: rowHover.hovered }
//   }
//
// Why a layer and not a colour binding: two animated colours (a fill and a
// border) racing each other strobe when the pointer sweeps a grid, and
// interpolating an alpha-blended colour bands. One rectangle whose OPACITY
// fades is smooth by construction, and it is the same motion on every surface —
// see Theme.hoverFade for the timings and the reasoning.
//
// `enabled: false` is deliberate and load-bearing: this is paint, not a target,
// so it must never swallow the pointer from the buttons, drag sources and
// hover areas underneath it. Declare it BEFORE the children that need clicks;
// an overlay that lands on top of them is a hover that eats clicks.
Rectangle {
    id: root

    // The surface's own hover state — every host already has one (a
    // HoverHandler, a PositionHoverArea, a MouseArea's containsMouse).
    property bool hovered: false
    // The wash. Defaults to the theme accent at the shared hover alpha; a
    // neutral surface (a menu row, a list row) passes a wash of its own.
    property color glowColor: Qt.alpha(Theme.accent, Theme.hoverGlowAlpha)
    // Draw a hover EDGE as well as the wash (cards and pills want it; menu rows
    // do not).
    property bool edge: false
    property color edgeColor: Qt.alpha(Theme.accent, Theme.hoverEdgeAlpha)
    // Micro-controls fade quicker; everything else uses the shared pace.
    property bool quick: false

    color: root.glowColor
    border.width: root.edge ? 1 : 0
    border.color: root.edgeColor
    opacity: root.hovered ? 1 : 0
    enabled: false

    Behavior on opacity {
        NumberAnimation {
            duration: root.quick ? Theme.hoverFadeFast : Theme.hoverFade
            easing.type: Theme.hoverEasing
        }
    }
}
