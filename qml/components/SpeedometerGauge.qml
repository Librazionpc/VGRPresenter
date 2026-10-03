import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// A generic speedometer dial: a needle sweeping a 240-degree arc over the top,
// quarter ticks at the rim, the value big under the hub and a caption beneath
// it. Callers supply the value, the scale, the unit and the colour the dial
// takes; every part of the SHAPE lives here, so the throttle and temperature
// dials cannot drift apart. (Qt Quick Shapes — the same arc primitive
// BusySpinner/AppHeader use — not an IconGlyph: this is instrument artwork.)
Item {
    id: root

    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string unit: ""
    property string caption: ""
    property color dialColor: Theme.accent

    implicitWidth: 240
    implicitHeight: 158

    readonly property real frac: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0
    readonly property string valueText: Math.round(root.value) + root.unit

    readonly property real cx: root.width / 2
    readonly property real cy: root.height * 0.58
    readonly property real dialRadius: Math.max(24, Math.min(root.width / 2, root.height * 0.58) - 18)
    // The dial runs from the lower-left over the top to the lower-right.
    // PathAngleArc reports angles clockwise with 0 at 3 o'clock, so 150 + 240
    // ends at 30 degrees.
    readonly property real dialStart: 150
    readonly property real dialSweep: 240

    // The empty track.
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: Theme.inset
            strokeWidth: 10
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.cx
                centerY: root.cy
                radiusX: root.dialRadius
                radiusY: root.dialRadius
                startAngle: root.dialStart
                sweepAngle: root.dialSweep
            }
        }
    }

    // The filled part up to the current value.
    Shape {
        anchors.fill: parent
        visible: root.frac > 0
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.dialColor
            strokeWidth: 10
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.cx
                centerY: root.cy
                radiusX: root.dialRadius
                radiusY: root.dialRadius
                startAngle: root.dialStart
                sweepAngle: root.dialSweep * root.frac
            }
        }
    }

    // Quarter ticks around the rim.
    Repeater {
        model: 5
        Rectangle {
            required property int index
            readonly property real tickAngle: (root.dialStart + root.dialSweep * (index / 4)) * Math.PI / 180
            readonly property real tickRadius: root.dialRadius - 10
            x: root.cx + Math.cos(tickAngle) * tickRadius
            y: root.cy + Math.sin(tickAngle) * tickRadius - 1
            width: 6
            height: 2
            radius: 1
            color: Theme.border
            transform: Rotation {
                origin.x: 0
                origin.y: 1
                angle: root.dialStart + root.dialSweep * (index / 4)
            }
        }
    }

    // The needle.
    Rectangle {
        x: root.cx
        y: root.cy - 1
        width: root.dialRadius - 14
        height: 2
        radius: 1
        color: Theme.textPrimary
        transformOrigin: Item.Left
        rotation: root.dialStart + root.dialSweep * root.frac
        Behavior on rotation { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
    }

    // The hub.
    Rectangle {
        x: root.cx - 3
        y: root.cy - 3
        width: 6
        height: 6
        radius: 3
        color: Theme.textPrimary
    }

    // The readout, inside the dial under the hub.
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: root.cy + 10
        spacing: 1

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.valueText
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXl
            font.weight: Font.DemiBold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.caption
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}
