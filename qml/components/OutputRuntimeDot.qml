import QtQuick
import VGRPresenterUI

// ============================================================================
// OutputRuntimeDot.qml — ONE output's runtime, as a coloured dot whose numbers
// are on hover.
//
// The colour IS the state (the picture carries no text — the Outputs row owns
// the words):
//   green  — frames are landing on this output
//   amber  — it is up but blacked out, or its feed has stalled
//   grey   — off air / waiting for the first frame / switched off
//
// The hover detail spells out the same state plus what the ENGINE measured for
// THIS output: the frames it has received and how long ago the last one landed
// (LiveOutputService.outputRuntime ← RenderEngine::OutputFeedAges, i.e. the same
// numbers the Outputs screen prints). So "is this one actually sending?" is
// answerable on the wall, without opening Settings.
//
// Built on HoverDetail: the tip, the position-driven hover and the
// forget-on-unload are inherited, not re-implemented. Any surface that needs an
// output's runtime (the wall's tiles and the Outputs rows both do) uses this one
// component, and a surface that needs it to look different overrides `dotColor`
// or `pulse` rather than re-deriving the state.
HoverDetail {
    id: root

    // WHICH output this dot is about — the roster name, exactly as
    // OutputListModel carries it (the runtime map and the stall set are keyed by
    // it).
    property string outputName: ""
    // Whether this output is switched ON in the roster (not Item.enabled — this
    // is the output's own state, so it is named for it). The runtime map only
    // exists while a session is live, so a caller that knows the row's flag (the
    // wall's tile, the Outputs row) passes it: a switched-off output then reads
    // "off" rather than "off air".
    property bool outputEnabled: true
    // The status source: the same maps the Outputs row reads. A caller that
    // already knows the state (a row that also colours its text by it) passes
    // dotColor instead.
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
    // Say the STATE in words beside the dot — and nothing else. A surface where
    // the dot stands alone on the picture (the wall's tiles) needs the word; a
    // surface that already prints its own line (the Outputs row) leaves it off so
    // the same state is not stated twice. The numbers are never here: they are
    // the hover detail (see detailText).
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
