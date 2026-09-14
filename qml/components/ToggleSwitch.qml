import QtQuick
import VGRPresenterUI

// Off = grey track + flush-left knob. On = tinted track + white knob flush-right.
Item {
    id: root

    property bool checked: false
    property color onColor: Theme.accent
    // `enabled` is inherited from Item — no need to redeclare it.

    signal toggled(bool checked)

    implicitWidth: 36
    implicitHeight: 20

    Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? root.onColor : Theme.toggleOffTrack
        opacity: root.enabled ? 1.0 : 0.5
        Behavior on color { ColorAnimation { duration: 140 } }
    }

    Rectangle {
        id: knob
        width: 16
        height: 16
        radius: width / 2
        anchors.verticalCenter: parent.verticalCenter
        x: root.checked ? root.width - width - 2 : 2
        color: "#ffffff"
        Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.checked = !root.checked
            root.toggled(root.checked)
        }
    }
}
