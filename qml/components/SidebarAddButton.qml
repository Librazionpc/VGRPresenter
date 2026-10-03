import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The "+ New ..." pill at the foot of a library sidebar (the reference's "+ New collection"
// / "+ Add folder" box): a red plus and a label. Shared by the Media and the
// Scripture / The Table sidebars.
Rectangle {
    id: root

    property string text: ""

    signal clicked()

    height: 32
    radius: 6
    color: hover.hovered ? "#1e1f28" : "#12131a"
    border.color: Theme.border
    border.width: 1

    Row {
        anchors.centerIn: parent
        spacing: 6
        Shape {
            width: 8.17; height: 8.17
            anchors.verticalCenter: parent.verticalCenter
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: "transparent"
                strokeColor: Theme.accent
                strokeWidth: 2
                capStyle: ShapePath.RoundCap
                PathSvg { path: "M 0 4.08 L 8.17 4.08 M 4.08 0 L 4.08 8.17" }
            }
        }
        Text {
            text: root.text
            color: Theme.textSecondary
            font.family: Theme.fontFamily; font.pixelSize: 13
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    PositionHoverArea {
        id: hover
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
