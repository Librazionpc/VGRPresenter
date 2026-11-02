import QtQuick
import VGRPresenterUI

Item {
    id: root

    property real value: 0
    // Used only when fixedColor is set; otherwise the bar is the gradient below.
    property color fillColor: Theme.success
    property bool fixedColor: false
    property bool showDot: true

    implicitWidth: 100
    implicitHeight: 4

    readonly property real pct: Db.pct(root.value)
    // "is anything coming in at all" — mirrors Mic.svelte's rawDb > -60 dot: a small
    // nonzero floor so a hair of noise floor doesn't flicker the dot on its own.
    readonly property bool active: root.value > 1.5

    readonly property Gradient barGradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop { position: 0.0; color: Theme.meterQuiet }
        GradientStop { position: 0.55; color: Theme.meterLoud }
        GradientStop { position: 0.84; color: Theme.meterHot }
        GradientStop { position: 1.0; color: Theme.meterClip }
    }

    Row {
        anchors.fill: parent
        spacing: 4

        // Signal dot — on/off, not level-proportional (FreeShow's own convention).
        Rectangle {
            id: dot
            visible: root.showDot
            anchors.verticalCenter: parent.verticalCenter
            width: 4; height: 4; radius: 2
            color: root.fixedColor ? root.fillColor : Theme.meterQuiet
            opacity: root.active ? 1 : 0.2
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }

        Item {
            id: bar
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - (root.showDot ? dot.width + parent.spacing : 0)
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