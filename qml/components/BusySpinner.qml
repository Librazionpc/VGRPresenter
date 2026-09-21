import QtQuick
import QtQuick.Shapes

// A small "working on it" spinner: a three-quarter arc turning. Use it wherever something is
// loading and would otherwise look empty (a folder being read, a list waiting for data).
// It only turns while `running` (default: while visible), so a hidden one costs nothing.
Item {
    id: root

    property color color: "#8a94a6"
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
