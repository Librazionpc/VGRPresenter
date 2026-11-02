import QtQuick
import VGRPresenterUI

Rectangle {
    id: field

    property string label: ""
    property string value: ""
    // Hints that tapping opens something (a dropdown, a picker) — purely
    // visual; the consumer decides what tapped() actually does.
    property bool chevron: false
    property bool modified: false
    signal tapped()
    signal resetRequested()

    height: 50
    radius: 10
    color: fieldHit.hovered ? Theme.hoverBg : Theme.inset
    border.color: fieldHit.hovered ? Theme.border : Theme.borderSubtle
    border.width: 1
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    Column {
        x: 12
        width: field.width - (field.chevron ? 32 : 24) - (field.modified ? 26 : 0)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            text: field.label
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Medium
            font.letterSpacing: 0.2
            elide: Text.ElideRight
            width: parent.width
        }
        Text {
            text: field.value
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: 16
            elide: Text.ElideRight
            width: parent.width
        }
    }

    IconGlyph {
        anchors.right: parent.right
        anchors.rightMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        visible: field.chevron
        name: "chevronDown"
        color: Theme.textMuted
        width: 12; height: 12
    }

    PositionHoverArea {
        id: fieldHit
        anchors.fill: parent
        checkAncestors: false
        onClicked: field.tapped()
    }

    // The reset arrow, above the box's own hit area so a click on it resets and does
    // not open the field.
    Item {
        anchors.right: parent.right
        anchors.rightMargin: field.chevron ? 28 : 8
        anchors.verticalCenter: parent.verticalCenter
        width: 22; height: 22
        visible: field.modified

        Text {
            anchors.centerIn: parent
            text: "↻"
            color: resetArea.containsMouse ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: 15
        }
        MouseArea {
            id: resetArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: field.resetRequested()
        }
    }
}