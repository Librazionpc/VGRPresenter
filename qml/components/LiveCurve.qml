import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The live loop's rate over the last minute, drawn as a graph — the engine's own
// per-second curve (Telemetry's `liveHistory`, oldest first, at most 60 seconds).
//
// TWO LINES, ONE QUESTION: "is the show being fed, and is it getting slower?"
//   the filled line     the rate the outputs were actually DELIVERED (presentFps)
//   the inner line      how much of it was a real raster (composedFps) — the rest are
//                       copies of the composed frame, which is why a still slide draws a
//                       steady delivered line with the raster line resting near zero
// A show that starts falling behind draws a falling slope and a spike shows as a
// spike. The newest second is always at the right edge, so the same fall takes the
// same shape whatever the poll cadence; a young show grows in from the right.
//
// The scale is a round number above the highest second in the window (15, 30, 60...
// fps), labelled on the left with a faint grid, so a peak reads as a height rather
// than as an unlabelled squiggle. The readout at the top right is the newest
// second's delivered rate.
//
// Drawn with Shapes (one polyline per line, one filled polygon), not a Canvas paint
// pass: a card that the operator may leave open all service must not spend work on
// the thread that draws, and the points only rebuild when a new second arrives.
Item {
    id: root

    // Seconds in the window: 60 = one minute of live loop, the engine's own cap.
    property int slots: 60
    // Space above the legend inside this item (the cards' shared Column idiom).
    property int topGap: 14
    property int chartHeight: 112

    readonly property var samples: TelemetryService.liveHistory
    readonly property var now: TelemetryService.live
    readonly property bool live: root.samples ? root.samples.length > 0 : false
    readonly property int emptySlots: Math.max(0, root.slots - (root.samples ? root.samples.length : 0))

    // Plot area: a left gutter for the scale labels, a hair of air on the right.
    readonly property real plotX: 34
    readonly property real plotW: Math.max(10, root.width - root.plotX - 6)
    readonly property real padTop: 8
    readonly property real padBottom: 6
    readonly property real plotH: root.chartHeight - root.padTop - root.padBottom

    // The tallest delivered second in the window, and the round scale above it.
    readonly property real peak: {
        var p = 1
        if (root.samples) {
            for (var i = 0; i < root.samples.length; ++i) {
                var v = root.samples[i] ? Number(root.samples[i].presentFps) : 0
                if (v > p)
                    p = v
            }
        }
        return p
    }
    function niceMax(v) {
        var steps = [15, 30, 60, 90, 120, 144, 240]
        for (var i = 0; i < steps.length; ++i)
            if (v * 1.08 <= steps[i])
                return steps[i]
        return Math.ceil(v * 1.08 / 60) * 60
    }
    readonly property real scaleMax: root.niceMax(root.peak)

    function yFor(value) {
        return root.padTop + root.plotH * (1 - Math.max(0, Math.min(1, value / root.scaleMax)))
    }
    function pointsFor(field) {
        var out = []
        if (!root.samples)
            return out
        var step = root.plotW / Math.max(1, root.slots - 1)
        for (var i = 0; i < root.samples.length; ++i) {
            var s = root.samples[i]
            out.push(Qt.point(root.plotX + (root.emptySlots + i) * step,
                              root.yFor(s ? Number(s[field]) : 0)))
        }
        return out
    }
    readonly property var presentPoints: root.pointsFor("presentFps")
    readonly property var rasterPoints: root.pointsFor("composedFps")
    // The delivered line closed down to the floor: the filled area under it.
    readonly property var areaPoints: {
        var pts = root.presentPoints
        if (pts.length < 2)
            return []
        var floorY = root.padTop + root.plotH
        var out = [Qt.point(pts[0].x, floorY)]
        for (var i = 0; i < pts.length; ++i)
            out.push(pts[i])
        out.push(Qt.point(pts[pts.length - 1].x, floorY))
        return out
    }
    readonly property real nowFps: (root.now && root.now.presentFps !== undefined) ? Number(root.now.presentFps) : 0

    implicitHeight: root.topGap + legend.height + 10 + root.chartHeight

    // ---- Legend + the readout --------------------------------------------------
    Item {
        id: legend
        y: root.topGap
        width: root.width
        height: 22

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 16

            Row {
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter
                Rectangle { width: 10; height: 3; radius: 1.5; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: qsTr("Delivered")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
            }
            Row {
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter
                Rectangle { width: 10; height: 3; radius: 1.5; color: Theme.accentLight; opacity: 0.7; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: qsTr("Drawn")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
            }
        }

        // The newest second, large: the number the graph is about.
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 5
            Text {
                text: root.live ? root.nowFps.toFixed(0) : "—"
                color: root.live ? Theme.textPrimary : Theme.textMuted
                font.family: FontService.monoFamily
                font.pixelSize: 17
                font.weight: Font.DemiBold
                anchors.baseline: unit.baseline
            }
            Text {
                id: unit
                text: qsTr("fps")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
        }
    }

    // ---- The plot ----------------------------------------------------------------
    Rectangle {
        id: plot
        y: root.topGap + legend.height + 10
        width: root.width
        height: root.chartHeight
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: Theme.borderSubtle
        clip: true

        // Grid: the floor, the middle, the top of the scale — labelled.
        Repeater {
            model: [0, 0.5, 1]
            delegate: Item {
                required property real modelData
                readonly property real lineY: root.padTop + root.plotH * (1 - modelData)
                width: plot.width
                height: 1
                y: lineY

                Rectangle {
                    x: root.plotX
                    width: root.plotW
                    height: 1
                    color: modelData === 0 ? Theme.border : Theme.borderSubtle
                    opacity: modelData === 0 ? 0.9 : 0.55
                }
                Text {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.round(root.scaleMax * modelData)
                    color: Theme.textMuted
                    font.family: FontService.monoFamily
                    font.pixelSize: 9
                }
            }
        }

        Shape {
            anchors.fill: parent
            visible: root.live
            layer.enabled: true
            layer.samples: 4

            // The filled area under the delivered line.
            ShapePath {
                strokeColor: "transparent"
                fillGradient: LinearGradient {
                    x1: 0; y1: root.padTop
                    x2: 0; y2: root.padTop + root.plotH
                    GradientStop { position: 0.0; color: Qt.alpha(Theme.accent, 0.42) }
                    GradientStop { position: 1.0; color: Qt.alpha(Theme.accent, 0.02) }
                }
                PathPolyline { path: root.areaPoints }
            }

            // How much of it was a real raster.
            ShapePath {
                strokeColor: Qt.alpha(Theme.accentLight, 0.7)
                strokeWidth: 1.4
                fillColor: "transparent"
                joinStyle: ShapePath.RoundJoin
                capStyle: ShapePath.RoundCap
                PathPolyline { path: root.rasterPoints }
            }

            // The delivered line, on top.
            ShapePath {
                strokeColor: Theme.accent
                strokeWidth: 2
                fillColor: "transparent"
                joinStyle: ShapePath.RoundJoin
                capStyle: ShapePath.RoundCap
                PathPolyline { path: root.presentPoints }
            }
        }

        // The newest second: a dot with a soft ring, at the right end of the line.
        Item {
            visible: root.live && root.presentPoints.length > 0
            readonly property var tip: root.presentPoints.length > 0
                                       ? root.presentPoints[root.presentPoints.length - 1] : Qt.point(0, 0)
            x: tip.x
            y: tip.y
            Rectangle { x: -7; y: -7; width: 14; height: 14; radius: 7; color: Qt.alpha(Theme.accent, 0.22) }
            Rectangle { x: -3.5; y: -3.5; width: 7; height: 7; radius: 3.5; color: Theme.accentLight; border.width: 1.5; border.color: Theme.accent }
        }

        // Nothing is live yet: a flat resting line, so the card still reads as a graph.
        Rectangle {
            visible: !root.live
            x: root.plotX
            y: root.padTop + root.plotH - 1
            width: root.plotW
            height: 2
            radius: 1
            color: Theme.border
        }
        Text {
            visible: !root.live
            anchors.centerIn: parent
            text: qsTr("Waiting for a live show")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}
