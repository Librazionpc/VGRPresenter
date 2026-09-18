import QtQuick
import VGRPresenterUI

// One labeled usage meter — label left, percentage right, rounded bar
// underneath filling to the percentage. Shared by the Settings screens that
// show resource usage (General's Resource profile row, Smart Config's
// Resource budgets); extracted so the meter visual can't drift between
// them. Hand `label`, `pct`, and a width; the height is intrinsic.
Item {
    id: root

    property string label: ""
    property int pct: 0
    // Accent by default; a health-style meter (e.g. free disk space) can
    // pass Theme.success instead.
    property color barColor: Theme.accent

    implicitWidth: 200
    implicitHeight: 30

    Text {
        anchors.top: parent.top
        text: root.label
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }
    Text {
        anchors.top: parent.top
        anchors.right: parent.right
        text: root.pct + "%"
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 6
        radius: 3
        color: Theme.inset

        Rectangle {
            width: parent.width * root.pct / 100
            height: parent.height
            radius: 3
            color: root.barColor
            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
        }
    }
}
