import QtQuick

Item {
    id: root

    property var guides: []

    anchors.fill: parent
    z: 1000

    Repeater {
        model: root.guides
        delegate: Rectangle {
            required property var modelData

            color: "#ff5c9c"
            x: modelData.axis === "x" ? modelData.pos - 0.5 : 0
            y: modelData.axis === "y" ? modelData.pos - 0.5 : 0
            width: modelData.axis === "x" ? 1 : root.width
            height: modelData.axis === "y" ? 1 : root.height
        }
    }
}