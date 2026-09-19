import QtQuick
import VGRPresenterUI

Item {
    id: root
    property string title: ""

    Column {
        anchors.centerIn: parent
        spacing: Theme.space2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.title
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXl
            font.weight: Font.DemiBold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Not built yet."
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
        }
    }
}
