import QtQuick
import VGRPresenterUI

// ONE real destination window for an Output bound to a physical screen —
// what an audience actually sees, as opposed to OutputMonitorTile.qml (the
// small in-app preview tile). Content mirrors OutputMonitorTile's own
// frameSource binding exactly (same image://livepreview provider, same
// per-output buffer-name convention via OutputListModel's frameBuffer
// role) — just full-window, borderless, and without the tile's
// checkerboard/style-preview chrome: a real output shows the live frame or
// plain black, never a placeholder texture (a checkerboard would look like
// a broken feed to an audience, not "nothing on air yet").
//
// Explicit x/y/width/height sized to the bound screen's real geometry
// (OutputListModel.displayFor) rather than Window.FullScreen visibility:
// simpler and more predictable across multi-monitor setups than relying on
// the OS fullscreen transition to land on the right screen, and it's what
// this class of "projector output" window (ProPresenter/OBS-style) already
// does in practice — a borderless window exactly covering the monitor reads
// identically to fullscreen with none of the transition ambiguity.
Window {
    id: root

    required property int outputIndex
    property bool outputEnabled: false
    property string outputScreenName: ""
    // OutputListModel's frameBuffer role ("" for an unstyled output, which
    // mirrors the shared preview feed — the exact convention
    // OutputMonitorTile.qml's ownBuffer already follows).
    property string ownBuffer: ""

    // Settings > Outputs' own per-output toggle (OutputListModel's
    // stayOnTop role) — off by default. Was a corner pill in this window;
    // moved to the real Output settings since that's where every other
    // per-output setting already lives, not a control floating on the
    // output itself.
    property bool outputStayOnTop: false
    // Settings > Outputs' "Fill the whole screen" toggle (OutputListModel's
    // fullscreenOutput role) — on by default, a real projector/HDMI output's
    // actual job. Off starts the window at half-size, centered and movable
    // (a safe test mode for a single dev monitor, where fullscreen would
    // bury the app itself with nothing to click — the Escape/double-click
    // dismiss below is that setup's real escape hatch).
    property bool outputFullscreen: true
    flags: Qt.FramelessWindowHint | (root.outputStayOnTop ? Qt.WindowStaysOnTopHint : 0)
    color: "black"

    // A stays-on-top borderless window with no dismiss gesture is a trap —
    // especially testing without a dedicated second monitor, where the
    // bound "screen" can be the same one the app itself is on, burying the
    // whole UI under a black window with nothing to click. Escape or a
    // double-click hides it; re-enabling the Output (or re-binding it to a
    // screen) brings it back, since THAT is the real "turn this output back
    // on" gesture — this is just an escape hatch, not a replacement for it.
    property bool dismissed: false
    onOutputEnabledChanged: if (root.outputEnabled) root.dismissed = false
    onOutputScreenNameChanged: root.dismissed = false
    // GO LIVE re-activates a dismissed window — the header button's whole
    // job is to put the show on the output, so a window the user dismissed
    // earlier (Escape / double-click) comes back and takes focus the moment
    // anything goes live. Stop keeps it (the claimed-output convention
    // above): only a fresh go-live re-raises it.
    Connections {
        target: LiveOutputService
        function onLiveChanged() {
            if (!LiveOutputService.live)
                return
            root.dismissed = false
            if (root.visible)
                root.requestActivate()
        }
    }

    // Re-reads the screen's real geometry whenever the bound screen name
    // changes (a monitor swap, or first bind) — void marks the dependency
    // explicitly since displayFor() is a plain Q_INVOKABLE, not a role
    // read, same defensive pattern used throughout this codebase for
    // invokable reads inside a binding (see OutputMonitorTile.qml's
    // styleBg/stylePulse comment for the same reasoning).
    readonly property var displayInfo: {
        void root.outputScreenName
        return root.outputScreenName !== "" ? OutputListModel.displayFor(root.outputIndex) : ({})
    }
    readonly property bool hasDisplay: root.displayInfo.width !== undefined

    // Fullscreen (the default): exactly covers the bound screen — the real
    // job of a projector/HDMI output. Off: half the bound screen's size,
    // centered on it, so testing on a single dev monitor doesn't bury the
    // whole app under an always-on-top window with nothing to click. Either
    // way this is only the STARTING geometry: once shown, the window is
    // user-movable (see the DragHandler below), which reassigns x/y
    // directly and quietly drops these bindings, same as any QML property
    // does once something else assigns to it.
    width: root.hasDisplay ? (root.outputFullscreen ? root.displayInfo.width : root.displayInfo.width / 2) : 480
    height: root.hasDisplay ? (root.outputFullscreen ? root.displayInfo.height : root.displayInfo.height / 2) : 270
    x: root.hasDisplay ? root.displayInfo.x + (root.displayInfo.width - root.width) / 2 : 0
    y: root.hasDisplay ? root.displayInfo.y + (root.displayInfo.height - root.height) / 2 : 0
    // Up (covering the screen) whenever the Output is enabled and bound —
    // independent of GO LIVE: a real projector/HDMI output stays claimed
    // once configured, showing black between cues rather than flickering
    // the OS desktop back into view every time the show stops (the same
    // "claimed output" convention ProPresenter/OBS projector windows use).
    visible: root.outputEnabled && root.hasDisplay && !root.dismissed

    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: root.hasFrame
        ? (root.ownBuffer !== ""
            ? "image://livepreview/" + root.ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev)
        : ""

    // This output's own style background ({ color, image, hasImage }) — the
    // SAME role OutputMonitorTile.qml's own styleBg reads. Was plain black
    // whenever there's no frame yet (window claimed, nothing on air): with
    // a real style assigned (a branded colour/image), that's what an idle
    // projector should show — a claimed-but-idle output reading as "off
    // air" black is wrong once a style exists to hold it instead.
    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
    }
    readonly property var styleBg: {
        void root.stylePulse
        void root.outputIndex
        const bg = OutputListModel.styleBackground(root.outputIndex)
        return (bg && typeof bg === "object") ? bg : { color: "", image: "", hasImage: false }
    }

    // "hasFrame" only means the SERVICE thinks a frame exists (live &&
    // frameRev bumped) — it does NOT mean THIS request actually got real
    // pixels back. The per-output buffer this window reads (ownBuffer,
    // rendered by a separate pass than the shared preview the tile uses)
    // isn't always populated in time: reported live as "sometimes it stays
    // black" — hasFrame was true, but the provider had nothing yet and
    // handed back its 1×1-transparent placeholder, which Stretch turns
    // into nothing visible and this window's OWN black fill showed through
    // instead of the style. Checking the loaded IMAGE's own Ready status +
    // real size (not just the service's coarser flag) is the same warm-up
    // check OutputMonitorTile.qml's media layer already uses for the exact
    // same "provider answered but with nothing yet" case.
    readonly property bool frameReady: frameImg.status === Image.Ready && frameImg.sourceSize.width > 1

    Rectangle {
        anchors.fill: parent
        visible: !root.frameReady && root.styleBg.color !== "" && root.styleBg.color !== "transparent"
        color: root.styleBg.color
    }
    Image {
        anchors.fill: parent
        visible: !root.frameReady && root.styleBg.hasImage
        source: root.styleBg.hasImage ? "file:///" + root.styleBg.image : ""
        fillMode: Image.PreserveAspectCrop
    }

    Image {
        id: frameImg
        anchors.fill: parent
        visible: root.hasFrame && root.frameReady
        source: root.frameSource
        fillMode: Image.Stretch
        asynchronous: false
        cache: false
        // The provider hands back an ARGB32 already sized to the request;
        // stretch keeps the mapping 1:1 with this window (matches the
        // preview tile's own distributed-frame Image, OutputMonitorTile.qml).
    }

    // The dismiss escape hatch: covers the whole window so Escape/double-
    // click work no matter where the pointer is, without intercepting
    // anything else (there's nothing else to click on a real output). Also
    // carries the drag-to-move gesture — same startSystemMove() convention
    // AppHeader.qml's own frameless-window drag already uses in this app,
    // reused here rather than hand-rolling x/y tracking.
    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.dismissed = true

        DragHandler {
            target: null
            onActiveChanged: if (active) root.startSystemMove()
        }

        MouseArea {
            anchors.fill: parent
            onDoubleClicked: root.dismissed = true
        }
    }
}
