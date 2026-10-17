import QtQuick
import VGRPresenterUI

// Reusable action button. variant: "primary" | "secondary" | "danger" | "ghost"
Rectangle {
    id: root

    property string text: ""
    property string variant: "primary"
    // `enabled` is inherited from Item — no need to redeclare it.
    property int horizontalPadding: Theme.space5

    signal clicked()

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: 34
    radius: Theme.radiusMd

    // Hoisted out: chaining Theme.<color>.r/.g/.b inline miscompiles under
    // Qt's QML AOT compiler (see NavItem.qml for the same fix).
    readonly property color ghostHoverTint: Qt.rgba(Theme.textPrimary.r, Theme.textPrimary.g, Theme.textPrimary.b, 0.06)

    // The RESTING fill only — hover is the shared layer below, so a button
    // lights the same way a card or a row does instead of lightening its own
    // fill by a hand-picked factor.
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

    // Over a filled button the wash is the app's own ink (white) — a white
    // lift is what "lighter(accent, 1.1)" was approximating; a ghost button
    // keeps its faint textPrimary tint.
    HoverGlow {
        anchors.fill: parent
        radius: parent.radius
        hovered: mouseArea.containsMouse && root.enabled
        glowColor: root.variant === "ghost" ? root.ghostHoverTint
                 : root.variant === "secondary" ? Qt.rgba(1, 1, 1, 0.05)
                 : Qt.rgba(1, 1, 1, 0.10)
    }

    border.width: (root.variant === "secondary" || root.variant === "ghost") ? 1 : 0
    border.color: Theme.borderSubtle
    opacity: root.enabled ? 1.0 : 0.5

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
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
