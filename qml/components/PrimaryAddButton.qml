import QtQuick
import VGRPresenterUI

Item {
    id: root

    property string text: ""
    signal clicked()

    implicitWidth: contentRow.width + Theme.space6 * 2
    implicitHeight: 40

    Rectangle {
        id: face
        anchors.fill: parent
        radius: Theme.radiusMd
        color: area.pressed ? Qt.darker(Theme.accent, 1.15)
             : area.containsMouse ? Qt.lighter(Theme.accent, 1.1)
             : Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: 8

            // The plus, drawn: two bars centred by anchors cannot sit off-centre
            // the way a stroked path can.
            Item {
                width: 11; height: 11
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.centerIn: parent
                    width: 11; height: 1.6; radius: 0.8
                    color: Theme.controlInk
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 1.6; height: 11; radius: 0.8
                    color: Theme.controlInk
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: Theme.controlInk
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}