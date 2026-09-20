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
// The pane stays decorative (no video engine surface behind it yet, same
// mock convention as the board cards' thumbs) — but there is exactly one
// place to change that now.
//
// Column root, like ProAudioForm: positioners auto-size from content, so
// consumers only set width and the pane + hint stack correctly.
Column {
    id: root

    // "camera" | "screen" | "media" | "ndi" — selects the glyph and the
    // pill/hint wording.
    property string kind: "camera"
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

    signal mutedToggled()
    signal modePicked(string mode)

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
        width: parent.width
        height: width * 9 / 16
        radius: Theme.radiusMd
        color: "#0d0f16"
        border.width: 1
        border.color: Theme.border
        clip: true

        Item {
            anchors.centerIn: parent
            visible: root.kind === "camera"
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
        // network feed, not a local device.
        Item {
            anchors.centerIn: parent
            visible: root.kind === "ndi"
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
            visible: root.kind === "screen"
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
