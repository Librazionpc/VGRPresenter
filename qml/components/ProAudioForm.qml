import QtQuick
import VGRPresenterUI
import "."

Column {
    id: root

    // ---- Delay (latency compensation, ms) ----
    property int delayMs: 0
    // ---- Volume (0..100, a LINEAR gain fader ×0..×1) ----
    property real volume: 0
    // ---- Muted (meters flatten; caller syncs with the signal pane) ----
    property bool muted: false
    // ---- Channels block ----
    property int channels: 2
    property var meterLevels: []
    property string meterLayout: ""
    // How many channel rows to render right now: the live tap's layout when
    // metering, the stored count otherwise.
    readonly property int renderedChannels: meterLayout === "mono" ? 1
        : meterLayout === "stereo" ? 2
        : meterLayout === "multi" && meterLevels.length > 0 ? meterLevels.length
        : channels
    // (No raw field aliases — the id names below are the targets; all
    // consumer access goes through the typed passthroughs.)

    signal delayEdited(int delayMs)
    signal volumeEdited(real volume)
    signal channelsEdited(int channels)
    signal routingClicked()
    signal channelToggled(int index, bool enabled, real gain)
    // The channel row's gain knob moved (0..1); index = channel row.
    // The dialogs write it through AudioInputListModel.setChannelGain.
    signal channelGainEdited(int index, real gain)
    signal mutedToggled()
    // Seed a row's knob from the model (call via the gainKnobFor callback
    // below — a function keeps the binding lazy, no array copy per row).
    property var gainFor: function(index) { return 1.0 }
    property var channelTrimFor: function(index) { return 0.0 }
    property var channelOnFor: function(index) { return true }

    spacing: 0

    // Channel enable-state — an exceptions array over a default-on base,
    // copy-on-written so re-assignment re-fires the row bindings.
    property var channelOn: []
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
    // ================================================================
    property string nameLabel: qsTr("Name")
    property string sourceLabel: qsTr("Source")
    property alias nameText: nameField.text
    signal nameEdited(string text)
    property string namePlaceholder: qsTr("Input Name")
    property string sourceValue: ""
    property var sourceOptions: []
    // Hint text for the Source box while nothing is picked.
    property string sourcePlaceholder
    property string sourceFieldObjectName: ""
    signal sourcePicked(string value)

    property bool showNameSource: true
    property bool showDelay: true
    property bool showVolume: true
    property bool showChannels: true
    property bool channelsEditable: true
    // The per-channel check boxes and "Select All / None": hidden for a strip that only READS its
    // channels (the Edit Bus dialog) — switches that cannot be flipped are just noise there.
    property bool showChannelSwitches: true

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
            width: parent.width - 76 - 72 - 12
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

        // dB readout box — EDITABLE: type a level ("-6", "-6 dB", "-inf") and press Enter. The fader is a
        // linear gain, so a typed dB maps to 10^(dB/20) (0 dB is the top; anything above clamps to it).
        Rectangle {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 64
            height: 28
            radius: Theme.radiusSm
            color: Theme.chip
            border.width: 1
            border.color: dbInput.activeFocus ? Theme.accent : Theme.border

            TextInput {
                id: dbInput
                anchors.centerIn: parent
                width: parent.width - 10
                horizontalAlignment: TextInput.AlignHCenter
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                selectByMouse: true
                clip: true
                inputMethodHints: Qt.ImhNoPredictiveText
                text: root.gainText(root.volume)

                function refresh() { dbInput.text = root.gainText(root.volume) }
                // The typed text -> a fader value 0..100, or -1 when it is not a level.
                function parseVolume(s) {
                    const t = String(s).toLowerCase().replace(/db/g, "").replace(",", ".").trim()
                    if (t === "" ) return -1
                    if (t.indexOf("inf") >= 0 || t.indexOf("∞") >= 0 || t === "off" || t === "mute")
                        return 0
                    const db = parseFloat(t)
                    if (!isFinite(db)) return -1
                    const linear = Math.pow(10, Math.min(0, db) / 20)
                    if (linear <= Db.floorLinear) return 0
                    return Math.round(linear * 1000) / 10
                }
                onActiveFocusChanged: {
                    if (activeFocus) selectAll()
                    else refresh()
                }
                onEditingFinished: {
                    const v = dbInput.parseVolume(dbInput.text)
                    if (v >= 0) root.volumeEdited(v)
                    dbInput.refresh()
                    dbInput.focus = false
                }
                Keys.onEscapePressed: (e) => {
                    dbInput.refresh()
                    dbInput.focus = false
                    e.accepted = true
                }
                Connections {
                    target: root
                    function onVolumeChanged() { if (!dbInput.activeFocus) dbInput.refresh() }
                }
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
                height: 52
                radius: Theme.radiusSm
                color: Theme.inset
                border.width: 1
                border.color: Theme.borderSubtle

                readonly property bool chOn: root.channelEnabled(chRow.index)
                Component.onCompleted: {
                    knob.gain = Math.max(0, Math.min(1, root.gainFor(chRow.index)))
                }
                onChOnChanged: knob.gain = chRow.chOn
                    ? root.trimFor(chRow.index) : 0.0

                readonly property bool passes: chRow.chOn && !root.muted
                readonly property real rawLevel: {
                    const v = root.meterLevels.length > chRow.index
                              ? root.meterLevels[chRow.index] : 0
                    return Math.max(0, Math.min(1, v))
                }
                readonly property real level: chRow.passes
                    ? Math.min(1, chRow.rawLevel * (root.volume / 100) * knob.gain)
                    : 0

                property real smoothed: 0
                property real peakValue: 0
                property real peakHeldAt: 0
                Timer {
                    interval: 33
                    running: chRow.visible
                    repeat: true
                    onTriggered: {
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
                    visible: root.showChannelSwitches
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 9
                    width: 15; height: 15
                    radius: 3
                    color: chRow.chOn ? Theme.accent : "transparent"
                    border.width: 1
                    border.color: chRow.chOn ? Theme.accent : Theme.controlBorder
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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

                Item {
                    id: knob
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: box.verticalCenter
                    width: 24; height: 24

                    property real gain: 1.0
                    readonly property real angle: -135 + gain * 270
                    readonly property string stateName: Db.channelState(gain)
                    readonly property color stateColor: Theme.channelStateColor(stateName)
                    readonly property real liveAngle: -135 + Db.dbPct(chRow.smoothed) * 270

                    function setGain(g) {
                        gain = Math.max(0, Math.min(1, g))
                        root.channelGainEdited(chRow.index, gain)
                    }

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

                    readonly property string tipText:
                        qsTr("Channel %1 gain · %2").arg(chRow.index + 1)
                                                     .arg(Db.channelStateText(knob.gain))

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
        visible: root.showChannels && root.showChannelSwitches

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