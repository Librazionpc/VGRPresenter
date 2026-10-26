import QtQuick

// Reusable "label — value — draggable track" slider row: a label on the
// left, the current value on the right, and a horizontal track/thumb below.
// Used by BackgroundColorModal.qml's Opacity row. Controlled-component
// convention: `value` is owned by the consumer, the slider only *reports*
// changes via `moved` and never writes back to its own `value` property —
// self-assigning it would break the consumer's `value: <binding>` (a QML
// binding dies the first time something assigns the property imperatively),
// which is exactly the bug that used to force consumers into imperative
// re-push mirroring. The consumer reassigns `value` from its model, the
// thumb follows, the Binding{when: !drag.active} handoff (same pattern as
// AppScrollBar.qml) keeps the drag and the declarative thumb position from
// fighting.
//
// Literal colors, pending migration to Theme.* — the "AOT singleton-
// resolution" reason once given for this file family is disproven (see
// DropdownPanel.qml's header).
Item {
    id: root

    property string label: ""
    property real value: 0
    property real minValue: 0
    property real maxValue: 100
    property string suffix: ""
    property int decimals: 0
    // The label/value inks are properties so a consumer can adopt FreeShow's
    // field language (label in the accent, value in primary ink at a step up
    // from it — see its edit panel's "Font size / 100" rows) without every
    // other slider in the app silently changing tone. Defaults are the
    // colours this row has always drawn.
    property color labelColor: Theme.textSecondary
    property color valueColor: "#aeb6c8"
    // FreeShow's per-row ^ / v pair (its "step" arrows, drawn at the row's
    // extreme right): a positive `step` shows them, each click reporting ONE
    // step through `moved` — clamped to min/max and bracketed by dragStarted/
    // dragFinished, so a click lands as one undo entry, not one per repaint.
    // 0 (the default) draws nothing, which leaves every existing caller
    // looking exactly as it did.
    property real step: 0
    readonly property bool hasSteppers: root.step > 0 && root.enabled

    function nudge(direction) {
        const raw = root.value + direction * root.step
        const clamped = Math.max(root.minValue, Math.min(root.maxValue, raw))
        // Round off the float drift a fractional step accumulates
        // (0.30000000000000004), so the row's own readout stays clean.
        const snapped = root.step < 1 ? Math.round(clamped * 100) / 100 : clamped
        if (Math.abs(snapped - root.value) < 1e-9)
            return
        root.dragStarted()
        root.moved(snapped)
        root.dragFinished()
    }

    signal moved(real value)
    // Brackets one whole drag (or a single track click) as a single
    // gesture — a consumer wiring this into an undo/history system snapshots
    // on dragStarted, not on every moved(), so a slider drag collapses into
    // one undo step instead of one per pixel of thumb travel. Any control
    // driving continuous `moved` calls from a press-drag-release gesture
    // should offer this same pair, not just this component.
    signal dragStarted()
    signal dragFinished()

    implicitHeight: 34

    readonly property real pct: root.maxValue > root.minValue
        ? Math.max(0, Math.min(1, (root.value - root.minValue) / (root.maxValue - root.minValue)))
        : 0

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: root.labelColor
        font.family: Theme.fontFamily
        font.pixelSize: 13
        font.weight: Font.Medium
    }
    Text {
        anchors.right: root.hasSteppers ? stepperCol.left : parent.right
        anchors.rightMargin: root.hasSteppers ? 8 : 0
        anchors.top: parent.top
        text: (root.decimals > 0 ? root.value.toFixed(root.decimals) : Math.round(root.value)) + root.suffix
        color: root.valueColor
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Font.Medium
    }

    // The stepper pair — two hit-targets stacked at the row's right edge, so a
    // precise value is reachable without dragging the track (FreeShow's
    // filters rows: a 360deg hue-rotate is not a drag-only control).
    Column {
        id: stepperCol
        visible: root.hasSteppers
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: -4
        spacing: 1

        Repeater {
            model: [1, -1]   // up, then down
            delegate: Rectangle {
                id: stepBtn
                required property int modelData
                readonly property int direction: stepBtn.modelData
                // Dimmed while the whole row is disabled — the two arrows are
                // a row control, not a separate one.
                opacity: root.enabled ? 1 : 0.4
                width: 16
                height: 11
                radius: 3
                color: stepArea.containsMouse ? Theme.hoverBg : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                IconGlyph {
                    anchors.centerIn: parent
                    name: stepBtn.direction > 0 ? "chevronUp" : "chevronDown"
                    color: stepArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                    width: 9
                    height: 9
                }

                MouseArea {
                    id: stepArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: root.enabled
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.nudge(stepBtn.direction)
                }
            }
        }
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 14

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.borderSubtle

            Rectangle {
                width: parent.width * root.pct
                height: parent.height
                radius: 2
                color: Theme.accent
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -5
                onClicked: (mouse) => {
                    const p = Math.max(0, Math.min(1, mouse.x / track.width))
                    // A track click is its own single, atomic gesture — no
                    // separate press/release to bracket, so both signals
                    // fire back-to-back around the one resulting move.
                    root.dragStarted()
                    // Report only — the consumer assigns `value`; see the
                    // controlled-component note in the header comment.
                    root.moved(root.minValue + p * (root.maxValue - root.minValue))
                    root.dragFinished()
                }
            }
        }

        Rectangle {
            id: thumb
            width: 14
            height: 14
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.controlInk
            border.color: Theme.accent
            border.width: 2

            Binding {
                target: thumb
                property: "x"
                value: (track.width - thumb.width) * root.pct
                when: !thumbArea.drag.active
            }
            onXChanged: {
                if (thumbArea.drag.active) {
                    const range = track.width - thumb.width
                    const p = range > 0 ? thumb.x / range : 0
                    // Report only — the consumer assigns `value`; see the
                    // controlled-component note in the header comment.
                    root.moved(root.minValue + p * (root.maxValue - root.minValue))
                }
            }

            MouseArea {
                id: thumbArea
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                // Same as LevelDial: the slider lives in scrollable dialogs
                // — the Flickable must not steal a thumb drag mid-gesture.
                preventStealing: true
                onPressed: root.dragStarted()
                onReleased: root.dragFinished()
                drag.target: thumb
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: track.width - thumb.width
            }
        }
    }
}
