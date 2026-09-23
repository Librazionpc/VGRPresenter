import QtQuick
import VGRPresenterUI
import "."

// The pro-audio form block in the audio dialogs — a faithful rebuild of
// the reference mock (VGRPresenter-style audio input editor):
//
//   Name    [ Input Name            ]
//   Source  [ ~~~ Microphone (...) ▼]
//   Mode    [Off][On][Auto Off][Auto On]
//           Off until manually enabled
//   ─────────────────────────────────────
//   Delay                          [0ms ▲▼]
//   Volume        ────────●──────── [0dB]
//   Channels (speaker)        [Routing…]
//   [✓] Channel 1  ▮▮▮▮▯▯  -36  (◯ 0)
//   [✓] Channel 2  ▮▮▮▮▯▯  -36  (◯ 0)
//   Select All / None
//
// Layout contract: EVERY row is the same two-column grid — a 76px label
// column on the left, the control filling the rest — matching the
// reference's aligned label rail. Controlled component: values are owned
// by the caller; changes report through the *Edited signals only.
//
// Literal colors, not Theme.* — matches this file family's convention
// (see EditScreen.qml/DropdownPanel.qml headers for the AOT depth-
// resolution rationale).
Column {
    id: root

    // ---- Mode (0 Off · 1 On · 2 Auto Off · 3 Auto On) ----
    property int mode: 0
    // ---- Delay (latency compensation, ms) ----
    property int delayMs: 0
    // ---- Volume (0..100 internal, shown as dBFS-equivalent) ----
    property real volume: 0
    // ---- Muted (meters flatten; caller syncs with the signal pane) ----
    property bool muted: false
    // ---- Channels block ----
    property int channels: 2
    // Live meter fill per channel row (0..1) — the fake-signal hook; the
    // dialogs drive it from the volume slider until real telemetry lands.
    property real meterLevel: 0
    // (No raw field aliases — the id names below are the targets; all
    // consumer access goes through the typed passthroughs.)

    signal modeEdited(int mode)
    signal delayEdited(int delayMs)
    signal volumeEdited(real volume)
    signal channelsEdited(int channels)
    signal routingClicked()
    signal channelToggled(int index, bool enabled)

    spacing: 0

    readonly property var modeLabels: [qsTr("Off"), qsTr("On"), qsTr("Auto Off"), qsTr("Auto On")]
    readonly property string modeHint: [
        qsTr("Off until manually enabled"),
        qsTr("Always passing signal"),
        qsTr("Enabled automatically when signal is detected"),
        qsTr("Enabled until signal ends")
    ][root.mode] || ""   // out-of-range mode reads blank, not "undefined"

    // Channel enable-state — an exceptions array over a default-on base,
    // copy-on-written so re-assignment re-fires the row bindings.
    property var channelOn: []
    // A shrinking channel count (a different device picked) must not leave
    // stale exceptions behind for channels that no longer exist.
    onChannelsChanged: {
        if (root.channelOn.length > root.channels)
            root.channelOn = root.channelOn.slice(0, root.channels)
    }
    function channelEnabled(i) { return root.channelOn[i] !== false }
    function setChannelEnabled(i, on) {
        if (root.channelEnabled(i) === on) return
        const next = root.channelOn.slice()
        while (next.length < root.channels) next.push(true)
        next[i] = on
        root.channelOn = next
        root.channelToggled(i, on)
    }
    function setAllChannels(on) {
        let changed = false
        for (let i = 0; i < root.channels; i++) {
            if (root.channelEnabled(i) !== on) changed = true
        }
        if (!changed) return
        const next = []
        for (let i = 0; i < root.channels; i++) next.push(on)
        root.channelOn = next
        for (let i = 0; i < root.channels; i++) root.channelToggled(i, on)
    }
    readonly property int channelsOnCount: {
        let n = 0
        for (let i = 0; i < root.channels; i++) if (root.channelEnabled(i)) n++
        return n
    }

    // dB readout — linear 0..100 mapped over a professional −60..0 range
    // (0 = −∞ per convention; the dial's 0 *is* silence).
    function dBText(v) {
        if (v <= 0) return "-\u221E dB"
        return (Math.max(-60, Math.round(-60 + (v / 100) * 60)) + " dB")
    }

    // The shared label rail — every row starts with this.
    component FormLabel: Text {
        color: "#8a94a6"
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    }

    // ================================================================
    // NAME + SOURCE rows — the consumer drops its SettingsField and
    // SelectField into the right column through the aliases, so both
    // dialogs' fields sit on the same label rail without re-declaring
    // them here.
    // ================================================================
    property string nameLabel: qsTr("Name")
    property string sourceLabel: qsTr("Source")
    property alias nameText: nameField.text
    signal nameEdited(string text)
    property string sourceValue: ""
    property var sourceOptions: []
    signal sourcePicked(string value)

    // (Declared as a Column child set below — see NameRow / SourceRow.)

    // ---- NAME row ----------------------------------------------------
    Item {
        width: parent.width
        height: 40

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: root.nameLabel
        }

        SettingsField {
            id: nameField
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 76
            label: ""
            showLabel: false
            placeholder: qsTr("Input Name")
            onTextEdited: (t) => root.nameEdited(t)
        }
    }

    // ---- SOURCE row --------------------------------------------------
    Item {
        width: parent.width
        height: 40

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: root.sourceLabel
        }

        SelectField {
            id: sourceField
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 76
            label: ""
            value: root.sourceValue
            options: root.sourceOptions
            onValuePicked: (v) => root.sourcePicked(v)
        }
    }

    // ---- MODE row ------------------------------------------------------
    Item {
        width: parent.width
        height: 56

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Mode")
        }

        Column {
            anchors.left: parent.left
            anchors.leftMargin: 76
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Row {
                spacing: 0

                Repeater {
                    model: root.modeLabels

                    delegate: Rectangle {
                        id: seg
                        required property int index
                        required property string modelData
                        width: 82
                        height: 30
                        color: root.mode === seg.index ? "#3574f0" : "#262933"
                        border.width: 1
                        border.color: root.mode === seg.index ? "#3574f0" : "#3a3d48"
                        Behavior on color { ColorAnimation { duration: 110 } }

                        // One pill: rounded only at the group's ends.
                        topLeftRadius: seg.index === 0 ? Theme.radiusMd : 0
                        bottomLeftRadius: seg.index === 0 ? Theme.radiusMd : 0
                        topRightRadius: seg.index === root.modeLabels.length - 1 ? Theme.radiusMd : 0
                        bottomRightRadius: seg.index === root.modeLabels.length - 1 ? Theme.radiusMd : 0

                        Text {
                            anchors.centerIn: parent
                            text: seg.modelData
                            color: root.mode === seg.index ? "#ffffff" : "#aab2c4"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.modeEdited(seg.index)
                        }
                    }
                }
            }

            Text {
                text: root.modeHint
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
        }
    }

    // ---- Divider (the reference's section break before Delay) ---------
    Rectangle {
        width: parent.width
        height: 1
        color: "#23262f"
    }

    Item { width: 1; height: 12 }

    // ---- DELAY row ------------------------------------------------------
    Item {
        width: parent.width
        height: 36

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Delay")
        }

        // Stepper box: (value ms) [▼][▲] — click, or hold to repeat.
        Rectangle {
            id: delayBox
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 84
            height: 28
            radius: Theme.radiusSm
            color: "#262933"
            border.width: 1
            border.color: Theme.border

            function nudge(delta) {
                const next = Math.max(0, Math.min(1000, root.delayMs + delta))
                if (next !== root.delayMs) root.delayEdited(next)
            }
            property int direction: 0

            Timer {
                id: delayRepeat
                interval: 70; repeat: true
                onTriggered: delayBox.nudge(delayBox.direction)
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 46
                text: root.delayMs + " ms"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }

            Text {
                anchors.centerIn: downArea
                text: "\u25BC"; color: Theme.textMuted; font.pixelSize: 8
            }
            MouseArea {
                id: downArea
                width: 20; height: parent.height
                anchors.right: upArea.left
                cursorShape: Qt.PointingHandCursor
                onPressed: () => { delayBox.direction = -10; delayBox.nudge(-10); delayRepeat.restart() }
                onReleased: delayRepeat.stop()
                onCanceled: delayRepeat.stop()
            }
            Text {
                anchors.centerIn: upArea
                text: "\u25B2"; color: Theme.textMuted; font.pixelSize: 8
            }
            MouseArea {
                id: upArea
                width: 20; height: parent.height
                anchors.right: parent.right
                cursorShape: Qt.PointingHandCursor
                onPressed: () => { delayBox.direction = 10; delayBox.nudge(10); delayRepeat.restart() }
                onReleased: delayRepeat.stop()
                onCanceled: delayRepeat.stop()
            }
        }
    }

    // ---- VOLUME row -------------------------------------------------------
    Item {
        width: parent.width
        height: 36

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Volume")
        }

        // Thin track with a round thumb — the reference's blue slider.
        Item {
            id: track
            anchors.left: parent.left
            anchors.leftMargin: 76
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 76 - 64 - 12
            height: 28

            readonly property real pct: Math.max(0, Math.min(1, root.volume / 100))

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: "#2a2d38"
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(4, track.pct * track.width)
                height: 4
                radius: 2
                color: "#3574f0"
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: Math.max(0, Math.min(track.width - 14, track.pct * (track.width - 14)))
                width: 14; height: 14; radius: 7
                color: "#c9d1e0"
                border.width: 1
                border.color: "#3574f0"
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -8
                preventStealing: true
                cursorShape: Qt.PointingHandCursor
                function apply(m) {
                    const v = Math.max(0, Math.min(1, (m.x - 7) / (track.width - 14))) * 100
                    root.volumeEdited(Math.round(v))
                }
                onPositionChanged: (m) => apply(m)
                onPressed: (m) => apply(m)
            }
        }

        // dB readout box.
        Rectangle {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 56
            height: 28
            radius: Theme.radiusSm
            color: "#262933"
            border.width: 1
            border.color: Theme.border

            Text {
                anchors.centerIn: parent
                text: root.dBText(root.volume)
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
        }
    }

    Item { width: 1; height: 10 }

    // ---- CHANNELS header --------------------------------------------------
    Item {
        width: parent.width
        height: 30

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Channels")
                color: "#c3cad8"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                name: "volume2"
                color: "#8a94a6"
                implicitWidth: 15
                implicitHeight: 15
            }
        }

        // Routing… — reports via routingClicked(); the dialogs commit the
        // patch on Apply. (No channel-count stepper: the count defaults
        // from the source — mono device / stereo media — and is not
        // user-adjustable here per design review.)
        Rectangle {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 78
            height: 26
            radius: Theme.radiusSm
            color: "#3a3f55"
            border.width: 1
            border.color: "#4a4f66"

            Text {
                anchors.centerIn: parent
                text: qsTr("Routing…")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.routingClicked()
            }
        }
    }

    // ---- Channel rows -----------------------------------------------------
    Column {
        width: parent.width
        spacing: 5

        Repeater {
            model: root.channels

            delegate: Rectangle {
                id: chRow
                required property int index
                width: parent.width
                height: 34
                radius: Theme.radiusSm
                color: "#1d2029"
                border.width: 1
                border.color: "#2c3040"

                readonly property bool chOn: root.channelEnabled(chRow.index)
                // Per-channel phase offset — rows never pulse in unison.
                readonly property real phase: chRow.index * 0.37

                // Meter wobble phase — ONE timer per channel row shared by all 24
                // dots (was a Timer per dot: 24 x channels of them).
                property real cycle: 0
                readonly property bool signalOn: chRow.chOn && root.meterLevel > 0 && !root.muted
                Timer {
                    interval: 140
                    running: chRow.signalOn
                    repeat: true
                    onTriggered: chRow.cycle = (chRow.cycle + 0.11) % 2
                }

                // Checkbox — the reference's plain square check.
                Rectangle {
                    id: box
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15; height: 15
                    radius: 3
                    color: chRow.chOn ? "#3574f0" : "transparent"
                    border.width: 1
                    border.color: chRow.chOn ? "#3574f0" : "#4a4f66"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        visible: chRow.chOn
                        text: "\u2713"
                        color: "#ffffff"
                        font.pixelSize: 13
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -7
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setChannelEnabled(chRow.index, !chRow.chOn)
                    }
                }

                Text {
                    anchors.left: box.right
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Channel %1").arg(chRow.index + 1)
                    color: chRow.chOn ? "#e2e8f0" : "#5c6475"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // Mini VU — segmented dots (green → amber → red), lit only
                // while the channel is enabled and the caller reports signal.
                // The strip stretches horizontally across the free width
                // between the channel label and the dB cell — one long scale,
                // not a short fixed chip row.
                Row {
                    id: meterRow
                    anchors.left: parent.left
                    anchors.leftMargin: 150
                    anchors.right: dbCell.left
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    clip: true
                    visible: width > 24   // a squeezed dialog drops the strip instead of overflowing it

                    readonly property int segs: 24
                    // Segments widen to share the strip, so the meter fills
                    // the row at any dialog width.
                    readonly property real segW: Math.max(4, (width - (segs - 1) * spacing) / segs)

                    Repeater {
                        model: meterRow.segs

                        delegate: Rectangle {
                            id: dot
                            required property int index
                            // Position across the meter (0..1) — the level
                            // threshold this dot represents.
                            readonly property real pos: dot.index / (meterRow.segs - 1)
                            width: meterRow.segW; height: 12; radius: 2
                            // A dot is LIT when the level has reached it —
                            // with a small wobble on the boundary so the
                            // leading edge dances like a real VU needle.
                            readonly property real env: !chRow.signalOn ? 0
                                : Math.max(0, Math.min(1,
                                    root.meterLevel - dot.pos
                                    + Math.sin(chRow.cycle + dot.index * 0.8) * 0.06))
                            readonly property bool lit: dot.env > 0.02
                            // The green→yellow→red ramp is ALWAYS visible
                            // (dimmed) — the meter reads as a scale even at
                            // rest, exactly like the reference; signal
                            // brightens the portion the level has reached.
                            color: dot.pos < 0.625 ? "#4ade80"
                                 : dot.pos < 0.8125 ? "#f5c26b"
                                 : "#ff4d3d"
                            opacity: dot.lit ? 1 : 0.22
                            Behavior on opacity { NumberAnimation { duration: 90 } }
                        }
                    }
                }

                // dB cell.
                Text {
                    id: dbCell
                    anchors.right: knob.left
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.dBText(root.volume)
                    color: "#8a94a6"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // The reference's green gain knob — a mini ring with an
                // indicator line that rotates across −135°..+135° with the
                // volume. (Same 0..100 mapping as the master slider; the
                // per-channel value is the shared level until per-channel
                // gains exist in the model.)
                Item {
                    id: knob
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24

                    readonly property real angle: -135 + (root.volume / 100) * 270

                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: "#2c3040"
                        border.width: 2
                        border.color: "#4ade80"
                    }
                    // Indicator line: a taller item pivoting about its own
                    // top-center at the knob's middle — the classic knob
                    // needle (line from center outward, rotating).
                    Item {
                        anchors.centerIn: parent
                        width: 2.5; height: 12
                        rotation: knob.angle

                        Rectangle {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 2.5
                            height: 8
                            radius: 1
                            color: "#e2e8f0"
                        }
                    }
                }
            }
        }
    }

    Item { width: 1; height: 6 }

    // ---- Select All / None -------------------------------------------------
    Text {
        text: qsTr("Select All / None")
        color: "#8a94a6"
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs

        MouseArea {
            anchors.fill: parent
            anchors.margins: -4
            cursorShape: Qt.PointingHandCursor
            // Any channel off → all on; otherwise all off.
            onClicked: root.setAllChannels(root.channelsOnCount < root.channels)
        }
    }
}
