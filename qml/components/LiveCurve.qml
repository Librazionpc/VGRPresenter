import QtQuick
import VGRPresenterUI

// The live loop's rate over the last minute, as bars — one bar per whole second
// of live show (see Telemetry's LiveSample: `liveHistory` is the engine's own
// per-second curve, oldest first, at most 60 of them).
//
// WHAT IT ANSWERS. The three meters above it in the Resource profile card say
// how BUSY the engine is; none of them says whether the show is getting SLOWER
// as the service goes on, because a live loop that composes one frame a second
// reads as a low rendering percentage next to a preview repainting at 60Hz. The
// bar is the rate the outputs were actually FED (presentFps) and the solid part
// inside it is how much of that was a real raster (composedFps) — the rest are
// copies of the composed frame (the dirty-frame re-present and the per-output
// sharing, see LiveOutputController). So a show that starts falling behind
// draws a descending staircase, and a static scene draws short soft bars with no
// solid part at all, which is what "nothing changed, the outputs are still fed"
// should look like.
//
// AND WHO DID IT. A shape alone leaves the operator with "it got slower" and no
// next step, so every second also carries the heaviest element of the frame
// composed in it (the renderer's own cost profile, published through Telemetry).
// The line under the bars names the hotspot of the WORST second in the window —
// the dip's culprit — rather than whatever the newest frame happened to be, so a
// service that dips when one video item comes on air says so by name.
//
// The window is fixed at `slots` seconds, so the newest second is always at the
// right edge and the same fall takes the same shape whatever the poll cadence.
// Bars come from the map the service publishes (same keys as `live`), so the
// component needs no engine access of its own — and it lays out plain
// Rectangles rather than running a Canvas paint pass, which keeps a card that
// the operator may leave open all service from spending work on the thread that
// draws.
Item {
    id: root

    // Bars in the window: 60 = one minute of live loop, the engine's own cap.
    property int slots: 60
    // Space above the bars inside this item, so it can be dropped into the
    // cards' shared Column without the caller wrapping it (the same idiom the
    // Text rows there use with topPadding).
    property int topGap: 14
    property int barHeight: 34
    property int barGap: 2

    readonly property var samples: TelemetryService.liveHistory
    readonly property var now: TelemetryService.live
    // How many leading slots are still empty: the curve grows in from the RIGHT,
    // so its x axis stays time (a minute ago on the left, now on the right) and
    // the first seconds of a show are the right-hand bars, not the left ones.
    readonly property int emptySlots: Math.max(0, root.slots - (root.samples ? root.samples.length : 0))
    // The second a slot stands for, or null when that second has not happened.
    function sampleAt(index) {
        var i = index - root.emptySlots
        if (!root.samples || i < 0 || i >= root.samples.length)
            return null
        return root.samples[i] || null
    }
    // The scale: the tallest delivered rate in the window, and never below 1 so
    // a quiet second is a real short bar rather than a division by zero.
    readonly property real peak: {
        var p = 1
        if (root.samples) {
            for (var i = 0; i < root.samples.length; ++i) {
                var v = root.samples[i] ? root.samples[i].presentFps : 0
                if (v > p)
                    p = v
            }
        }
        return p
    }
    readonly property real slotWidth: root.slots > 0
                                     ? Math.max(1, (root.width - (root.slots - 1) * root.barGap) / root.slots)
                                     : 1
    readonly property bool live: root.samples ? root.samples.length > 0 : false

    // The culprit behind the shape: the heaviest element of the composed frame in
    // the WORST second of the window that has one (lowest delivered rate), since
    // that is the dip being looked at. Falls back to the live snapshot, so a
    // running show names its current hotspot before the first second closes and
    // a curve whose low point happened to be a copy-only second still says
    // something useful. Null when nothing was composed yet.
    readonly property var hotspot: {
        var worst = null
        var worstFps = 0
        if (root.samples) {
            for (var i = 0; i < root.samples.length; ++i) {
                var s = root.samples[i]
                if (!s || !s.costElement)
                    continue
                var fps = Number(s.presentFps)
                if (worst === null || fps < worstFps) {
                    worst = s
                    worstFps = fps
                }
            }
        }
        if (worst !== null)
            return worst
        return (root.now && root.now.costElement) ? root.now : null
    }

    implicitHeight: root.topGap + root.barHeight + caption.height + 4
                    + (hotspotLine.visible ? hotspotLine.height + 2 : 0)

    // The window itself, so an empty minute still reads as a scale rather than
    // as nothing at all.
    Rectangle {
        x: 0
        y: root.topGap
        width: root.width
        height: root.barHeight
        radius: Theme.radiusSm
        color: Theme.inset
    }

    Row {
        x: 0
        y: root.topGap
        width: root.width
        height: root.barHeight
        spacing: root.barGap
        layoutDirection: Qt.LeftToRight

        // A FIXED model (the slot count), not the samples array: the bars are
        // then built once and only their heights re-bind as the curve grows, so a
        // card left open through a service is not rebuilding sixty items twice a
        // second on the thread that draws.
        Repeater {
            model: root.slots

            delegate: Item {
                required property int index
                readonly property var sample: root.sampleAt(index)
                width: root.slotWidth
                height: root.barHeight

                // The delivered rate that second: what every enabled output, the
                // preview and the NDI feed were handed.
                Rectangle {
                    visible: parent.sample !== null
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.sample !== null
                            ? Math.max(1, parent.height * Math.min(1, parent.sample.presentFps / root.peak))
                            : 0
                    radius: 2
                    color: Qt.alpha(Theme.accent, 0.35)
                }

                // The part of it that was a real raster (a copy is cheaper, and
                // that difference is the whole point of the sharing/dirty-frame
                // paths) — so the solid core is "what had to be drawn".
                Rectangle {
                    visible: parent.sample !== null && parent.sample.composedFps > 0
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.sample !== null
                            ? Math.max(1, parent.height * Math.min(1, parent.sample.composedFps / root.peak))
                            : 0
                    radius: 2
                    color: Theme.accent
                }
            }
        }
    }

    Text {
        id: caption
        y: root.topGap + root.barHeight + 4
        width: root.width
        text: {
            if (!root.live)
                return qsTr("Live curve · no session yet — the bars fill while a show is live (the newest second is on the right).")
            var rasterMs = root.now.frameMs !== undefined ? Number(root.now.frameMs) : 0
            var copyMs = root.now.representMs !== undefined ? Number(root.now.representMs) : 0
            var present = root.now.presentFps !== undefined ? Number(root.now.presentFps) : 0
            var composed = root.now.composedFps !== undefined ? Number(root.now.composedFps) : 0
            return qsTr("Live curve · last %1 s · delivered %2 fps · of that %3 fps were rasters · raster %4 ms · copy %5 ms (peak %6 fps)")
                    .arg(root.samples.length)
                    .arg(present.toFixed(1))
                    .arg(composed.toFixed(1))
                    .arg(rasterMs.toFixed(1))
                    .arg(copyMs.toFixed(1))
                    .arg(root.peak.toFixed(1))
        }
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSub
        wrapMode: Text.WordWrap
    }

    // WHICH ELEMENT MADE THE FRAME EXPENSIVE — the renderer accounts for the
    // pixel work of every element it draws (RenderEngine's cost profile), split
    // across that frame's measured cost, and each second of the curve carries
    // the heaviest LAYER of the frame composed in it. Naming it is the whole
    // point: "Images · Announcement video — 71% of the frame (≈12.3 ms)" is an
    // answer, while a shorter bar on its own is only a symptom.
    //
    // The scene's own background clear is deliberately NOT the name (it is a
    // full frame of pixels before any layer draws, so it would be reported every
    // second of every service); its share is carried alongside instead, because
    // a content share read without it can look like the whole story.
    Text {
        id: hotspotLine
        y: caption.y + caption.height + 2
        width: root.width
        visible: root.hotspot !== null
        text: {
            if (root.hotspot === null)
                return ""
            var share = root.hotspot.costSharePct !== undefined ? Number(root.hotspot.costSharePct) : 0
            var ms = root.hotspot.costMs !== undefined ? Number(root.hotspot.costMs) : 0
            var bg = root.hotspot.costBackgroundPct !== undefined ? Number(root.hotspot.costBackgroundPct) : 0
            var line = qsTr("Heaviest element: %1 — %2% of that frame's work (≈%3 ms of it)")
                    .arg(root.hotspot.costElement)
                    .arg(share.toFixed(0))
                    .arg(ms.toFixed(1))
            // The scene's background is the rest of the picture: saying so keeps
            // "the video is only 3%" from reading as "the frame is free".
            if (bg > 0)
                line += qsTr("; the scene's own background is %1% of the frame").arg(bg.toFixed(0))
            return line
        }
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSub
        elide: Text.ElideRight
    }
}
