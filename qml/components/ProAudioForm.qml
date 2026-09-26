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
    // ---- Volume (0..100, a LINEAR gain fader ×0..×1) ----
    // Scales the channel meters (post-fader display — see the meter rows)
    // and reads as dB gain (20·log10). Stored on the model; the engine
    // applies it as real DSP once gain lands there.
    property real volume: 0
    // ---- Muted (meters flatten; caller syncs with the signal pane) ----
    property bool muted: false
    // ---- Channels block ----
    property int channels: 2
    // Live per-channel meter fill (0..1 each, index = channel row) — REAL
    // telemetry from the engine's WASAPI capture tap (EngineBridge.inputLevels
    // → the dialogs). An empty array, or a strip the tap no longer feeds,
    // renders dark: the meters read "no signal", they never fake it.
    property var meterLevels: []
    // The tap's live channel LAYOUT — "mono" | "stereo" | "multi" (engine
    // truth from the capture format). While metering, this WINS over the
    // stored `channels` count: the rows re-render to match what the device
    // actually delivers (a mono mic shows one strip even if the roster said
    // 2; a stereo line-in shows two). Empty string = not metering.
    property string meterLayout: ""
    // How many channel rows to render right now: the live tap's layout when
    // metering, the stored count otherwise.
    readonly property int renderedChannels: meterLayout === "mono" ? 1
        : meterLayout === "stereo" ? 2
        : meterLayout === "multi" && meterLevels.length > 0 ? meterLevels.length
        : channels
    // Back-compat passthrough for the single-level case.
    property real meterLevel: meterLevels.length > 0 ? meterLevels[0] : 0
    // (No raw field aliases — the id names below are the targets; all
    // consumer access goes through the typed passthroughs.)

    signal modeEdited(int mode)
    signal delayEdited(int delayMs)
    signal volumeEdited(real volume)
    signal channelsEdited(int channels)
    signal routingClicked()
    signal channelToggled(int index, bool enabled)
    // The channel row's gain knob moved (0..1); index = channel row.
    // The dialogs write it through AudioInputListModel.setChannelGain.
    signal channelGainEdited(int index, real gain)
    // Mute toggle request — the form has no mute control of its own (the
    // signal pane that held it was removed), but consumers like the Bus
    // dialog reuse the strip and wire their own mute through this.
    signal mutedToggled()
    // Seed a row's knob from the model (call via the gainKnobFor callback
    // below — a function keeps the binding lazy, no array copy per row).
    property var gainFor: function(index) { return 1.0 }

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
    // A shrinking channel count (a different device picked, or the live tap
    // reporting a narrower layout than the roster) must not leave stale
    // exceptions behind for channels that no longer exist.
    onChannelsChanged: root.trimChannelExceptions()
    onRenderedChannelsChanged: root.trimChannelExceptions()
    function trimChannelExceptions() {
        if (root.channelOn.length > root.renderedChannels)
            root.channelOn = root.channelOn.slice(0, root.renderedChannels)
    }
    function channelEnabled(i) { return root.channelOn[i] !== false }
    function setChannelEnabled(i, on) {
        if (root.channelEnabled(i) === on) return
        const next = root.channelOn.slice()
        while (next.length < root.renderedChannels) next.push(true)
        next[i] = on
        root.channelOn = next
        root.channelToggled(i, on)
    }
    function setAllChannels(on) {
        let changed = false
        for (let i = 0; i < root.renderedChannels; i++) {
            if (root.channelEnabled(i) !== on) changed = true
        }
        if (!changed) return
        const next = []
        for (let i = 0; i < root.renderedChannels; i++) next.push(on)
        root.channelOn = next
        for (let i = 0; i < root.renderedChannels; i++) root.channelToggled(i, on)
    }
    readonly property int channelsOnCount: {
        let n = 0
        for (let i = 0; i < root.renderedChannels; i++) if (root.channelEnabled(i)) n++
        return n
    }

    // ---- dB readouts (linear convention, matching the meter math) -------
    // gainText: the fader's gain in dB — 100 → "0 dB" (unity), 50 → "−6 dB",
    // 0 → "−∞". levelText: a measured 0..1 level in dBFS — what a channel's
    // cell shows LIVE from the tap (a healthy song peaks around −6..−3).
    // (The old readout mapped the fader's POSITION over −60..0 dB, which
    // printed "0 dB" at max and a misleading −30 dB at mid — unrelated to
    // what the meters actually showed. Both readouts now use the same
    // 20·log10 math the post-fader display applies.)
    function gainText(v) {
        if (v <= 0) return "-\u221E dB"
        return Math.round(20 * Math.log10(v / 100)) + " dB"
    }
    function levelText(l) {
        if (l <= 0.0005) return "-\u221E dB"   // ≈ −66 dBFS floor → silence
        return Math.round(20 * Math.log10(l)) + " dB"
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
                text: root.gainText(root.volume)
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
                text: root.meterLayout === "mono" ? qsTr("Channels · Mono")
                    : root.meterLayout === "stereo" ? qsTr("Channels · Stereo")
                    : qsTr("Channels")
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
    // The row count follows the LIVE tap layout while metering (see
    // renderedChannels) — the user sees the device's real mono/stereo/N shape
    // the moment the meter starts, not the roster's stored guess.
    Column {
        width: parent.width
        spacing: 5

        Repeater {
            model: root.renderedChannels

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
                Component.onCompleted: knob.gain = root.gainFor(chRow.index)

                // POST-FADER METER — the real WASAPI tap (channel i's own
                // entry of meterLevels) through the full chain: master fader
                // → this channel's gain knob, gated by the row's enable
                // checkbox and the input's mute. The SIGNAL is engine truth;
                // the gain math is display-side — the exact scaling the
                // engine's DSP will apply once gain lands there.
                // A cut/muted row reads "no signal" (nothing passes it).
                readonly property bool passes: chRow.chOn && !root.muted
                readonly property real rawLevel: {
                    const v = root.meterLevels.length > chRow.index
                              ? root.meterLevels[chRow.index] : 0
                    return Math.max(0, Math.min(1, v))
                }
                readonly property real level: chRow.passes
                    ? Math.min(1, chRow.rawLevel * (root.volume / 100) * knob.gain)
                    : 0

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
                            // A dot is LIT when the channel's level has
                            // reached it. CALIBRATION: the comparison runs in
                            // dB — a linear threshold pins healthy signals at
                            // the top (a −6 dBFS song lights ~94% of a linear
                            // strip). levelDb maps 0..1 → −60..0 dBFS; dots
                            // light below (levelDb + 60·pos... via the dB
                            // distance), so a −12 dBFS signal reads ~80%, and
                            // the red band only lights above −12 dBFS — head-
                            // room like a real VU. (This is the "−12 dB
                            // display alignment" calibration knob.)
                            readonly property real levelDb: chRow.level > 0.0005
                                ? Math.max(-60, 20 * Math.log10(chRow.level)) : -60
                            readonly property bool lit: levelDb > -60 + dot.pos * 60 + 0.5
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

                // dB cell — the channel's LIVE post-fader level in dBFS
                // (was: a static echo of the fader's gain, which printed
                // "0 dB" no matter what the meter did).
                Text {
                    id: dbCell
                    anchors.right: knob.left
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.levelText(chRow.level)
                    color: "#8a94a6"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // The reference's green gain knob — a mini ring with an
                // The channel's own GAIN KNOB — the green ring, now REAL:
                // it is this channel's fader (0..1, default unity), dragged
                // vertically, double-clicked to reset. The meter shows the
                // full post-fader chain: WASAPI level → master fader → this
                // knob → the row's dots and dB cell.
                Item {
                    id: knob
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24

                    // This channel's gain (0..1): read from the model if the
                    // caller wired channelGainEdited, else a local default
                    // (the component stays usable without a backing model).
                    property real gain: 1.0
                    // Drag math: full knob travel = ±135° over 0..1, ~0.4 dB
                    // per pixel (270° over ~120px of travel) — slow enough
                    // for fine trims, fast enough to sweep 0..1 quickly.
                    readonly property real angle: -135 + gain * 270
                    readonly property bool atUnity: Math.abs(gain - 1.0) < 0.005

                    function setGain(g) {
                        gain = Math.max(0, Math.min(1, g))
                        root.channelGainEdited(chRow.index, gain)
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: "#2c3040"
                        border.width: 2
                        border.color: knob.atUnity ? "#4ade80" : "#eab308"
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                    }
                    // The classic needle, rotating −135°..+135° with gain.
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

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        property real lastY: 0
                        onPressed: (m) => lastY = m.y
                        onPositionChanged: (m) => {
                            // Up = louder (pull the fader), ~0.4 dB/px.
                            knob.setGain(knob.gain + (lastY - m.y) / 120)
                            lastY = m.y
                        }
                        onDoubleClicked: knob.setGain(1.0)   // double-click → unity
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
            onClicked: root.setAllChannels(root.channelsOnCount < root.renderedChannels)
        }
    }
}
