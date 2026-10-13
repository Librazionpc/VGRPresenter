import QtQuick
import VGRPresenterUI

// ONE L/R channel pill — the round button that glows with a channel's live
// level and closes that channel when clicked.
//
// WHY THIS EXISTS: the Media pane grew three near-identical copies of it — the
// audio input cards' pills, the bus cards' pills and the video cards' small
// overlay pills — and the copies had already drifted (two of the three made
// level look like mute and one muted the whole bus). The user's own words:
// "just take the features those ones have and bring in, you don't need to
// reinvent the wheel". So the wheel is here, once, and every card uses it:
// a bus card's L/R is now literally the same control as a source card's.
//
// PRESENTATIONAL: the pill owns its LOOK (colour, glow, slash, label) and
// reports a click through toggled(). It never touches a model — the consumer
// hands over its row's own channel GAIN and writes a new one on toggled (a
// source row writes a channel gain, a bus writes its own per-side trim), which
// is the one thing the three cards legitimately differ in. The gain arrives as
// a number rather than as a pre-chewed "muted" flag so that the pill's state
// comes from the app's single channel-state vocabulary — see Db.channelState.
Rectangle {
    id: root

    // Which side: 0 = "L", 1 = "R". The label is derived, never passed, so a
    // pill can never be labelled the wrong way round.
    required property int channelIndex
    // Display level 0..1, ALREADY shaped by the consumer (the live meters pass
    // sqrt(peak) so a mic's near-silent baseline still reads as a glowing nub;
    // a stored fader passes its own value). The pill does not re-curve it.
    property real level: 0
    // This channel's GAIN (0..1) — handed over by the consumer from its own
    // model (channelGain). The pill derives the channel's STATE from it through
    // the app's one vocabulary (Db.channelState / Theme.channelStateColor), so a
    // side that is closed or trimmed reads the same here as on the audio
    // dialogs' gain knob. The three cards used to pass a `muted` boolean each
    // computed with their own `<= 0.0005` test, which also made a TRIMMED side
    // invisible: half-closing a channel from the gain knob looked identical to
    // leaving it at unity everywhere else in the app.
    property real gain: 1
    // Derived, never passed. A closed channel is one whose gain is 0 (that IS
    // "off" in this app — the pills write 0 to close a side), and it forces the
    // level to zero: a closed channel shows no signal, the same "closed means
    // dark" rule every other meter in the app follows.
    readonly property bool muted: Db.isChannelOff(gain)
    readonly property bool trimmed: Db.isChannelTrimmed(gain)
    // false = this source carries no audio AT ALL (a camera feed): dimmed and
    // inert, a visible "no audio here" rather than a hidden control.
    property bool capable: true
    // Sizes. TWO SIZES, NAMED — not four numbers re-decided at each call site
    // (they were passed per card, and the pair that sat in a card's corner ended
    // up smaller than the pair the user then asked to be CENTRED and big).
    // The default is the big live-meter pill: its own glow is the meter, so it is
    // sized to be read at a glance. `compact` is the corner overlay pair on a card
    // whose state window shows a picture, which must not cover the feed.
    property bool compact: false
    property int diameter: compact ? 20 : 38
    property real haloSpread: diameter * 0.37
    property real haloWidth: compact ? 4 : 6
    property real labelSize: compact ? 8 : 12
    // Thicker rim while there is signal. (The compact pair keeps a uniform 1px
    // rim — at that size a thicker one reads as a smear.)
    property int activeBorderWidth: compact ? 1 : 2
    // Breathe with the level. Off for the small overlay pair, which shows a
    // stored fader rather than live audio and must not imply live motion.
    property bool scaleWithLevel: true

    // The consumer's write: it knows the row and the model.
    signal toggled()

    // ---- what this pill SAYS it is ----------------------------------------
    // The pill's three states were readable only as tones (the slash, the
    // border, the label's colour), and "trimmed" was invisible in words
    // everywhere. Built here rather than at the three call sites, so the same
    // side reads the same sentence on an input card, a bus card and a video
    // card — the app's channel-state vocabulary in words, with the trim's own
    // dB (Db.channelStateText).
    readonly property string tipText: !capable
        ? qsTr("No audio on this source")
        : qsTr("%1 channel · %2").arg(channelLabel).arg(Db.channelStateText(gain))

    readonly property string channelLabel: channelIndex === 0 ? "L" : "R"
    readonly property real shownLevel: capable && !muted
        ? Math.max(0, Math.min(1, level)) : 0
    readonly property bool active: capable && shownLevel > 0.1

    // Green (quiet) → amber (mid) → red (hot), sampled at the level — one
    // colour for the rim, the halo and the label, so a channel reads as one
    // object whatever it is doing.
    readonly property color glowColor: {
        const t = shownLevel
        const stops = [
            { r: 0.29, g: 0.88, b: 0.63 },
            { r: 1.00, g: 0.82, b: 0.40 },
            { r: 1.00, g: 0.42, b: 0.38 }
        ]
        const seg = t < 0.5 ? 0 : 1
        const k = t < 0.5 ? t / 0.5 : (t - 0.5) / 0.5
        const a = stops[seg], b = stops[seg + 1]
        return Qt.rgba(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k,
                       a.b + (b.b - a.b) * k, 1)
    }

    width: diameter
    height: diameter
    radius: diameter / 2
    // A closed side is INACTIVE, not faulty: it takes the vocabulary's off tone
    // (the same muted chrome the gain knob shows for an off channel), with the
    // slash left to carry "closed" on its own. Danger red is kept for faults.
    color: !capable ? Theme.windowBg
         : muted ? Qt.alpha(Theme.channelOff, 0.16) : Theme.inset
    border.width: active ? activeBorderWidth : 1
    border.color: !capable ? Theme.borderSubtle
                : muted ? Qt.alpha(Theme.channelOff, 0.55)
                : trimmed ? Theme.channelTrimmed
                : active ? glowColor : Theme.borderSubtle
    opacity: capable ? 1 : 0.55
    Behavior on border.color { ColorAnimation { duration: 120 } }
    Behavior on color { ColorAnimation { duration: 120 } }

    // Soft glow halo — the "premium" touch: a wider, dim corona in the SAME
    // sampled colour, brightening with level.
    Rectangle {
        anchors.centerIn: parent
        width: parent.width + root.haloSpread
        height: parent.height + root.haloSpread
        radius: width / 2
        color: "transparent"
        border.width: root.haloWidth
        border.color: root.glowColor
        opacity: root.shownLevel * 0.4
        Behavior on opacity { NumberAnimation { duration: 100 } }
    }

    Text {
        anchors.centerIn: parent
        text: root.channelLabel
        color: !root.capable ? Theme.textMuted
             : root.muted ? Theme.channelOff
             : root.trimmed ? Theme.channelTrimmed
             : (root.active ? root.glowColor : Theme.textSecondary)
        font.family: Theme.fontFamily
        font.pixelSize: root.labelSize
        font.bold: true
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    // Mute slash — only while actually closed, so an idle-but-open channel
    // does not read as off.
    Rectangle {
        visible: root.capable && root.muted
        anchors.centerIn: parent
        width: root.diameter * 0.68
        height: Math.max(1.5, root.diameter / 19)
        rotation: 45
        radius: 1
        color: Theme.channelOff
    }

    Behavior on scale { NumberAnimation { duration: 80 } }
    scale: root.scaleWithLevel ? 1 + root.shownLevel * 0.05 : 1

    MouseArea {
        anchors.fill: parent
        enabled: root.capable
        cursorShape: root.capable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.toggled()
    }

    // The tip's trigger, from POINTER POSITION rather than hover events (the
    // hover-exit this build never delivers would leave the bubble standing — see
    // PositionHoverArea). Its own click sink is disabled, so the pill's MouseArea
    // above still takes the press and the pill's own click behaviour is
    // untouched; and because it goes through AppCursor.blocked(), a pill under an
    // open popup does not raise a tip through it.
    PositionHoverArea {
        id: tipArea
        anchors.fill: parent
        // The pill already shows the hand; leaving the cursor claim to it keeps
        // this purely about the tip.
        showCursor: false
        clickEnabled: false
        onEntered: TipService.show(root.tipText, root)
        onExited: TipService.hide(root)
    }
    // Closing/trimming a side while its tip is up re-labels it in place (the
    // click that does it is ON the pill, so the pointer has not moved).
    onGainChanged: if (tipArea.hovered) TipService.show(root.tipText, root)
    onCapableChanged: if (tipArea.hovered) TipService.show(root.tipText, root)
    // A recycled card delegate unloads mid-hover; a tip must not outlive its
    // control.
    Component.onDestruction: TipService.forget(root)
}
