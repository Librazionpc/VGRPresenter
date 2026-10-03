import QtQuick
import VGRPresenterUI

// ONE real destination window for an Output bound to a physical screen —
// what an audience actually sees, as opposed to OutputMonitorTile.qml (the
// small in-app preview tile). Content now MIRRORS the tile's OWN compositing
// approach directly (DesignPreview drawing the on-air slide's blocks in
// QML) instead of depending on the engine's separate per-output distributed
// frame for text — that frame pipeline (a slower GDI+ text-layout pass,
// its own scene cache, its own "frame ready" signal race) is what kept
// going wrong here across several rounds of fixes, while the tile — using
// this SAME DesignPreview path — never had any of those problems. The
// distributed frame is now used ONLY where the tile also needs it: media/
// camera-kind on-air content, where a block render would just be a
// name-on-a-tile placeholder and the real captured/decoded pixels are the
// honest picture. No checkerboard chrome (unlike the tile): a real output
// shows the live frame/blocks or plain black/style background, never a
// placeholder texture (a checkerboard would look like a broken feed to an
// audience, not "nothing on air yet").
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

    required    property int outputIndex
    property bool outputEnabled: false
    // NDI/network rows are on-air-only: their ONLY transport is their own
    // feed (the receiver's monitor is the audience screen), so this window —
    // the physical-screen transport — must never exist for them. Wired from
    // OutputListModel's onAirOnly role by OutputWindowManager. A row switched
    // HDMI→NDI used to keep its display binding AND this window here: at GO
    // LIVE both transports came up at once (the reported go-live hang).
    property bool onAirOnly: false
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
    // styleBg/stylePulse comment for the same reasoning). Also tracks
    // OutputListModel.screensRevision: a hot-plugged/unplugged monitor or a
    // display-mode change re-evaluates this (and the x/y/width/height below)
    // so the window tracks its monitor LIVE instead of only at next bind.
    readonly property var displayInfo: {
        void root.outputScreenName
        void OutputListModel.screensRevision
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
    // Up ONLY while actually live — GO LIVE is the explicit "put this on
    // the screen" action; an enabled-and-bound-but-not-live Output leaves
    // the OS desktop alone instead of parking a black/branded window over
    // it between services. STOP (or the show ending) takes the window back
    // down the same frame LiveOutputService.live flips.
    visible: root.outputEnabled && root.hasDisplay && !root.onAirOnly
             && !root.dismissed && LiveOutputService.live

    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: root.hasFrame
        ? (root.ownBuffer !== ""
            ? "image://livepreview/" + root.ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev)
        : ""

    // This output's own style background ({ color, image, hasImage }) — the
    // SAME role OutputMonitorTile.qml's own styleBg reads. The window only
    // shows while live (see `visible` above), but a go-live with nothing
    // staged still has to show SOMETHING — the style's own branded colour/
    // image, not plain black — so this stays independent of hasSlidePreview.
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

    // Whether THIS output row is the app's currently-active one — mirrors
    // OutputMonitorTile.qml's own root.active dependency inside
    // suppressStyleBgImage below (wired from OutputListModel's ActiveRole
    // by OutputWindowManager.qml, same as every other per-row property here).
    property bool outputActive: false

    // ---- On-air content — the SAME data/priority OutputMonitorTile.qml
    // reads, so this window shows exactly what the tile shows instead of
    // depending on the engine's own separate (and, historically, buggier)
    // per-output distributed frame for text. See that file's own comments
    // for the full reasoning behind each of these.
    readonly property var onAirSlide: LiveOutputService.stagedSlide.valid === true
        ? LiveOutputService.stagedSlide : LiveOutputService.onAirSlide
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    readonly property bool inputTaken: LiveOutputService.inputLabel !== ""
    readonly property url inputSource: inputTaken
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    readonly property bool mediaOnAir: LiveOutputService.mediaOnAir && !root.inputTaken
    readonly property url mediaSource: mediaOnAir && !LiveOutputService.mediaIsAudio
        ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""

    function isClearColor(c) {
        if (c === undefined || c === null || c === "" || c === "transparent")
            return true
        try { return Qt.color(c).a === 0 } catch (e) { return false }
    }
    readonly property bool styled: !isClearColor(root.styleBg.color) || root.styleBg.hasImage === true

    readonly property bool suppressStyleBgImage: root.styleBg.clearOnText === true
                                                 && root.outputActive
        && (root.hasSlidePreview || root.mediaOnAir || root.inputTaken)

    readonly property bool videoUnderneath: root.inputTaken
        || (root.mediaOnAir && !LiveOutputService.mediaIsAudio)

    // Media-kind on-air blocks (camera/media/audio/image) draw as a
    // placeholder-only tile through DesignPreview — the distributed frame
    // is the honest picture for those specifically; everything else (text/
    // shape/clock/timer) draws true through DesignPreview and wins, since
    // that frame can lag a slide change while the blocks never do.
    readonly property bool framePriority: {
        if (!hasFrame || !hasSlidePreview)
            return false
        const blocks = onAirSlide.blocks
        for (let i = 0; i < blocks.length; ++i) {
            const kind = String(blocks[i] && blocks[i].kind ? blocks[i].kind : "")
            if (kind === "camera" || kind === "media" || kind === "audio" || kind === "image")
                return true
        }
        return false
    }

    Rectangle {
        anchors.fill: parent
        visible: root.styleBg.color !== "" && root.styleBg.color !== "transparent"
        color: root.styleBg.color
    }
    Image {
        anchors.fill: parent
        visible: root.styleBg.hasImage && !root.suppressStyleBgImage && !root.videoUnderneath
        source: root.styleBg.hasImage && !root.suppressStyleBgImage && !root.videoUnderneath
                ? "file:///" + root.styleBg.image : ""
        fillMode: Image.PreserveAspectCrop
    }

    // Taken camera/screen input — under the on-air content, above the style
    // background (camera-behind-lyrics), same layer OutputMonitorTile.qml
    // draws.
    Image {
        anchors.fill: parent
        visible: root.inputTaken
        source: root.inputSource
        fillMode: Image.PreserveAspectCrop
        cache: false
        asynchronous: false
    }

    // Taken media file on air — same convention as the input layer above.
    Image {
        anchors.fill: parent
        visible: root.mediaOnAir && !LiveOutputService.mediaIsAudio
        source: root.mediaSource
        fillMode: Image.PreserveAspectFit
        cache: false
        asynchronous: false
    }

    // Overlays, UNDER-SLIDE group — same model/lookup as
    // OutputMonitorTile.qml.
    Repeater {
        model: LiveOutputService.activeOverlays
        delegate: DesignPreview {
            required property var modelData
            readonly property var design: OverlayLibraryService.design(modelData.id)
            anchors.fill: parent
            visible: design.placeUnderSlide === true
            showCheckerboard: false
            blocks: design.blocks ?? []
            background: design.background ?? "transparent"
        }
    }

    // The on-air slide, drawn block-true through the SAME renderer the
    // preview tile uses — this is the fix: no more depending on the
    // engine's own separate distributed render for text.
    DesignPreview {
        anchors.fill: parent
        visible: root.hasSlidePreview && !root.framePriority
        showBindPlaceholders: false
        showCheckerboard: false
        backgroundImage: (root.styleBg.hasImage && !root.suppressStyleBgImage && !root.videoUnderneath)
            ? root.styleBg.image : ""
        blocks: root.onAirSlide.blocks ?? []
        background: root.videoUnderneath ? "transparent" : (root.onAirSlide.background ?? "transparent")
    }

    // The distributed frame — wins only for media-kind on-air content (a
    // block render would just be a name-on-a-tile placeholder), or when
    // there's no slide preview at all but the service still has a frame
    // (camera/screen taken with nothing else on air). Never over a taken
    // input/media layer, which already IS the real picture.
    Image {
        anchors.fill: parent
        visible: root.hasFrame
                 && (root.framePriority && !root.mediaOnAir && !root.inputTaken
                     || (!root.hasSlidePreview && !root.mediaOnAir && !root.inputTaken))
        source: root.frameSource
        fillMode: Image.Stretch
        asynchronous: false
        cache: false
    }

    // Overlays, OVER-SLIDE group — above everything, including the slide's
    // own text (FreeShow's own overlays-above-text order).
    Repeater {
        model: LiveOutputService.activeOverlays
        delegate: DesignPreview {
            required property var modelData
            readonly property var design: OverlayLibraryService.design(modelData.id)
            anchors.fill: parent
            visible: design.placeUnderSlide !== true
            showCheckerboard: false
            blocks: design.blocks ?? []
            background: design.background ?? "transparent"
        }
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
