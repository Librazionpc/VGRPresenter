import QtQuick
import VGRPresenterUI
import "."

// The pro-audio form block in the audio dialogs — a faithful rebuild of
// the reference mock (VGRPresenter-style audio input editor):
//
//   Name    [ Input Name            ]
//   Source  [ ~~~ Microphone (...) ▼]
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
// Every colour here reads a Theme token. This file was the last surface still
// carrying its own dark hex literals, which had two costs: with Light chosen
// the dialog card went white while every strip inside it stayed dark, and each
// state colour became a guess — the amber gain ring that "came out of nowhere"
// was one of those guesses. The meter scale this form shares with LevelTrack
// and OutputMonitorTile lives in Theme too (meterQuiet…meterClip), so the three
// can no longer drift apart.
Column {
    id: root

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

    signal delayEdited(int delayMs)
    signal volumeEdited(real volume)
    signal channelsEdited(int channels)
    signal routingClicked()
    // A channel switched on/off, carrying the trim the consumer must WRITE for
    // it: 0 when it is switched off (that IS "off" — the model has no separate
    // enabled bit, and gain 0 is exactly what the Media pane's L/R pill
    // writes), and the row's remembered trim when it is switched back on. That
    // is what makes "turn the left channel off" travel downstream — the board's
    // bus mix, the Media pane's cards and pills, the monitor tile's meters and
    // the engine's own graph trim all read that one value — instead of only
    // greying this dialog's row out.
    signal channelToggled(int index, bool enabled, real gain)
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
    // The trim this channel should COME BACK AT when it is switched back on —
    // asked of the model (X.channelTrim(row, i)), because that is where the trim
    // now lives (models/ChannelTrim.h). It used to be a `channelTrim` array
    // owned by this form, which meant the restore worked only for the life of
    // one open dialog and only for a side closed from inside THIS dialog: a side
    // closed from the Media pane's L/R pill came back at unity here, and a side
    // closed here came back at unity there. The default (nothing remembered)
    // falls back to the level the side already holds, else unity — the same
    // chain the models apply, so an unwired host cannot reopen a side at 0.
    property var channelTrimFor: function(index) { return 0.0 }
    // Seed a row's ON state from the model too. The checkbox is an exception
    // array over a default-on base (channelOn), so a channel the user switched
    // off ANYWHERE — the Media pane's L/R pill, a previous session, this dialog
    // last time it was open — must read as off here: the callback is the
    // model's own answer (gain 0 ≡ off, the convention the pills and every
    // downstream meter already read), and the local array only overrides it
    // while this dialog's own click is in flight.
    property var channelOnFor: function(index) { return true }

    spacing: 0

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
    function channelEnabled(i) {
        return root.channelOn[i] !== undefined ? root.channelOn[i] !== false
                                               : root.channelOnFor(i)
    }
    // What a channel switched back ON comes back at. Switching one off writes
    // gain 0 to the model, so the knob's own value is gone by the time someone
    // switches it back on — this asks the model for the trim it remembers (see
    // channelTrimFor above), and only falls back to the side's current level and
    // then unity. Never the 0 that "off" is stored as. Exactly
    // channeltrim::reopenGain (models/ChannelTrim.h) — one policy, two
    // languages.
    function trimFor(i) {
        const remembered = Math.max(0, Math.min(1, root.channelTrimFor(i)))
        if (!Db.isChannelOff(remembered))
            return remembered
        const current = Math.max(0, Math.min(1, root.gainFor(i)))
        return Db.isChannelOff(current) ? 1.0 : current
    }
    function setChannelEnabled(i, on) {
        if (root.channelEnabled(i) === on) return
        const gain = on ? root.trimFor(i) : 0.0
        const next = root.channelOn.slice()
        while (next.length < root.renderedChannels) next.push(true)
        next[i] = on
        root.channelOn = next
        root.channelToggled(i, on, gain)
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
        for (let i = 0; i < root.renderedChannels; i++)
            root.channelToggled(i, on, on ? root.trimFor(i) : 0.0)
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
        if (l <= Db.floorLinear) return "-\u221E dB"   // ≈ −66 dBFS floor → silence
        return Math.round(20 * Math.log10(l)) + " dB"
    }

    // The shared label rail — every row starts with this.
    component FormLabel: Text {
        color: Theme.textSecondary
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
    // The Name box's hint text — a consumer whose Add dialog wants a
    // friendlier example ("e.g. “Cam 3 · Aisle”") passes one; the edit
    // dialogs keep the plain "Input Name".
    property string namePlaceholder: qsTr("Input Name")
    property string sourceValue: ""
    property var sourceOptions: []
    // Hint text for the Source box while nothing is picked.
    property string sourcePlaceholder
    // SelectField's test hook (see its comment): setting this exposes the
    // field's box as <name>_box and its popup as <name>_menu, so the modal-fit
    // sweep can click the field and measure the menu that opened. Empty = no
    // handle (the edit dialogs do not need one).
    property string sourceFieldObjectName: ""
    signal sourcePicked(string value)

    // Reuse gates — a consumer that only wants the Channels section (the
    // video dialogs' Media/NDI audio meter, which already has its own
    // Name/Kind/Source fields elsewhere in that dialog and no latency
    // stepper) hides the rows it doesn't need instead of duplicating the
    // whole channel-row block a third time.
    property bool showNameSource: true
    property bool showDelay: true
    // Volume and Channels are separable too, and that is what lets the VIDEO
    // dialogs use this same form instead of their own field set: a camera or a
    // screen has no audio to trim, so those dialogs hide those two sections
    // (showNameSource stays on), while Media/NDI kinds show them. Every video
    // field then sits on the SAME label rail, with the same rows, as the audio
    // dialogs' — one design, not two (live report: "the app has two designs for
    // the audio and video edit popup — make it only one").
    property bool showVolume: true
    property bool showChannels: true
    // Are the channel rows something the consumer can WRITE? The audio input
    // dialogs say yes (a click writes gain 0 = closed to the roster — the one
    // "closed channel" convention this app has). A BUS cannot: the engine's
    // VolumeControl carries gain/mute/solo/pan and no per-channel trims, so a
    // bus's rows are a read-through of what feeds it (the Edit Bus dialog's
    // channelOnFor/gainFor) and setting channelsEditable:false keeps them
    // rendered EXACTLY like an input row's — same checkbox, same knob ring,
    // same dimming — with the controls inert instead of silently writing
    // nothing. Consistency of the rendering is the point; nothing pretends a
    // value was stored.
    property bool channelsEditable: true

    // (Declared as a Column child set below — see NameRow / SourceRow.)

    // ---- NAME row ----------------------------------------------------
    Item {
        width: parent.width
        height: 40
        visible: root.showNameSource

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
            placeholder: root.namePlaceholder
            onTextEdited: (t) => root.nameEdited(t)
        }
    }

    // ---- SOURCE row --------------------------------------------------
    Item {
        width: parent.width
        height: 40
        visible: root.showNameSource

        FormLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: root.sourceLabel
        }

        SelectField {
            id: sourceField
            objectName: root.sourceFieldObjectName
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 76
            label: ""
            value: root.sourceValue
            placeholder: root.sourcePlaceholder
            options: root.sourceOptions
            onValuePicked: (v) => root.sourcePicked(v)
        }
    }

    // ---- DELAY row ------------------------------------------------------
    Item {
        width: parent.width
        height: 36
        visible: root.showDelay

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
            color: Theme.chip
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
        visible: root.showVolume

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
                color: Theme.toggleOffTrack
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(4, track.pct * track.width)
                height: 4
                radius: 2
                color: Theme.accent
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: Math.max(0, Math.min(track.width - 14, track.pct * (track.width - 14)))
                width: 14; height: 14; radius: 7
                color: Theme.navLabelMuted
                border.width: 1
                border.color: Theme.accent
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
            color: Theme.chip
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

    Item { width: 1; height: 10; visible: root.showChannels }

    // ---- CHANNELS header --------------------------------------------------
    Item {
        width: parent.width
        height: 30
        visible: root.showChannels

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.meterLayout === "mono" ? qsTr("Channels · Mono")
                    : root.meterLayout === "stereo" ? qsTr("Channels · Stereo")
                    : qsTr("Channels")
                color: Theme.navLabelMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                name: "volume2"
                color: Theme.textSecondary
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
            color: Theme.controlBg
            border.width: 1
            border.color: Theme.controlBorder

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
        visible: root.showChannels

        Repeater {
            model: root.renderedChannels

            delegate: Rectangle {
                id: chRow
                required property int index
                width: parent.width
                // Taller than before (was 34): FreeShow's "detailed" AudioMeter
                // (src/frontend/components/drawer/audio/AudioMeter.svelte, the
                // exact one AudioChannelMixer.svelte gives each channel strip)
                // carries a dB scale under the bar, not just the bar itself.
                height: 52
                radius: Theme.radiusSm
                color: Theme.inset
                border.width: 1
                border.color: Theme.borderSubtle

                readonly property bool chOn: root.channelEnabled(chRow.index)
                Component.onCompleted: {
                    // The knob reads the model's gain. Nothing to remember here
                    // any more: the trim lives in the model, and setChannelGain
                    // records every OPEN level it is given as that side's trim.
                    knob.gain = Math.max(0, Math.min(1, root.gainFor(chRow.index)))
                }
                // The knob shows what the model now holds: a channel switched
                // off reads 0, one switched back on reads the trim it went off
                // with — so the row's bar, dB cell, knob and the model agreement
                // cannot drift apart across a toggle.
                onChOnChanged: knob.gain = chRow.chOn
                    ? root.trimFor(chRow.index) : 0.0

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

                // Fast attack, slow release (FreeShow's AudioMeter.svelte's
                // updateMeterChannel), and a peak-hold that sticks 2s before
                // easing down — the same behaviour OutputMonitorTile's meter uses.
                property real smoothed: 0
                property real peakValue: 0
                property real peakHeldAt: 0
                Timer {
                    interval: 33
                    running: chRow.visible
                    repeat: true
                    onTriggered: {
                        // A channel that is switched off must go dark at once;
                        // retaining its peak hold would make a muted side look
                        // like it was still carrying signal.
                        if (!chRow.passes || Db.isChannelOff(knob.gain)) {
                            chRow.smoothed = 0
                            chRow.peakValue = 0
                            chRow.peakHeldAt = Date.now()
                            return
                        }
                        const target = chRow.level
                        chRow.smoothed = target > chRow.smoothed
                            ? target : chRow.smoothed + (target - chRow.smoothed) * 0.2
                        const now = Date.now()
                        if (chRow.smoothed >= chRow.peakValue) {
                            chRow.peakValue = chRow.smoothed
                            chRow.peakHeldAt = now
                        } else if (now - chRow.peakHeldAt > 2000) {
                            chRow.peakValue = Math.max(chRow.smoothed, chRow.peakValue - 0.02)
                        }
                    }
                }

                // Checkbox — the reference's plain square check.
                Rectangle {
                    id: box
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 9
                    width: 15; height: 15
                    radius: 3
                    color: chRow.chOn ? Theme.accent : "transparent"
                    border.width: 1
                    border.color: chRow.chOn ? Theme.accent : Theme.controlBorder
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        visible: chRow.chOn
                        text: "\u2713"
                        color: Theme.controlInk
                        font.pixelSize: 13
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -7
                        enabled: root.channelsEditable
                        cursorShape: root.channelsEditable ? Qt.PointingHandCursor
                                                           : Qt.ArrowCursor
                        onClicked: root.setChannelEnabled(chRow.index, !chRow.chOn)
                    }
                }

                Text {
                    anchors.left: box.right
                    anchors.leftMargin: 14
                    anchors.verticalCenter: box.verticalCenter
                    text: qsTr("Channel %1").arg(chRow.index + 1)
                    color: chRow.chOn ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // Signal dot — on/off, not level-proportional (FreeShow's own convention).
                Rectangle {
                    id: signalDot
                    anchors.left: parent.left
                    anchors.leftMargin: 150
                    anchors.verticalCenter: box.verticalCenter
                    width: 5; height: 5; radius: 2.5
                    color: Theme.meterQuiet
                    opacity: chRow.level > 0.01 ? 1 : 0.2
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                }

                // The gradient every layer below shares — left (quiet) = cyan,
                // through green/amber, right (loud) = red.
                readonly property Gradient barGradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.meterQuiet }
                    GradientStop { position: 0.55; color: Theme.meterLoud }
                    GradientStop { position: 0.84; color: Theme.meterHot }
                    GradientStop { position: 1.0; color: Theme.meterClip }
                }

                // The dB scale the ticks/bars/needle all read from is the shared
                // -60..0 dB mapping (Db.dbPct) — was a private copy here, in
                // LevelTrack and OutputMonitorTile; three copies, three ways to
                // drift.

                // The bar itself — a CONTINUOUS gradient reveal (FreeShow's real design),
                // not discrete LED dots. Stretches across the free width between the
                // signal dot and the dB cell.
                Item {
                    id: meterBar
                    anchors.left: signalDot.right
                    anchors.leftMargin: 10
                    anchors.right: dbCell.left
                    anchors.rightMargin: 16
                    anchors.verticalCenter: box.verticalCenter
                    height: 10
                    visible: width > 24   // a squeezed dialog drops the bar instead of overflowing it

                    // Ghost — the full range, always faintly visible, so the scale reads at rest.
                    Rectangle {
                        anchors.fill: parent
                        opacity: 0.14
                        gradient: chRow.barGradient
                    }

                    // Lit portion — clipped to the smoothed level; the gradient rectangle
                    // inside is the FULL bar width so its colors line up with the ghost.
                    Item {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: parent.width * Db.dbPct(chRow.smoothed)
                        clip: true

                        Rectangle {
                            width: meterBar.width
                            height: parent.height
                            gradient: chRow.barGradient
                        }
                    }

                    // Peak-hold tick.
                    Rectangle {
                        visible: chRow.peakValue > 0.01
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        x: parent.width * Db.dbPct(chRow.peakValue) - 1
                        color: Theme.controlInk
                        opacity: 0.6
                    }
                }

                // dB scale — FreeShow's own ladder (-60..0, major ticks every 6dB), positioned
                // linearly in dB (same -60..0 → 0..1 mapping the bar's fill above uses) so the
                // ticks line up with what the bar actually shows, evenly spaced every 6dB.
                Item {
                    id: dbScale
                    anchors.left: meterBar.left
                    anchors.right: meterBar.right
                    anchors.top: meterBar.bottom
                    anchors.topMargin: 4
                    height: 12
                    visible: meterBar.visible

                    Repeater {
                        model: [-60, -54, -48, -42, -36, -30, -24, -18, -12, -6, 0]
                        delegate: Text {
                            required property int modelData
                            readonly property real pct: (modelData + 60) / 60
                            x: Math.min(dbScale.width - implicitWidth, dbScale.width * pct - implicitWidth / 2)
                            y: 0
                            text: modelData === 0 ? "0" : String(modelData)
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 8
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
                    anchors.verticalCenter: box.verticalCenter
                    text: root.levelText(chRow.level)
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }

                // The reference's green gain knob — a mini ring with an
                // The channel's own GAIN KNOB — the green ring, now REAL:
                // it is this channel's fader (0..1, default unity), dragged
                // vertically, double-clicked to reset. The meter shows the
                // full post-fader chain: WASAPI level → master fader → this
                // knob → the row's bar and dB cell.
                Item {
                    id: knob
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: box.verticalCenter
                    width: 24; height: 24

                    // This channel's gain (0..1): read from the model if the
                    // caller wired channelGainEdited, else a local default
                    // (the component stays usable without a backing model).
                    property real gain: 1.0
                    // Drag math: full knob travel = ±135° over 0..1, ~0.4 dB
                    // per pixel (270° over ~120px of travel) — slow enough
                    // for fine trims, fast enough to sweep 0..1 quickly.
                    readonly property real angle: -135 + gain * 270
                    // The ring (and the trim pill on it) reads the app's ONE
                    // channel-state vocabulary — Db.channelState decides, Theme
                    // .channelStateColor draws — the same decision the cards'
                    // L/R pills and the monitor strips now read, so off and
                    // trimmed mean one thing everywhere.
                    //
                    // It used to hardcode its own green/amber pair, which had
                    // two consequences: the amber turned up in every dialog that
                    // reuses this form (Edit Audio Input, Edit Bus, Edit Video
                    // Source) as an unexplained orange ring — "that orange pill,
                    // where is it coming from" — and the state it was trying to
                    // announce was invisible on every other surface.
                    readonly property string stateName: Db.channelState(gain)
                    readonly property color stateColor: Theme.channelStateColor(stateName)
                    // The needle's own sweep — LIVE level (same Db.dbPct/smoothed the bar
                    // uses), not gain. A glowing ring alone still read as "not moving";
                    // this is the actual speedometer-style needle motion that was missing.
                    readonly property real liveAngle: -135 + Db.dbPct(chRow.smoothed) * 270

                    function setGain(g) {
                        gain = Math.max(0, Math.min(1, g))
                        // The consumer writes the model, which remembers this as
                        // the side's trim (see setChannelGain) — so a knob moved
                        // here is restorable from the Media pane's pill too.
                        root.channelGainEdited(chRow.index, gain)
                    }

                    // Live glow ring — the knob itself is a manual gain trim (it only
                    // moves when dragged), but sitting right beside the live dB
                    // reading it reads as broken/frozen if nothing about it ever
                    // responds to signal. This ring brightens with the SAME
                    // smoothed/dbPct level driving the bar, so the knob visibly
                    // pulses with live audio without disturbing the drag gesture.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: 15
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.meterQuiet
                        opacity: Math.min(0.85, Db.dbPct(chRow.smoothed))
                        Behavior on opacity { NumberAnimation { duration: 80 } }
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: Theme.borderSubtle
                        border.width: 2
                        border.color: knob.stateColor
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                    }
                    // Gain-trim marker — a small fixed tick on the outer glow ring
                    // showing where the manual trim sits (the needle below now sweeps
                    // live level instead, so trim gets its own, separate indicator).
                    Item {
                        anchors.centerIn: parent
                        width: 2; height: 30
                        rotation: knob.angle

                        Rectangle {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 2; height: 4
                            radius: 1
                            color: knob.stateColor
                        }
                    }

                    // The needle — sweeps −135°..+135° with LIVE level, speedometer-style
                    // (fast attack / slow release, same smoothing driving the bar).
                    Item {
                        anchors.centerIn: parent
                        width: 2.5; height: 12
                        rotation: knob.liveAngle
                        Behavior on rotation { NumberAnimation { duration: 60; easing.type: Easing.OutQuad } }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 2.5
                            height: 8
                            radius: 1
                            color: Theme.textPrimary
                        }
                    }

                    // What the knob IS, in words: the ring's colour alone never
                    // said "trimmed" anywhere, and the state the operator cannot
                    // name is the state they cannot trust. Same vocabulary and
                    // same dB as the L/R pills' tips (Db.channelStateText), which
                    // is the point — one channel, one sentence, whichever control
                    // you happen to be over.
                    readonly property string tipText:
                        qsTr("Channel %1 gain · %2").arg(chRow.index + 1)
                                                     .arg(Db.channelStateText(knob.gain))

                    // Triggered by pointer position, not hover events (see
                    // PositionHoverArea): the hover-exit this build never delivers
                    // would leave the bubble up. Click sink off, so the knob's own
                    // drag below is untouched.
                    PositionHoverArea {
                        id: knobTipArea
                        anchors.fill: parent
                        showCursor: false
                        clickEnabled: false
                        onEntered: TipService.show(knob.tipText, knob)
                        onExited: TipService.hide(knob)
                    }
                    // The tip follows the trim while it is being dragged — the
                    // gesture is on this knob, so the pointer is on it too.
                    onGainChanged: if (knobTipArea.hovered) TipService.show(knob.tipText, knob)
                    Component.onDestruction: TipService.forget(knob)

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        enabled: root.channelsEditable
                        cursorShape: root.channelsEditable ? Qt.PointingHandCursor
                                                           : Qt.ArrowCursor
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

    Item { width: 1; height: 6; visible: root.showChannels }

    // ---- Select All / None -------------------------------------------------
    Text {
        text: qsTr("Select All / None")
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        visible: root.showChannels

        MouseArea {
            anchors.fill: parent
            anchors.margins: -4
            // A read-through strip (the bus) has nothing to select: leave the
            // label in place, take no clicks.
            enabled: root.channelsEditable
            cursorShape: root.channelsEditable ? Qt.PointingHandCursor : Qt.ArrowCursor
            // Any channel off → all on; otherwise all off.
            onClicked: root.setAllChannels(root.channelsOnCount < root.renderedChannels)
        }
    }
}
