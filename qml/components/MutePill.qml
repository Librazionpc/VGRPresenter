import QtQuick
import VGRPresenterUI

Pill {
    id: root

    property bool muted: false
    property color accent: Theme.success
    property color accentLight: Theme.successLight
    property string offLabel: qsTr("MUTE")
    signal toggleRequested()

    anchors.rightMargin: 10
    anchors.topMargin: 8
    text: root.muted ? root.offLabel : qsTr("LIVE")
    baseColor: root.muted ? Theme.textMuted : root.accent
    lightColor: root.muted ? Theme.textSecondary : root.accentLight
    tint: true
    tintAlpha: root.muted ? 0.12 : 0.18
    fontSize: 9

    MouseArea {
        anchors.fill: parent
        anchors.margins: -6
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggleRequested()
    }
}