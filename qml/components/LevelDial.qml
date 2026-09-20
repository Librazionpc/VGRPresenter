import QtQuick
import VGRPresenterUI

// Circular level knob (the reference Add Source dialog's big dial —
// VGRPresenter_Settings_Audio_Video_Add.qml's circular volume control).
// Controlled-component contract identical to LabeledSlider.qml: `value` is
// owned by the caller, the dial only *reports* via moved()/dragStarted()/
// dragFinished() and never assigns its own `value` (self-assigning would
// kill the consumer's binding — the bug class that convention exists for).
//
// The pointer is a plain rotated Rectangle pivoting at the knob's center
// (transformOrigin: Item.Bottom on a pointer whose bottom sits on the
// center) sweeping -135°..+135°. No PathArc/Shape ring — the reference
// image shows a dial pointer, not a filled arc, and a rotated pointer is
// the simpler, lower-risk drawing. Angle math is the inverse of the drag
// mapping: atan2 around the center, clamped to the sweep.
Item {
    id: root

    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string label: ""

    signal moved(real value)
    // Same gesture-bracketing pair as LabeledSlider — a whole drag reports
    // as one gesture, not one event per pixel.
    signal dragStarted()
    signal dragFinished()

    implicitWidth: 120
    implicitHeight: knob.height + captionCol.implicitHeight + Theme.space2

    readonly property real pct: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0
    // 0% points -135° (lower-left), 100% points +135° (lower-right).
    readonly property real pointerRotation: -135 + 270 * pct

    Item {
        id: knob
        width: 110
        height: 110
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top

        Rectangle {
            id: body
            anchors.fill: parent
            radius: width / 2
            color: Theme.inset
            border.width: 1
            border.color: dragArea.pressed ? Theme.accent : Theme.border
            Behavior on border.color { ColorAnimation { duration: 100 } }
        }

        // Pointer — pivots around its own bottom edge, which sits exactly on
        // the knob's center (y + height == knob center). Accent, matching
        // LabeledSlider's accent fill — the app's control-active color.
        Rectangle {
            width: 3
            height: knob.width / 2 - 14
            radius: 1.5
            color: Theme.accent
            x: knob.width / 2 - width / 2
            y: knob.height / 2 - height
            transformOrigin: Item.Bottom
            rotation: root.pointerRotation
            Behavior on rotation { NumberAnimation { duration: 40 } }
        }

        // Center cap over the pivot.
        Rectangle {
            anchors.centerIn: parent
            width: 16
            height: 16
            radius: 8
            color: Theme.surface
            border.width: 1
            border.color: Theme.border
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            // The dial lives inside scrollable dialogs — without this the
            // Flickable steals the drag a few pixels in and the page scrolls
            // mid-adjustment (the "scroller changes while adjusting volume"
            // bug).
            preventStealing: true

            onPressed: (mouse) => {
                root.dragStarted()
                setFromPosition(mouse.x, mouse.y)
            }
            onPositionChanged: (mouse) => setFromPosition(mouse.x, mouse.y)
            onReleased: root.dragFinished()
            onCanceled: root.dragFinished()

            function setFromPosition(px, py) {
                // Screen coords → knob-local, then atan2 with 0° pointing up
                // and positive clockwise (matching the pointer's sweep).
                //
                // DEAD-ZONE handling — the sweep spans -135°..+135°, so the
                // bottom 90° is unreachable. Raw atan2 wraps through it:
                // dragging just past the lower-left stop lands at ~±180°,
                // and naive clamping of THAT would snap to +135 → the dial
                // JUMPED TO 100% on a small anti-clockwise overshoot.
                // Hardware-knob end stops instead: angles in the dead zone
                // hold the NEAREST stop (-180..-135 → -135 → 0%; +135..+180
                // → +135 → 100%), so overshooting a stop never changes the
                // value — and never wraps to the opposite end.
                const dx = px - knob.width / 2
                const dy = py - knob.height / 2
                const deg = Math.atan2(dx, -dy) * 180 / Math.PI
                let stopAngle = deg
                if (deg < -135)
                    stopAngle = -135   // dead zone: hold the lower stop (0%)
                else if (deg > 135)
                    stopAngle = 135    // dead zone: hold the upper stop (100%)
                root.moved(root.minValue + (stopAngle + 135) / 270 * (root.maxValue - root.minValue))
            }
        }
    }

    Column {
        id: captionCol
        anchors.top: knob.bottom
        anchors.topMargin: Theme.space2
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 1

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Math.round(root.value) + "%"
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }

        Text {
            visible: root.label !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.label
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}
