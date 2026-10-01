import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "."

// The video dialogs' preview pane — ONE component for Add Source and Edit
// Video Source (they used to carry byte-identical ~150-line copies of this
// whole block, which drifted the moment NDI volume landed: two places to
// remember). Owns the 16:9 pane, the per-kind glyph art, the MUTE/PAUSED
// pill, the Resolution mode pill and the kind-specific hint. Controlled:
// the consumer owns kind/muted/mode and reacts to mutedToggled()/modePicked.
//
// The pane is LIVE for camera, screen AND NDI rows: the consumer passes
// previewLabel (the device's roster label / the NDI source's full name); the
// pane starts the PAL's MF Source Reader tap (camera) / the monitor tap
// (screen) / the engine's NDI receiver (NDI) and pumps
// image://videopreview/<label>?<nonce> at ~15 fps — REAL frames under the
// glyphs. Only media keeps the decorative glyph art (a file preview is not
// a live tap).
//
// Column root, like ProAudioForm: positioners auto-size from content, so
// consumers only set width and the pane + hint stack correctly.
Column {
    id: root

    // "camera" | "screen" | "media" | "ndi" — selects the glyph and the
    // pill/hint wording.
    property string kind: "camera"
    // The source device's roster label — camera name, monitor label, or the
    // NDI source's full discovery name. non-empty + kind camera/screen/ndi
    // turns the pane live (a real tap / engine receiver). Empty = decorative.
    property string previewLabel: ""
    property bool muted: false
    // Current capture-mode pick ("" = unset — the pill shows its fallback).
    property string mode: ""
    // Offered capture modes + the device's own fps ceiling (0 = no gate —
    // NDI/network feeds have no local capability list).
    property var modes: []
    property real maxFps: 0
    // The Edit dialog shows the kind-specific hint under the pane; the Add
    // dialog's column is tighter and passes false.
    property bool showHint: true
    // objectName for the pane rectangle (self-test grab handle; "" = none).
    property string paneObjectName: ""

    signal mutedToggled()
    signal modePicked(string mode)

    // ---- Live preview plumbing ------------------------------------------
    // The nonce is the re-fetch trigger: QML's image cache keys the whole
    // URL, so bumping ?n= forces a fresh drain of the PAL's newest JPEG.
    // NOTE: never assign the same URL twice — QML dedupes identical sources
    // and won't re-request (the nonce changes every tick, so this only
    // matters if the pump stalls).
    property int previewNonce: 0
    readonly property bool live: (kind === "camera" || kind === "screen" || kind === "ndi")
                                 && previewLabel !== ""
    // The tap registered for THIS pane, reconciled (never assumed): switching
    // cameras changes previewLabel while live stays true, so a start wired to
    // onLiveChanged alone left every re-pick tapless (the "no tap was started"
    // storm) with the old camera still streaming. Reconcile STOPS the old
    // label's tap and starts the new one — switching sources FREES the old
    // device (user policy: no pooled keeps-alive; a paused/switched-away
    // camera must release its hardware). Born-live panes (Edit dialog opens
    // with a device already picked) get no change signal at all, so
    // onCompleted reconciles too.
    property string activeTapLabel: ""
    // The ACTIVE tap's kind — a re-pick can flip kind camera↔screen while
    // switching labels, and the stop must address the OLD label's kind (the
    // row-thumbnail reconcile tracks this too).
    property string activeTapKind: ""
    function syncTap() {
        const want = effectiveLive ? previewLabel : ""
        if (want === activeTapLabel)
            return
        const old = activeTapLabel
        const oldKind = activeTapKind
        activeTapLabel = want
        activeTapKind = want !== "" ? kind : ""
        if (old !== "") {
            if (oldKind === "screen")
                EngineBridge.stopScreenPreview(old, "dialog")
            else if (oldKind === "ndi")
                EngineBridge.stopNdiPreview(old, "dialog")
            else
                EngineBridge.stopVideoPreview(old, "dialog")
        }
        if (want !== "") {
            if (kind === "screen")
                EngineBridge.startScreenPreview(want, "dialog")
            else if (kind === "ndi")
                EngineBridge.startNdiPreview(want, "dialog")
            else
                EngineBridge.startVideoPreview(want, mode, "dialog")
            previewNonce++
            previewTimer.restart()
        }
    }
    // PAUSE FREES THE DEVICE: kind camera/screen + empty previewLabel OR a
    // paused pane drops the effective-live → false → reconcile stops the
    // tap (the hardware releases; the camera light goes off). Restarting is
    // a plain re-pick. NDI's MUTE pill is AUDIO-only (embedded-audio level),
    // so a muted NDI pane stays live — the same rule the board row applies.
    readonly property bool effectiveLive: live && (kind === "ndi" || !muted)
    onEffectiveLiveChanged: syncTap()
    onPreviewLabelChanged: syncTap()
    Component.onCompleted: syncTap()
    // Pause frees the device (see effectiveLive below); the frame pump and
    // source follow the same state so a paused pane shows nothing.
    // A Resolution re-pick re-locks the tap to the new size (cameras only —
    // screens have no mode; the tap always follows the current resolution).
    onModeChanged: {
        if (effectiveLive && activeTapLabel === previewLabel && kind === "camera") {
            EngineBridge.startVideoPreview(previewLabel, mode, "dialog")
            previewNonce++
        }
    }
    Timer {
        id: previewTimer
        interval: 66   // ~15 fps, matching the tap's production rate
        repeat: true
        running: root.effectiveLive
        onTriggered: root.previewNonce++
    }
    readonly property url frameSource: effectiveLive
        ? "image://videopreview/" + encodeURIComponent(previewLabel) + "?n=" + previewNonce : ""
    // True once REAL frames are flowing — the warm-up test for the decorative
    // glyphs (they belong to the seconds before the first frame, not on top
    // of a live feed).
    readonly property bool frameReady: effectiveLive && frameImage.status === Image.Ready
                                       && frameImage.sourceSize.width > 1

    // Audio-carrying kinds — the same rule the dialogs gate the Volume
    // slider with (media files and NDI streams embed audio; camera/screen
    // feeds are silent, so their pill reads PAUSED, not MUTE).
    readonly property bool kindHasAudio: kind === "media" || kind === "ndi"

    spacing: Theme.space2

    Text {
        text: qsTr("Preview")
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
    }

    Rectangle {
        // Self-test grab handle — set per usage (the Edit dialog's pane must
        // NOT share it: findItem would hit whichever instantiates first).
        objectName: paneObjectName
        width: parent.width
        height: width * 9 / 16
        radius: Theme.radiusMd
        color: "#0d0f16"
        border.width: 1
        border.color: Theme.border
        clip: true

        // LIVE FRAMES — the tap's newest JPEG, aspect-fit inside the pane,
        // ABOVE the pane's background but BELOW the glyphs (which stay for
        // the warm-up seconds before the first frame arrives).
        Image {
            id: frameImage
            // Self-test witness: the sweep logs status/size at grab time —
            // the decisive evidence for "frames SERVED but pane dark".
            objectName: root.paneObjectName !== "" ? root.paneObjectName + "Image" : ""
            anchors.fill: parent
            anchors.margins: 1
            source: root.frameSource
            visible: root.frameReady
            fillMode: root.kind === "screen" ? Image.PreserveAspectFit : Image.PreserveAspectFit
            cache: false   // every nonce IS a new frame — never cache stale ones
        }

        Item {
            anchors.centerIn: parent
            visible: root.kind === "camera" && !root.frameReady
            width: 64; height: 64

            Rectangle {
                anchors.centerIn: parent
                width: 44; height: 44; radius: 22
                color: "transparent"
                border.width: 3
                border.color: Theme.textMuted
            }
            Rectangle {
                anchors.centerIn: parent
                width: 14; height: 14; radius: 7
                color: Theme.textMuted
            }
        }

        // NDI — broadcast ripples (center dot + two rings): a
        // network feed, not a local device. Warm-up only — they retire
        // once real frames flow (same rule as the camera lens).
        Item {
            anchors.centerIn: parent
            visible: root.kind === "ndi" && !root.frameReady
            width: 56; height: 56

            Rectangle {
                anchors.centerIn: parent
                width: 10; height: 10; radius: 5
                color: Theme.textMuted
            }
            Rectangle {
                anchors.centerIn: parent
                width: 30; height: 30; radius: 15
                color: "transparent"
                border.width: 2.4
                border.color: Theme.textMuted
            }
            Rectangle {
                anchors.centerIn: parent
                width: 52; height: 52; radius: 26
                color: "transparent"
                border.width: 2
                border.color: Theme.textMuted
            }
        }

        Item {
            anchors.centerIn: parent
            visible: root.kind === "screen" && !root.frameReady
            width: 80; height: 60

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: 64; height: 42; radius: 3
                color: "transparent"
                border.width: 2.4
                border.color: Theme.textMuted
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 24; height: 3; radius: 1.5
                color: Theme.textMuted
            }
        }

        Shape {
            anchors.centerIn: parent
            visible: root.kind === "media"
            width: 40; height: 46
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                fillColor: Theme.textMuted
                strokeColor: "transparent"
                startX: 0; startY: 0
                PathLine { x: 0; y: 46 }
                PathLine { x: 40; y: 23 }
                PathLine { x: 0; y: 0 }
            }
        }

        // The pill IS the toggle — same convention as every card on
        // the board, so there's no separate toggle row below.
        MutePill {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 10
            muted: root.muted
            offLabel: root.kindHasAudio ? qsTr("MUTE") : qsTr("PAUSED")
            accent: Theme.info
            accentLight: Theme.infoLight
            onToggleRequested: root.mutedToggled()
        }

        // The mode pill IS the Resolution control: click it and the
        // capture-mode list drops from the pill (fallback text while
        // nothing is picked), anything beyond the device's own max
        // fps greyed. Media sources have no capture modes — no pill.
        ModePillButton {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 10
            anchors.bottomMargin: 10
            visible: root.kind !== "media"
            text: root.mode !== "" ? root.mode : qsTr("1080p · 60fps")
            modes: root.modes
            maxFps: root.maxFps
            onModePicked: (m) => root.modePicked(m)
        }
    }

    // Kind-specific hint: media/NDI mute their embedded audio,
    // camera/screen pause the feed itself.
    Text {
        width: parent.width
        visible: root.showHint
        text: root.kind === "media"
              ? qsTr("Muting silences this media's audio track.")
              : root.kind === "ndi"
                ? qsTr("NDI carries audio with its video — Volume sets this source's embedded-audio level; muting silences it.")
                : qsTr("Pausing blacks this feed out at the bus it feeds — cameras and screens carry no audio track.")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        wrapMode: Text.WordWrap
    }
}
