import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The audio dialog's SIGNAL block, redesigned around the same idiom as the
// video dialogs' preview pane (labeled block at the TOP of the dialog):
// a dark 16:9-ish pane holding stacked level strips — one channel for mono
// sources, two side-by-side (L / R) for stereo — with the level wave
// inside them and a circular MIC MUTE button docked in the pane's corner.
//
// Why strips in a pane instead of the old floating bar grid: the strips
// ARE the channel image (mono = one line, stereo = two), the pane frames
// it like real metering hardware, and mute lives where your eyes already
// are — on the signal itself, not as a detached toggle row below.
//
// Signal truth: the wave animates ONLY when `live` (a source actually
// selected), and its amplitude scales with `volume` (0 = flat baseline).
// Muted flattens the strips too — a muted channel passes no signal.
Item {
    id: root

    // Caller gates: dialog open + a source that can carry audio.
    property bool live: false
    // 0-100 from the level dial — amplitude envelope of the wave.
    property real volume: 100
    // Muted state; the mute button reports clicks via toggled().
    property bool muted: false
    signal toggled()

    // Stereo shows L+R strips; mono shows a single (unlabeled) strip.
    property bool stereo: false
    // Explicit channel count (1..8) — wins over `stereo` when > 2. The
    // Channels block's stepper drives this: a 4-channel input shows four
    // labeled strips (1..4); the legacy bool still covers the plain
    // mono/stereo pair.
    property int channels: stereo ? 2 : 1
    // Live meter fill per channel (0..1 each) — real telemetry later; the
    // dial-driven wave ignores it for now (channels < 2).

    implicitWidth: 320
    implicitHeight: paneCol.implicitHeight

    Column {
        id: paneCol
        width: parent.width
        spacing: 6

        Rectangle {
            id: pane
            width: parent.width
            // Multi-channel panes grow a row per strip; the mono/stereo
            // pair keeps the original fixed heights.
            height: root.channels > 2
                    ? 20 + root.channels * 26 + (root.channels - 1) * 6
                    : (root.stereo ? 96 : 72)
            radius: Theme.radiusMd
            color: "#0d0f16"
            border.width: 1
            border.color: root.muted ? Theme.border : Theme.borderSubtle
            Behavior on border.color { ColorAnimation { duration: 120 } }

            // The strips — one mono line or a side-by-side L/R pair. Bars
            // grow UP from each strip's baseline like a meter bridge.
            Column {
                id: stripCol
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.rightMargin: 64   // room for the mute button
                spacing: 8

                Repeater {
                    model: root.channels > 2 ? root.channels : (root.stereo ? 2 : 1)

                    delegate: Item {
                        id: strip
                        required property int index
                        width: stripCol.width
                        height: root.channels > 2 ? 26 : (root.stereo ? 32 : 44)

                        // Channel tag — labeled for any multi-channel
                        // strip (L/R for stereo pairs, 1..N beyond).
                        Text {
                            visible: root.stereo || root.channels > 1
                            anchors.left: parent.left
                            anchors.top: parent.top
                            text: root.channels === 2
                                  ? (strip.index === 0 ? "L" : "R")
                                  : String(strip.index + 1)
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                        }

                        // One meter strip: a row of bottom-anchored bars.
                        Row {
                            id: bars
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            spacing: 3

                            Repeater {
                                model: root.channels > 2 ? 12 : 16

                                delegate: Rectangle {
                                    id: bar
                                    required property int index
                                    width: 6
                                    radius: 2
                                    anchors.bottom: parent.bottom
                                    // VU coloring by bar position — the
                                    // hotter the segment, the warmer the
                                    // color (matches LevelTrack's zones).
                                    color: bar.index < 9 ? Theme.success
                                         : bar.index < 13 ? Theme.warning
                                         : Theme.danger

                                    property real cycle: 0
                                    readonly property real phase: (strip.index * 0.5 + bar.index * 0.37)
                                    readonly property real envelope: root.live && !root.muted && root.volume > 0
                                                                     ? Math.abs(Math.sin(Math.PI * (cycle + phase))) : 0

                                    height: 4 + (strip.height - 6) * envelope * root.volFactor

                                    Timer {
                                        interval: 150
                                        running: root.live && !root.muted && root.volume > 0
                                        repeat: true
                                        onTriggered: bar.cycle = (bar.cycle + 0.13) % 2
                                    }
                                    Behavior on height { NumberAnimation { duration: 130; easing.type: Easing.OutQuad } }
                                }
                            }

                            // Bright playhead cap on the strip's leading
                            // edge — the "signal position" cue.
                            Rectangle {
                                visible: root.live && !root.muted && root.volume > 0
                                width: 2
                                height: strip.height - 6
                                radius: 1
                                color: "#e2e8f0"
                                opacity: 0.85
                                x: {
                                    const span = bars.width - 8
                                    const lead = Math.max(0, Math.min(1, root.volume / 100))
                                    return Math.max(0, Math.min(span, span * lead))
                                }
                            }
                        }
                    }
                }
            }

            // Circular MIC MUTE button — docked in the pane's right edge,
            // mic glyph when open, struck-through mic when muted. This is
            // THE mute control for the dialog (replaces the toggle row).
            Rectangle {
                id: muteBtn
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 44
                height: 44
                radius: 22
                color: root.muted ? "#33ff4d3d" : Theme.inset
                border.width: 1
                border.color: root.muted ? Theme.danger : Theme.border
                Behavior on color { ColorAnimation { duration: 120 } }
                Behavior on border.color { ColorAnimation { duration: 120 } }

                IconGlyph {
                    anchors.centerIn: parent
                    name: root.muted ? "micOff" : "mic"
                    color: root.muted ? "#ff6b61" : Theme.textPrimary
                    implicitWidth: 20
                    implicitHeight: 20
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggled()
                }
            }
        }

        // Caption row — mirrors the video preview's "decorative readout"
        // convention: channel format + honest signal state.
        Item {
            width: parent.width
            height: 14

            Text {
                anchors.left: parent.left
                text: root.channels > 2 ? qsTr("%1 channels").arg(root.channels)
                    : root.stereo ? qsTr("Stereo · L/R") : qsTr("Mono")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }
            Text {
                anchors.right: parent.right
                text: root.muted ? qsTr("MUTED")
                    : !root.live ? qsTr("NO SOURCE")
                    : root.volume > 0 ? qsTr("SIGNAL") : qsTr("SILENT")
                color: root.muted ? "#ff6b61"
                     : !root.live || root.volume <= 0 ? Theme.textMuted
                     : Theme.success
                font.family: Theme.fontFamily
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
        }
    }

    readonly property real volFactor: Math.max(0, Math.min(100, volume)) / 100
}
