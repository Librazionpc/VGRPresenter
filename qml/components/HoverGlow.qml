import QtQuick

Rectangle {
    id: root

    // The surface's own hover state — every host already has one (a
    // HoverHandler, a PositionHoverArea, a MouseArea's containsMouse).
    property bool hovered: false
    property color glowColor: Theme.hoverBg
    // Draw a hover EDGE as well as the wash (cards and pills want it; menu rows
    // do not).
    property bool edge: false
    property color edgeColor: Qt.alpha(Theme.accent, Theme.hoverEdgeAlpha)
    color: root.glowColor
    border.width: root.edge ? 1 : 0
    border.color: root.edgeColor
    opacity: root.hovered ? 1 : 0
    enabled: false

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.hoverFade
            easing.type: Theme.hoverEasing
        }
    }
}