import QtQuick
import VGRPresenterUI

// The small level meter on a routing-board card (audio rows, video media rows, buses).
// FreeShow's own compact meter (src/frontend/components/drawer/live/Mic.svelte): a signal
// dot beside a thin bar. The bar's a CONTINUOUS gradient (cyan -> green -> amber -> red),
// not discrete LED segments — a faint "ghost" of the full range stays visible at rest (so
// the scale reads even at silence), and the lit portion reveals left-to-right as `value`
// rises. Extracted so track/fill geometry can't drift across delegates — place it and give
// it `value` (0..100, clamped).
//
// COLOR IS VALUE-DRIVEN, like a real VU meter, EXCEPT when a caller passes a deliberate
// fixed color (fixedColor: true — e.g. a bus row tinting by TYPE, not level): then the bar
// is a flat fill in that color instead of the gradient, same as before.
Item {
    id: root

    property real value: 0
    // Used only when fixedColor is set; otherwise the bar is the gradient below.
    property color fillColor: Theme.success
    property bool fixedColor: false

    implicitWidth: 100
    implicitHeight: 4

    // `value` (0..100) is a raw LINEAR amplitude percentage straight off the
    // WASAPI tap. Meters read in dB, not linear amplitude, so the FILL is
    // mapped on the same -60..0 dB scale the gradient's zones assume — the
    // one shared mapping lives in the Db singleton (was a private copy here,
    // OutputMonitorTile and ProAudioForm; three copies, three ways to drift).
    readonly property real pct: Db.pct(root.value)
    // "is anything coming in at all" — mirrors Mic.svelte's rawDb > -60 dot: a small
    // nonzero floor so a hair of noise floor doesn't flicker the dot on its own.
    readonly property bool active: root.value > 1.5

    readonly property Gradient barGradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: "#00c8c8" }
        GradientStop { position: 0.55; color: "#00ff32" }
        GradientStop { position: 0.84; color: "#ffc800" }
        GradientStop { position: 1.0; color: "#c80000" }
    }

    Row {
        anchors.fill: parent
        spacing: 4

        // Signal dot — on/off, not level-proportional (FreeShow's own convention).
        Rectangle {
            id: dot
            anchors.verticalCenter: parent.verticalCenter
            width: 4; height: 4; radius: 2
            color: root.fixedColor ? root.fillColor : "#00c8c8"
            opacity: root.active ? 1 : 0.2
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }

        Item {
            id: bar
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - dot.width - parent.spacing
            height: parent.height

            Rectangle {
                id: track
                anchors.fill: parent
                color: Theme.chip
            }

            // Ghost — the full range, always faintly visible, so the scale reads at rest.
            Rectangle {
                anchors.fill: parent
                opacity: 0.12
                color: root.fixedColor ? root.fillColor : "transparent"
                gradient: root.fixedColor ? null : root.barGradient
            }

            // Lit portion — clipped to `value`; the gradient is sized to the FULL bar
            // width so the revealed colors line up with the ghost underneath it.
            Item {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: bar.width * root.pct
                clip: true
                // FreeShow's own Mic.svelte transition: width 0.05s ease.
                Behavior on width { NumberAnimation { duration: 50; easing.type: Easing.OutQuad } }

                Rectangle {
                    width: bar.width
                    height: parent.height
                    color: root.fixedColor ? root.fillColor : "transparent"
                    gradient: root.fixedColor ? null : root.barGradient
                }
            }
        }
    }
}
