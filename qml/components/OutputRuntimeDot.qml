import QtQuick
import VGRPresenterUI

// ============================================================================
HoverDetail {
    id: root

    property string outputName: ""
    property bool outputEnabled: true
    readonly property var runtime: LiveOutputService.outputRuntime[root.outputName] || null
    // outputFeedStalled() is an invokable — opaque to QML's dependency tracking —
    // so the revision is read to make this re-evaluate when the stall set moves.
    readonly property bool stalled: {
        void LiveOutputService.feedHealthRev
        return LiveOutputService.live && LiveOutputService.outputFeedStalled(root.outputName)
    }
    // The state in the app's words; also what the self-tests and the tile-facts
    // diagnostic read, so the dot's colour cannot drift from its reason.
    readonly property string state: {
        // Switched off is the operator's own decision, so it is said first and
        // in its own words.
        if (!root.outputEnabled)
            return "off"
        if (root.stalled)
            return "no feed"
        if (LiveOutputService.blackedOut)
            return "blackout"
        if (!LiveOutputService.live)
            return "off air"
        if (!root.runtime || root.runtime.presented !== true)
            return "waiting"
        return "live"
    }
    property color dotColor: root.state === "live" ? "#3ddc84"
                             : (root.state === "blackout" || root.state === "no feed"
                                ? Theme.warning : Theme.textMuted)
    // Pulse the dot while it is doing something worth following (a row that is
    // feeding or warming up asks for this; the wall's tiles stay still).
    property bool pulse: false
    property int dotSize: 7
    property bool showDot: true
    property bool showState: false

    // ---- the hover detail (the numbers, not the colour) ---------------------
    readonly property string detailText: {
        switch (root.state) {
        case "off":
            return qsTr("switched off")
        case "no feed":
            return qsTr("no feed — frames stopped")
        case "blackout":
            return qsTr("blackout up — nothing on the screens")
        case "off air":
            return qsTr("off air — go live to send")
        case "waiting":
            return qsTr("waiting for the first frame")
        }
        const frames = Number(root.runtime.frames) || 0
        const ageMs = Number(root.runtime.ageMs)
        if (!(ageMs >= 0))
            return qsTr("live · %1 frames").arg(frames)
        const age = ageMs >= 1000
            ? qsTr("%1 s").arg((ageMs / 1000).toFixed(1))
            : qsTr("%1 ms").arg(Math.max(0, Math.round(ageMs)))
        return qsTr("live · %1 frames · last %2 ago").arg(frames).arg(age)
    }
    title: root.outputName
    detail: root.detailText

    implicitWidth: stateRow.implicitWidth
    implicitHeight: stateRow.implicitHeight

    Row {
        id: stateRow
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: root.dotSize
            height: root.dotSize
            radius: width / 2
            color: root.dotColor
            border.width: 1
            border.color: "#66000000"
            visible: root.showDot

            SequentialAnimation on opacity {
                running: root.pulse && root.state !== "off"
                loops: Animation.Infinite
                NumberAnimation { from: 1; to: 0.3; duration: 700 }
                NumberAnimation { from: 0.3; to: 1; duration: 700 }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.showState
            text: root.state
            color: root.dotColor
            font.family: Theme.fontFamily
            font.pixelSize: 10
            textFormat: Text.PlainText
        }
    }
}