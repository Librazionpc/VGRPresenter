import QtQuick
import VGRPresenterUI

// The small outlined action that sits at the end of a value row ("Change", "None",
// "Pick", "Remove image"). One shape, one height, so the rows of a form stop each
// carrying their own hand-built copy:
//
//   PillButton { text: qsTr("Change"); onClicked: ... }
//   PillButton { text: qsTr("None"); tone: "muted"; onClicked: ... }
//
// tone: "accent" (the primary action of the row) or "muted" (a quiet secondary one).
Rectangle {
    id: root

    property string text: ""
    property string tone: "accent"
    signal clicked()

    implicitHeight: 28
    implicitWidth: label.implicitWidth + 28
    radius: Theme.radiusMd
    color: area.containsMouse ? Theme.chip : "transparent"
    border.width: 1
    border.color: area.containsMouse && root.tone === "accent" ? Qt.alpha(Theme.accent, 0.6) : Theme.border
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: root.tone === "accent" ? Theme.accentLight : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs + 1
        font.weight: Font.Medium
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
