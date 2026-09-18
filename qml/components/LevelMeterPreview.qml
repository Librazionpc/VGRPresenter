import QtQuick
import VGRPresenterUI

// Decorative "live input level" bar graph for the Add Source / Edit Audio
// Input dialogs (the reference's animated meter strip). DECORATIVE BY
// DESIGN: there is no audio engine anywhere in this app (levels, LIVE
// badges etc. are stored mock values, not telemetry), so this meter jitters
// a static grid of bars with a Timer — it previews what a live meter will
// look like, nothing more, and says so in its row caption.
//
// FIXED CANVAS — the one rule that keeps the dialogs stable: this component
// has an explicit implicitWidth/implicitHeight and the bars animate INSIDE
// it, never the other way around. The bars' heights change every tick, so
// any content-sized container (a Row/Column sizing itself to the bars)
// would resize on every frame and drag the whole dialog's layout with it —
// the modal visibly "breathing", with the scrollbar appearing chained to
// the animation because its height tracks the Flickable. Bars anchor to a
// fixed bottom baseline instead, so only pixels inside the box move.
//
// Bars are green → amber → red by column position (fixed bands), only their
// heights animate. The Timer runs ONLY while `active` is true, so a closed
// dialog never ticks in the background.
Item {
    id: root

    // Caller gates this on the dialog actually being open (ModalCard's
    // `shown`), not merely constructed.
    property bool active: false

    readonly property int barW: 6
    readonly property int barGap: 3
    readonly property int barCount: 20

    implicitWidth: barCount * barW + (barCount - 1) * barGap
    implicitHeight: 48

    Repeater {
        model: root.barCount

        delegate: Rectangle {
            id: bar
            required property int index

            x: index * (root.barW + root.barGap)
            width: root.barW
            anchors.bottom: parent.bottom
            radius: 2
            color: bar.index < 12 ? Theme.success
                 : bar.index < 17 ? Theme.warning
                 : Theme.danger

            // Bumped by the timer; the height binding follows. Per-bar phase
            // offsets make the jitter read as a wave, not a strobe.
            property real cycle: 0
            readonly property real phase: bar.index * 0.37

            height: 6 + (root.height - 6) * Math.abs(Math.sin(Math.PI * (cycle + phase)))

            Timer {
                interval: 150
                running: root.active
                repeat: true
                onTriggered: bar.cycle = (bar.cycle + 0.13) % 2
            }

            Behavior on height { NumberAnimation { duration: 130; easing.type: Easing.OutQuad } }
        }
    }
}
