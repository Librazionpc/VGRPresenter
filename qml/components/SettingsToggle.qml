import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property bool checked: false
    signal toggled()

    implicitWidth: 36
    implicitHeight: 20
    radius: height / 2
    color: checked ? Theme.accent : Theme.toggleOffTrack
    border.width: 1
    border.color: checked ? Theme.accent : Theme.borderSubtle
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    Rectangle {
        id: knob
        width: 14
        height: 14
        radius: 7
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.controlInk
        x: root.checked ? root.width - width - 3 : 3
        Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()   // consumer flips `checked` — see header
    }
}