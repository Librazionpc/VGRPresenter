import QtQuick
import VGRPresenterUI

Window {
    id: root

    objectName: "outputWindow"

    required    property int outputIndex
    property bool outputEnabled: false
    // SELF-TEST ONLY: show this window with no display attached and no live session, so a scenario can
    // grab it (an output window is otherwise only up when an output is live on a real screen). Nothing
    // in the app sets this; only the card-preview/the-output-window scenarios do.
    property bool selfTestForceVisible: false
    property bool onAirOnly: false
    property string outputScreenName: ""
    property string ownBuffer: ""
    property string outputRes: ""
    readonly property var parsedRes: {
        const parts = root.outputRes.split("×")
        if (parts.length !== 2)
            return null
        const w = parseInt(parts[0], 10)
        const h = parseInt(parts[1], 10)
        return (w > 0 && h > 0) ? { width: w, height: h } : null
    }

    property bool outputStayOnTop: false
    flags: Qt.FramelessWindowHint | (root.outputStayOnTop ? Qt.WindowStaysOnTopHint : 0)

    color: "transparent"

    readonly property bool alphaInspect: (typeof alphaInspectEnabled !== "undefined")
                                         && alphaInspectEnabled === true

    property bool alphaLogged: false
    onVisibleChanged: {
        if (!root.visible || root.alphaLogged)
            return
        root.alphaLogged = true
        console.log("[OUTPUT-WINDOW] alpha: screen='" + root.outputScreenName
                    + "' color=" + root.color
                    + " alphaBuffer=requested"
                    + " size=" + root.width + "x" + root.height)
    }

    property bool dismissed: false
    onOutputEnabledChanged: {
        if (root.outputEnabled)
            root.dismissed = false
    }   // a switched-off output draws no file: the gate takes `outputEnabled` (see OutputGateState)
    onOutputScreenNameChanged: root.dismissed = false
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

    readonly property var displayInfo: {
        void root.outputScreenName
        void OutputListModel.screensRevision
        return root.outputScreenName !== "" ? OutputListModel.displayFor(root.outputIndex) : ({})
    }
    readonly property bool hasDisplay: root.displayInfo.width !== undefined

    // ONE PICTURE PER DISPLAY AREA. A projector Windows MIRRORS shows up as two screens with one geometry, so two
    // outputs on two differently named screens opened two windows over each other on the one monitor (the lower third
    // drawn on top of Main Output). The earlier output keeps the area; this one stays closed and says why.
    readonly property int mirroredByRow: {
        void root.outputScreenName
        void root.outputEnabled
        void OutputListModel.screensRevision
        return root.outputEnabled && root.hasDisplay ? OutputListModel.mirroredBy(root.outputIndex) : -1
    }
    property int mirrorNoticeFor: -2
    onMirroredByRowChanged: {
        if (root.mirroredByRow < 0 || root.mirrorNoticeFor === root.mirroredByRow)
            return
        root.mirrorNoticeFor = root.mirroredByRow
        const other = OutputListModel.getOutput(root.mirroredByRow)
        const me = OutputListModel.getOutput(root.outputIndex)
        EventBus.notify(qsTr("“%1” is on the same screen area as “%2” (a mirrored display), so it was not opened — two pictures cannot share one display. Extend your displays instead of duplicating them, or give it its own screen in Settings · Routing.")
                            .arg(String(me && me.name ? me.name : ""))
                            .arg(String(other && other.name ? other.name : "")),
                        "warning", qsTr("Outputs"), "outputs.mirrored")
    }

    readonly property var targetGeometry: root.hasDisplay
        ? { x: root.displayInfo.x, y: root.displayInfo.y,
            width: root.parsedRes ? root.parsedRes.width : root.displayInfo.width,
            height: root.parsedRes ? root.parsedRes.height : root.displayInfo.height }
        : { x: 0, y: 0, width: 480, height: 270 }

    width: root.targetGeometry.width
    height: root.targetGeometry.height
    x: root.targetGeometry.x
    y: root.targetGeometry.y

    onTargetGeometryChanged: {
        root.x = root.targetGeometry.x
        root.y = root.targetGeometry.y
        root.width = root.targetGeometry.width
        root.height = root.targetGeometry.height
    }
    visible: root.selfTestForceVisible
             || (root.outputEnabled && root.hasDisplay && !root.onAirOnly && root.mirroredByRow < 0
                 && !root.dismissed && LiveOutputService.live)

    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: root.hasFrame
        ? (root.ownBuffer !== ""
            ? "image://livepreview/" + root.ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview/__live_preview__?v=" + LiveOutputService.frameRev)
        : ""

    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { LiveOutputService.invalidateComposed(); root.stylePulse++ }
        function onDataChanged() { LiveOutputService.invalidateComposed(); root.stylePulse++ }
    }

    property bool outputActive: false

    // ---- On-air content — the SAME data/priority OutputMonitorTile.qml
    readonly property string outputStyleId: {
        void root.stylePulse
        const output = OutputListModel.getOutput(root.outputIndex)
        return output && output.styleId ? String(output.styleId) : ""
    }
    property var onAirSlide: ({})
    property bool onAirSlideBusy: false
    property bool onAirSlidePending: false
    function refreshOnAirSlide() {
        if (root.onAirSlideBusy) {
            root.onAirSlidePending = true
            return
        }
        root.onAirSlideBusy = true
        do {
            root.onAirSlidePending = false
            root.refreshOnAirSlideOnce()
        } while (root.onAirSlidePending)
        root.onAirSlideBusy = false
    }
    // Same memo as the monitor tile (OutputMonitorTile.qml): decide from scalars, and only
    // read the slide for a picture this window has not composed yet.
    function composeFromSlides() {
        const stagedRaw = LiveOutputService.stagedSlideRaw
        const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
            ? stagedRaw : LiveOutputService.onAirSlideRaw
        return LiveOutputService.previewWithStyle(
            (raw && typeof raw === "object") ? raw : ({}), root.outputStyleId)
    }
    function refreshOnAirSlideOnce() {
        const walking = LiveOutputService.stagedHasFlow || LiveOutputService.onAirHasFlow
        const key = walking
            ? "walk|" + root.outputStyleId + "|rev:" + LiveOutputService.stagedRev
              + "|" + LiveOutputService.readingStageSerial + "|" + LiveOutputService.readingPage
              + "|" + LiveOutputService.live
              + "|" + LiveOutputService.mediaOnAir + "|" + LiveOutputService.mediaRev
              + "|" + LiveOutputService.inputLabel
            : ""
        const memo = root.slideMemo
        if (key !== "" && key === memo.key) {
            root.onAirSlide = memo.value
            return
        }
        // (composed once per style for every surface of it — see LiveOutputService.composedForStyle)
        const composed = walking
            ? LiveOutputService.composedForStyle(root.outputStyleId, key.slice(("walk|" + root.outputStyleId + "|").length))
            : root.composeFromSlides()
        memo.key = key
        memo.value = composed
        root.onAirSlide = composed
    }
    readonly property var slideMemo: ({ key: "", value: {} })
    onStylePulseChanged: root.refreshOnAirSlide()
    onOutputStyleIdChanged: root.refreshOnAirSlide()
    Connections {
        target: LiveOutputService
        function onMediaChanged() { root.refreshOnAirSlide() }
        // Per decoded frame and position update: only a change of WHAT is on air recomposes the slide.
        function onMediaTick() {
            const key = LiveOutputService.mediaOnAir + "|" + LiveOutputService.mediaIsAudio
                        + "|" + LiveOutputService.mediaName
            if (key === root.lastMediaKey)
                return
            root.lastMediaKey = key
            root.refreshOnAirSlide()
        }
        // See OutputMonitorTile: the picture changes every frame, the composition does not.
        function onInputChanged() {
            const key = LiveOutputService.inputLabel + "|" + LiveOutputService.inputKind
                        + "|" + LiveOutputService.inputLive
            if (key === root.lastInputKey)
                return
            root.lastInputKey = key
            root.refreshOnAirSlide()
        }
        function onStagedChanged() { root.refreshOnAirSlide() }
        function onOnAirChanged() { root.refreshOnAirSlide() }
    }
    property string lastInputKey: ""
    property string lastMediaKey: ""
    Component.onCompleted: root.refreshOnAirSlide()
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    // WHICH BLOCKS THE ENGINE PUTS BEHIND THE VIDEO (its own backdrop pass). While a file is on air those
    // are the style's background, and the layer order here draws the file ABOVE this window's slide — so
    // they must be marked, or the slide's background paints over the picture and a live output window
    // showing media reads as black until the engine's frame (a taken camera) replaces the whole composition.
    readonly property var blocksForPreview: {
        const bs = root.onAirSlide.blocks ?? []
        if (bs.length === 0 || typeof EngineBridge === "undefined")
            return bs
        const under = EngineBridge.backdropBlockIndexes(bs)
        if (!under || under.length === 0)
            return bs
        const out = []
        for (let i = 0; i < bs.length; ++i)
            out.push(under.indexOf(i) >= 0 ? Object.assign({}, bs[i], { underVideo: true }) : bs[i])
        return out
    }

    readonly property bool blacked: LiveOutputService.blackedOut
    readonly property bool inputTaken: !root.blacked && LiveOutputService.inputLabel !== ""
    // The ENGINE already composes this output's picture when a CAMERA is taken — the video AND this
    // output's slide, exactly what the NDI feed sends. While that frame is flowing, this window shows it
    // instead of rebuilding it here: the raw video image (a JPEG decode on the interface thread), the
    // slide composed over it, and the overlays drawn a second time. Same pixels as NDI, and the
    // interface thread does none of that work. A MEDIA FILE is drawn by the shared LiveMediaSurface
    // instead (the file's own frames from the one player, slide composed over it here), so it never
    // waits on the output's preview feed.
    // The full-resolution HDMI window uses the camera's newest preview frame directly; the small
    // monitor tile may use the engine-composited copy, but that copy visibly lagged on HDMI.
    readonly property bool engineComposed: false
    readonly property url inputSource: inputTaken && !engineComposed
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    // THE MEDIA-ON-AIR GATE, FROM ITS ONE HOME (OutputGateState) — the rule the monitor tile also uses.
    // This window needs the ANSWER, not just LiveMediaSurface's obedience: the tile's mediaOnAir counts an
    // unrouted output as *nothing composed* and draws the checkerboard, while a window that asked only "is
    // a file on air in the app" painted a black frame (the style's colour) beside it.
    OutputGateState {
        id: gate
        outputName: root.routeOutputName
        outputEnabled: root.outputEnabled
        blacked: root.blacked
        inputTaken: root.inputTaken
    }
    readonly property bool mediaGateOpen: gate.mediaOpen
    readonly property bool mediaOnAir: gate.mediaOnAir
    // THE GATE'S NAME, FROM THE OUTPUT MODEL, RE-READ ON THE STYLE PULSE. Two traps here, both found by
    // grabbing this window: the board-row lookup this used to make was keyed by the BOARD's row number,
    // which is not the output index (the buses take the first rows) — so each window asked the gate about
    // a bus, and a bus with a wired source answers "ok"; and it was read once at bind time, before the
    // roster had published, which answers "". Either way the gate opened for every window and the window
    // drew the program's file whatever its own route said (two windows grabbed byte-identical). The
    // output's NAME is what the tiles pass, from the same model.
    readonly property string routeOutputName: {
        void root.stylePulse
        const output = OutputListModel.getOutput(root.outputIndex)
        return output && output.name !== undefined ? String(output.name) : ""
    }
    // THE OVERLAY GATE, FROM THE SAME HOME: an output whose O switch is off paints no overlays, whatever
    // the show layer is doing. Asked BY NAME like the tile (the board's rows are not the output indexes —
    // the buses take the first rows, so the row-keyed version asked about a bus every time).
    readonly property bool overlaysOn: gate.overlaysOn

    readonly property string effectiveFit: root.onAirSlide.mediaFit || "contain"
    readonly property int effectiveFillMode: effectiveFit === "cover" ? Image.PreserveAspectCrop
                                            : effectiveFit === "fill" ? Image.Stretch
                                                                      : Image.PreserveAspectFit

    readonly property bool framePriority: root.hasFrame
                                           && root.onAirSlide.framePriority === true

    // ---- THIS OUTPUT'S EMPTY-STATE RULE (see EmptyFrameState.qml) ----
    // The window must say what the monitor tile says, so the rule has one home and both feed it their own
    // facts: what counts as composed, what a cleared frame is, what a painted frame's colour is, and when
    // the style's own background image is the picture. Neither side can drift from the other again — and
    // this window's `mediaOnAir` already carries ITS route gate, which is what the rule must be told
    // (asking only whether a file was on air in the app is what once painted this window black beside a
    // checkerboard tile).
    EmptyFrameState {
        id: frameState
        enabled: root.outputEnabled
        blacked: root.blacked
        styleId: root.outputStyleId
        slide: root.onAirSlide
        hasSlidePreview: root.hasSlidePreview
        mediaOnAir: root.mediaOnAir
        inputTaken: root.inputTaken
        engineComposed: root.engineComposed
        hasFrame: root.hasFrame
    }
    Connections {
        target: StyleListModel
        function onRosterChanged() { LiveOutputService.invalidateComposed(); root.stylePulse++ }   // the slide composition re-reads with it
    }

    // A frame that paints nothing is TRANSPARENT — cleared, switched off, a transparent style, or nothing
    // composed on a style that paints nothing — and this app draws transparency as the checkerboard.
    readonly property bool frameClear: frameState.frameClear

    // The transparency convention sits at the BOTTOM, as on the tile: whatever paints covers it.
    Checkerboard {
        anchors.fill: parent
        // Checkerboard is for preview/diagnostics; painting it for frameClear made transparent template
        // pixels opaque on the actual HDMI output window.
        visible: root.alphaInspect
    }

    // The colour the output paints: the composition's when there is a slide, otherwise the style's own
    // background, and black when neither paints a colour. Hidden for a clear frame, so the checkers (or
    // the window's own alpha) show through.
    Rectangle {
        anchors.fill: parent
        visible: !root.frameClear
        color: frameState.frameColor
    }
    // The style's background IMAGE — from the composition when there is a slide (it steps aside for
    // clearBackgroundOnText), otherwise the roster's own, so an output whose style owns a background
    // image paints it with no slide on it instead of a black frame.
    Image {
        anchors.fill: parent
        readonly property bool rosterImage: frameState.rosterStyleImage
        visible: frameState.styleImageVisible
        source: frameState.styleImageSource
        fillMode: Image.PreserveAspectCrop
    }

    // A taken camera/screen: the GPU surface (the tap's own frames, no JPEG) — see LiveInputSurface. The
    // image path below is only the fallback for a scene with no GPU.
    LiveInputSurface {
        id: inputSurface
        anchors.fill: parent
        active: root.inputTaken
    }
    // THE FALLBACK IS A SYNCHRONOUS Image, like the monitor tile's: the `videopreview` provider reads the
    // interface thread's own tap table, so an ASYNCHRONOUS load (what a LiveFrameImage does) is served from
    // the loader thread, comes back as the 1x1 "no frame yet" placeholder, and is only retried after
    // 150 ms — which is why a camera on HDMI lagged while the tile beside it did not.
    Image {
        anchors.fill: parent
        visible: root.inputTaken && !inputSurface.pathGpu
        source: inputSurface.pathGpu ? "" : root.inputSource
        fillMode: Image.PreserveAspectCrop
        cache: false   // every rev IS a new frame
        // gui-ok: the camera/NDI tap map the provider reads is interface-thread state (only while an input is taken)
        asynchronous: false
    }

    // Taken media file on air — the shared surface (see OutputMonitorTile/LiveMediaSurface): the file's
    // own frames on the GPU, with the provider path inside it for a machine with no GPU.
    LiveMediaSurface {
        anchors.fill: parent
        // the gate asks BY NAME (see LiveMediaSurface), re-resolved when the board republishes
        outputName: root.routeOutputName
        active: root.mediaOnAir && !LiveOutputService.mediaIsAudio && !root.engineComposed
        fit: root.effectiveFit             // the file's own fill, else this output's
        sizeToItem: false                  // keep the file's own aspect on the provider path
    }

    // Overlays, UNDER-SLIDE group.
    OverlayLayer {
        underSlide: true
        blacked: root.blacked
        // the engine's frame already carries the overlays; the gate decides whether they belong
        visible: !root.engineComposed && root.overlaysOn
    }

    DesignPreview {
        anchors.fill: parent
        visible: !root.blacked && root.hasSlidePreview && !root.framePriority && !root.engineComposed
        // Only Scroll mode draws the passage flow, matching the engine's
        // Slide::flowText contract. Slide mode renders the selected page whole.
        animateLineSteps: LiveOutputService.scrollMode
        readingStyleId: root.outputStyleId !== undefined && root.outputStyleId !== null
                        ? String(root.outputStyleId) : ""
        showBindPlaceholders: false
        showCheckerboard: false
        // The style's background (colour AND image) belongs UNDER the file, where the engine puts it: this
        // layer is above LiveMediaSurface, so painting it here is a picture hidden behind a slide.
        backgroundImage: (root.mediaOnAir || root.inputTaken || root.onAirSlide.frameTransparent === true)
            ? "" : (root.onAirSlide.styleImagePainted === true ? root.onAirSlide.styleImage : "")
        blocks: root.blocksForPreview
        background: (root.mediaOnAir || root.inputTaken || root.onAirSlide.frameTransparent === true)
                    ? "transparent" : (root.onAirSlide.background ?? "transparent")
    }

    LiveFrameImage {
        anchors.fill: parent
        // NEVER OVER A CLEARED FRAME: the engine's preview buffer exists for every output the moment the
        // session goes live, and for an output that composes nothing it holds an empty frame whose alpha
        // the image path flattens — so GO LIVE painted this window flat black over the checkerboard that
        // says the output is empty. frameClear already knows there is nothing here; keep the layer off it.
        visible: !root.blacked && root.hasFrame && !root.frameClear
                 && (root.engineComposed
                     || (root.framePriority && !root.mediaOnAir && !root.inputTaken)
                     || (!root.hasSlidePreview && !root.mediaOnAir && !root.inputTaken))
        source: root.frameSource
        fillMode: Image.Stretch
    }

    // Overlays, OVER-SLIDE group — above everything, including the slide's
    // own text (FreeShow's own overlays-above-text order).
    OverlayLayer {
        blacked: root.blacked
        visible: !root.engineComposed && root.overlaysOn
    }

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
