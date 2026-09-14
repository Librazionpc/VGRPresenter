import QtQuick
import VGRPresenterUI

// One row in the settings nav rail. Selected state = tinted pill + left accent
// bar + tinted icon chip + accent label/icon, matching the pattern used across
// every Settings screen.
Item {
    id: root

    property string label: ""
    property string iconKind: "sliders"
    property bool selected: false

    signal clicked()

    implicitWidth: 176
    implicitHeight: 42

    // Hoisted out of the color bindings below: chaining Theme.<color>.r/.g/.b
    // directly inside a ternary miscompiles under Qt's QML AOT compiler.
    readonly property color accentTint10: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0x1a / 255)
    readonly property color accentTint16: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0x29 / 255)
    readonly property color textPrimaryTint04: Qt.rgba(Theme.textPrimary.r, Theme.textPrimary.g, Theme.textPrimary.b, 0.04)

    Rectangle {
        id: pill
        anchors.fill: parent
        radius: Theme.radiusMd
        color: root.selected ? root.accentTint10
             : mouseArea.containsMouse ? root.textPrimaryTint04
             : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Rectangle {
        id: accentBar
        width: 3
        height: 20
        radius: 1.5
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        color: Theme.accent
        opacity: root.selected ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    Row {
        anchors.verticalCenter: parent.verticalCenter
        x: 14
        spacing: Theme.space3

        Rectangle {
            width: 28
            height: 28
            radius: Theme.radiusSm
            anchors.verticalCenter: parent.verticalCenter
            color: root.selected ? root.accentTint16 : Theme.navChipBg

            NavIcon {
                anchors.centerIn: parent
                kind: root.iconKind
                color: root.selected ? Theme.accentLight : Theme.iconMuted
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textMd
            color: root.selected ? Theme.accentLight : Theme.navLabelMuted
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
