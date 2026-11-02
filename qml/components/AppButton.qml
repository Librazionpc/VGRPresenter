import QtQuick
import VGRPresenterUI

// Reusable action button. variant: "primary" | "secondary" | "danger" | "ghost"
Rectangle {
    id: root

    property string text: ""
    property string variant: "primary"
    // `enabled` is inherited from Item — no need to redeclare it.
    property int horizontalPadding: Theme.space5
    // The caption size; a glyph-only button ("+") sets it larger.
    property int fontSize: Theme.textSm

    signal clicked()

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: 34
    radius: Theme.radiusMd

    color: {
        if (!root.enabled) return Theme.inset
        if (root.variant === "primary")
            return mouseArea.pressed ? Qt.darker(Theme.accent, 1.15) : Theme.accent
        if (root.variant === "danger")
            return mouseArea.pressed ? Qt.darker(Theme.danger, 1.15) : Theme.danger
        if (root.variant === "secondary")
            return Theme.inset
        return "transparent"
    }
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    HoverGlow {
        anchors.fill: parent
        radius: parent.radius
        hovered: mouseArea.containsMouse && root.enabled
        glowColor: Theme.hoverBg
    }

    border.width: (root.variant === "secondary" || root.variant === "ghost") ? 1 : 0
    border.color: Theme.borderSubtle
    opacity: root.enabled ? 1.0 : 0.5

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        font.family: Theme.fontFamily
        font.pixelSize: root.fontSize
        font.weight: Font.Medium
        color: (root.variant === "primary" || root.variant === "danger") ? Theme.controlInk
             : (root.variant === "ghost") ? Theme.textSecondary
             : Theme.textPrimary
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}