import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "."

Column {
    id: root

    // "camera" | "screen" | "media" | "ndi" — selects the glyph and the
    // pill/hint wording.
    property string kind: "camera"
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
    property int previewNonce: 0
    readonly property bool live: (kind === "camera" || kind === "screen" || kind === "ndi")
                                 && previewLabel !== ""
    property string activeTapLabel: ""
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
    readonly property bool effectiveLive: live && (kind === "ndi" || !muted)
    onEffectiveLiveChanged: syncTap()
    onPreviewLabelChanged: syncTap()
    Component.onCompleted: syncTap()
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
    readonly property bool frameReady: effectiveLive && frameImage.status === Image.Ready
                                       && frameImage.sourceSize.width > 1

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
        color: Theme.windowBg
        border.width: 1
        border.color: Theme.border
        clip: true

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