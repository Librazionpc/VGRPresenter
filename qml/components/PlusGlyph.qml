import QtQuick

Item {
    id: root

    property real size: 14
    property real thickness: Math.max(1.2, size * 0.12)
    property color color: Theme.controlInk

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.centerIn: parent
        width: root.size
        height: root.thickness
        radius: root.thickness / 2
        color: root.color
    }
    Rectangle {
        anchors.centerIn: parent
        width: root.thickness
        height: root.size
        radius: root.thickness / 2
        color: root.color
    }
}