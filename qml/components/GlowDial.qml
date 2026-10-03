import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// A compact GLOWING dial -- the Audio and Video channel knob's glow-ring idea,
// promoted from a knob decoration into a small readout instrument. A
// 240-degree arc (the SAME sweep SpeedometerGauge uses, so the app's
// instruments agree with each other) glows in a caller-chosen colour: green
// healthy, amber near a limit, red past it. The value sits inside the arc and
// the caption sits on its own row underneath.
//
// Three separate bands -- arc, number, caption -- and no sweeping needle. An
// earlier version pivoted a needle at the value's own centre, so the arm swept
// straight through the number and the caption collided with the ring's bottom
// edge. A filled arc can never reach the middle of the dial, and the caption
// owns its own row, so nothing overlaps at any size. The dial is square and
// centred in whatever width the caller gives it, so a row of these lines up
// automatically; a call site only ever sets the width.
Item {
    id: root

    property real value: 0
    property real minValue: 0
    property string unit: ""
    property string caption: ""
    property real maxValue: 100
    // The glow colour: the caller's message (a resource tile picks it from the
    // budget the dial stands for). Instrument artwork, like SpeedometerGauge --
    // not an IconGlyph.
    property color dialColor: Theme.success
    // false: the machine cannot report this reading, so the dial goes quiet,
    // the arc stays empty and the number reads a dash instead of inventing a
    // value.
    property bool measured: true

    // The dial is square and capped, so a wide tile centres it rather than
    // stretching it into a flat oval.
    readonly property real dialSize: Math.max(56, Math.min(root.width, 116))
    readonly property real frac: (root.measured && root.maxValue > root.minValue)
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0
    readonly property string valueText: root.measured ? (Math.round(root.value) + root.unit) : "\u2014"
    readonly property color glowColor: root.measured ? root.dialColor : Theme.textMuted
    readonly property int captionGap: 6

    // The BUILT-IN implicitWidth/implicitHeight, so `height: implicitHeight`
    // at a call site is enough and nothing is clipped: arc, then caption.
    implicitWidth: 96
    implicitHeight: dial.height + root.captionGap + captionText.implicitHeight

    // ---- The arc, with its glow ----
    Item {
        id: dial
        width: root.dialSize
        height: Math.round(root.dialSize * 0.94)
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter

        readonly property real stroke: 9
        readonly property real cx: width / 2
        readonly property real cy: height * 0.56
        readonly property real radius: Math.max(20, Math.min(width / 2, height * 0.56) - stroke / 2 - 4)
        // Lower-left, over the top, to the lower-right -- PathAngleArc reports
        // angles clockwise with 0 at 3 o'clock, so 150 + 240 ends at 30.
        readonly property real startAngle: 150
        readonly property real sweepAngle: 240

        // The glow: two wider translucent passes under the live arc.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: root.measured ? 0.10 : 0.05
            ShapePath {
                fillColor: "transparent"
                strokeColor: root.glowColor
                strokeWidth: dial.stroke + 12
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: dial.cx; centerY: dial.cy
                    radiusX: dial.radius; radiusY: dial.radius
                    startAngle: dial.startAngle; sweepAngle: dial.sweepAngle
                }
            }
        }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: root.measured ? 0.20 : 0.10
            ShapePath {
                fillColor: "transparent"
                strokeColor: root.glowColor
                strokeWidth: dial.stroke + 5
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: dial.cx; centerY: dial.cy
                    radiusX: dial.radius; radiusY: dial.radius
                    startAngle: dial.startAngle; sweepAngle: dial.sweepAngle
                }
            }
        }

        // The empty track. Theme.borderSubtle, NOT Theme.inset: inset is a
        // hair off the card's own background, so an unmeasured dial (no live
        // arc over it) read as an empty hole rather than a quiet gauge.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: "transparent"
                strokeColor: Theme.borderSubtle
                strokeWidth: dial.stroke
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: dial.cx; centerY: dial.cy
                    radiusX: dial.radius; radiusY: dial.radius
                    startAngle: dial.startAngle; sweepAngle: dial.sweepAngle
                }
            }
        }

        // The live arc, filled to the value's fraction of the range.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            visible: root.frac > 0
            ShapePath {
                fillColor: "transparent"
                strokeColor: root.glowColor
                strokeWidth: dial.stroke
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: dial.cx; centerY: dial.cy
                    radiusX: dial.radius; radiusY: dial.radius
                    startAngle: dial.startAngle
                    sweepAngle: dial.sweepAngle * root.frac
                }
            }
        }

        // The value, inside the arc's opening -- an arc can never reach it.
        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -dial.height * 0.04
            text: root.valueText
            color: root.measured ? Theme.textPrimary : Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold
        }
    }

    // ---- The caption, on its own row under the arc ----
    Text {
        id: captionText
        anchors.top: dial.bottom
        anchors.topMargin: root.captionGap
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.width
        horizontalAlignment: Text.AlignHCenter
        text: root.caption
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        color: Theme.textMuted
        elide: Text.ElideRight
    }
}