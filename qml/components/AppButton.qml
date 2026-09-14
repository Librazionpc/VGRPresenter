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

    color: {
        if (!root.enabled) return Theme.inset
        if (root.variant === "primary")
            return mouseArea.pressed ? Qt.darker(Theme.accent, 1.15)
                 : mouseArea.containsMouse ? Qt.lighter(Theme.accent, 1.1)
                 : Theme.accent
        if (root.variant === "danger")
            return mouseArea.pressed ? Qt.darker(Theme.danger, 1.15)
                 : mouseArea.containsMouse ? Qt.lighter(Theme.danger, 1.08)
                 : Theme.danger
        if (root.variant === "secondary")
            return mouseArea.containsMouse ? Qt.lighter(Theme.inset, 1.3) : Theme.inset
        return mouseArea.containsMouse ? root.ghostHoverTint : "transparent"
    }
    Behavior on color { ColorAnimation { duration: 120 } }

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
        color: (root.variant === "primary" || root.variant === "danger") ? "#ffffff"
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
