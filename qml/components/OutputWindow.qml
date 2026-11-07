import QtQuick
import VGRPresenterUI

Window {
    id: root

    objectName: "outputWindow"

    required    property int outputIndex
    property bool outputEnabled: false
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
    onOutputEnabledChanged: if (root.outputEnabled) root.dismissed = false
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
    visible: root.outputEnabled && root.hasDisplay && !root.onAirOnly
             && !root.dismissed && LiveOutputService.live

    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: root.hasFrame
        ? (root.ownBuffer !== ""
            ? "image://livepreview/" + root.ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev)
        : ""

    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
        function onDataChanged() { root.stylePulse++ }
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
        const stagedRaw = LiveOutputService.stagedSlideRaw
        const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
            ? stagedRaw : LiveOutputService.onAirSlideRaw
        const composed = LiveOutputService.previewWithStyle(
            (raw && typeof raw === "object") ? raw : ({}), root.outputStyleId)
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

    readonly property bool blacked: LiveOutputService.blackedOut
    readonly property bool inputTaken: !root.blacked && LiveOutputService.inputLabel !== ""
    // The ENGINE already composes this output's picture — the video AND this output's slide, exactly
    // what the NDI feed sends. While a video is on air and that frame is flowing, this window shows it
    // instead of rebuilding it here: the raw video image (a JPEG decode on the interface thread for a
    // camera, an image copy per frame for a file), the slide composed over it, and the overlays drawn a
    // second time. Same pixels as NDI, and the interface thread does none of that work.
    readonly property bool engineComposed: !root.blacked && root.hasFrame
                                           && (root.inputTaken
                                               || (LiveOutputService.mediaOnAir
                                                   && !LiveOutputService.mediaIsAudio))
    readonly property url inputSource: inputTaken && !engineComposed
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    readonly property bool mediaOnAir: !root.blacked && LiveOutputService.mediaOnAir
                                       && !root.inputTaken
    readonly property url mediaSource: mediaOnAir && !LiveOutputService.mediaIsAudio && !engineComposed
        ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""

    readonly property string effectiveFit: root.onAirSlide.mediaFit || "contain"
    readonly property int effectiveFillMode: effectiveFit === "cover" ? Image.PreserveAspectCrop
                                            : effectiveFit === "fill" ? Image.Stretch
                                                                      : Image.PreserveAspectFit

    readonly property bool framePriority: root.hasFrame
                                           && root.onAirSlide.framePriority === true

    Checkerboard {
        anchors.fill: parent
        visible: root.alphaInspect
    }

    Rectangle {
        anchors.fill: parent
        visible: !root.blacked && root.onAirSlide.styleColor !== ""
                 && root.onAirSlide.styleColor !== "transparent"
        color: root.onAirSlide.styleColor
    }
    Image {
        anchors.fill: parent
        visible: !root.blacked && root.onAirSlide.styleImagePainted === true
        source: root.onAirSlide.styleImagePainted === true
                ? "file:///" + root.onAirSlide.styleImage : ""
        fillMode: Image.PreserveAspectCrop
    }

    Image {
        anchors.fill: parent
        visible: root.inputTaken && !root.engineComposed
        source: root.inputSource
        fillMode: Image.PreserveAspectCrop
        cache: false
        // gui-ok: the camera/NDI tap map the provider reads is interface-thread state (only while an input is taken)
        asynchronous: false
    }

    // Taken media file on air — same convention as the input layer above, but loaded off the interface thread.
    LiveFrameImage {
        anchors.fill: parent
        visible: root.mediaOnAir && !LiveOutputService.mediaIsAudio && !root.engineComposed
        source: root.mediaSource
        fillMode: root.effectiveFillMode   // the file's own fill, else this output's
        sizeToItem: false                  // the provider stretches to a requested size; keep the file's aspect
    }

    // Overlays, UNDER-SLIDE group.
    OverlayLayer {
        underSlide: true
        blacked: root.blacked
        visible: !root.engineComposed   // the engine's frame already carries the overlays
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
        backgroundImage: root.onAirSlide.styleImagePainted === true
            ? root.onAirSlide.styleImage : ""
        blocks: root.onAirSlide.blocks ?? []
        background: root.onAirSlide.frameTransparent === true
                    ? "transparent" : (root.onAirSlide.background ?? "transparent")
    }

    LiveFrameImage {
        anchors.fill: parent
        visible: !root.blacked && root.hasFrame
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
        visible: !root.engineComposed   // the engine's frame already carries the overlays
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