import QtQuick
import VGRPresenterUI

// Reusable icon toggle with a clear disabled-state slash.
Item {
    id: root

    property string iconName: ""
    property color iconColor: Theme.accent
    property bool checked: true
    property string enabledTitle: qsTr("Disable output")
    property string disabledTitle: qsTr("Enable output")
    property string enabledDetail: qsTr("Click to stop this output")
    property string disabledDetail: qsTr("Click to resume this output")
    signal clicked()

    IconGlyph {
        anchors.centerIn: parent
        width: Math.min(parent.width, parent.height) * 0.55
        height: width
        fit: true
        name: root.iconName
        color: root.checked ? root.iconColor : Theme.textMuted
    }

    Rectangle {
        visible: !root.checked
        anchors.centerIn: parent
        width: Math.min(parent.width, parent.height) * 0.82
        height: 2
        radius: 1
        rotation: -45
        color: Theme.dangerLight
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    HoverDetail {
        anchors.fill: parent
        title: root.checked ? root.enabledTitle : root.disabledTitle
        detail: root.checked ? root.enabledDetail : root.disabledDetail
    }
}
