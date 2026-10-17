import QtQuick
import VGRPresenterUI

// One selectable option chip — the active/hover chip pattern used across the
// settings screens (Recording's platform chips, Smart Config's mode cells).
// Extracted so chip styling can't drift; controlled component: `selected` is
// owned by the consumer, clicking emits `picked`.
Rectangle {
    id: root

    property string label: ""
    property bool selected: false
    signal picked()

    implicitWidth: chipLabel.implicitWidth + 26
    implicitHeight: 30
    radius: Theme.radiusMd
    color: root.selected ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                         : Theme.inset
    border.width: 1
    border.color: root.selected ? Theme.accent : Theme.border
    // Selection is state and still fades; hover is the shared layer.
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    HoverGlow {
        anchors.fill: parent
        radius: parent.radius
        hovered: chipArea.containsMouse && !root.selected
        glowColor: Theme.chip
    }

    Text {
        id: chipLabel
        anchors.centerIn: parent
        text: root.label
        color: root.selected ? Theme.accentLight : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        font.weight: root.selected ? Font.DemiBold : Font.Medium
    }

    MouseArea {
        id: chipArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.picked()
    }
}
