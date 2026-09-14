import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property string searchPlaceholder: "Search settings"
    property alias searchText: searchInput.text

    signal closeRequested()

    implicitHeight: 56
    color: Theme.surface

    Rectangle {
        id: logo
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        width: 26
        height: 26
        radius: Theme.radiusMd
        color: Theme.accent
    }

    Rectangle {
        id: searchBox
        anchors.right: closeButton.left
        anchors.rightMargin: Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        width: 200
        height: 30
        radius: Theme.radiusMd
        color: Theme.inset

        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "⌕"
            color: Theme.textMuted
            font.pixelSize: Theme.textSm
        }

        TextInput {
            id: searchInput
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.right: parent.right
            anchors.rightMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            color: Theme.textPrimary
            clip: true

            Text {
                visible: !searchInput.text.length && !searchInput.activeFocus
                text: root.searchPlaceholder
                font: searchInput.font
                color: Theme.textMuted
            }
        }
    }

    Rectangle {
        id: closeButton
        anchors.right: parent.right
        anchors.rightMargin: Theme.space5
        anchors.verticalCenter: parent.verticalCenter
        width: 34
        height: 34
        radius: Theme.radiusMd
        color: closeArea.containsMouse ? Theme.chip : Theme.inset

        Text {
            anchors.centerIn: parent
            text: "✕"
            color: Theme.textSecondary
            font.pixelSize: Theme.textMd
        }

        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.closeRequested()
        }
    }
}
