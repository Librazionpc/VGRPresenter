import QtQuick
import VGRPresenterUI

// The clickable LIVE/MUTE pill — THE mute toggle for every signal-carrying
// card on the routing board (audio input, video source, bus). One component
// so position recipe (top-right, 10/8 insets), muted styling, and the
// hit-area can't drift across the three columns. Controlled component:
// `muted` is owned by the caller; report intent via toggleRequested().
Pill {
    id: root

    property bool muted: false
    property color accent: Theme.success
    property color accentLight: Theme.successLight
    signal toggleRequested()

    anchors.rightMargin: 10
    anchors.topMargin: 8
    text: root.muted ? qsTr("MUTE") : qsTr("LIVE")
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
