import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    // Which side: 0 = "L", 1 = "R". The label is derived, never passed, so a
    // pill can never be labelled the wrong way round.
    required property int channelIndex
    property real level: 0
    property real gain: 1
    readonly property bool muted: Db.isChannelOff(gain)
    readonly property bool trimmed: Db.isChannelTrimmed(gain)
    // false = this source carries no audio AT ALL (a camera feed): dimmed and
    // inert, a visible "no audio here" rather than a hidden control.
    property bool capable: true
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
    readonly property string tipText: !capable
        ? qsTr("No audio on this source")
        : qsTr("%1 channel · %2").arg(channelLabel).arg(Db.channelStateText(gain))

    readonly property string channelLabel: channelIndex === 0 ? "L" : "R"
    readonly property real shownLevel: capable && !muted
        ? Math.max(0, Math.min(1, level)) : 0
    readonly property bool active: capable && shownLevel > 0.1

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
    color: !capable ? Theme.windowBg
         : muted ? Qt.alpha(Theme.channelOff, 0.16) : Theme.inset
    border.width: active ? activeBorderWidth : 1
    border.color: !capable ? Theme.borderSubtle
                : muted ? Qt.alpha(Theme.channelOff, 0.55)
                : trimmed ? Theme.channelTrimmed
                : active ? glowColor : Theme.borderSubtle
    opacity: capable ? 1 : 0.55
    Behavior on border.color { ColorAnimation { duration: 120 } }
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
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