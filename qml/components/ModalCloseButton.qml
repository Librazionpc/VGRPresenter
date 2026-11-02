import QtQuick
import VGRPresenterUI

// The ✕ in a modal header: one copy of the hit area, the hover tint and the glyph,
// so a modal only says what closing means for it.
Rectangle {
    id: btn

    signal clicked()

    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    width: 28
    height: 28
    radius: 7
    color: area.containsMouse ? Theme.hoverBg : "transparent"
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    Text {
        anchors.centerIn: parent
        text: "✕"
        color: Theme.textSecondary
        font.pixelSize: 14
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: btn.clicked()
    }
}
