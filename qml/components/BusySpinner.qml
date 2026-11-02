import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property color color: Theme.textSecondary
    property real thickness: 2.5
    property bool running: visible

    implicitWidth: 28
    implicitHeight: 28

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: root.thickness
            capStyle: ShapePath.RoundCap

            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: Math.max(1, root.width / 2 - root.thickness)
                radiusY: Math.max(1, root.height / 2 - root.thickness)
                startAngle: -90
                sweepAngle: 270
            }
        }

        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: root.running
        }
    }
}