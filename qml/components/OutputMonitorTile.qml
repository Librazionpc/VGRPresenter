import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    // OutputListModel roles
    required property string name
    required property string badge
    required property string styleId
    required property bool active
    required property bool isEnabled
    required property var model
    // THE MEDIA-ON-AIR GATE AND THE OVERLAY GATE, FROM THEIR ONE HOME (OutputGateState). They are asked
    // BY NAME (see LiveMediaSurface), never by row index: the tile's model index and the board's row were
    // not the same key in every model, and an index-keyed gate answered about someone else's row — that
    // is how a connected output could sit black. This tile and the output window used to each carry the
    // rule, and the copies drifted (see the component's header).
    OutputGateState {
        id: gate
        outputName: String(root.name)
        outputEnabled: root.isEnabled
        blacked: root.blacked
        inputTaken: root.inputTaken
    }
    readonly property bool mediaGateOpen: gate.mediaOpen
    readonly property bool overlaysOn: gate.overlaysOn

    readonly property string ownBuffer: (root.model !== null && root.model !== undefined)
                                        ? String(root.model.frameBuffer ?? "") : ""
    readonly property bool hasFrame: LiveOutputService.frameRev > 0
                                     && (LiveOutputService.live || root.mediaOnAir)
    readonly property url frameSource: {
        if (!hasFrame)
            return ""
        return (LiveOutputService.live && ownBuffer !== "")
            ? "image://livepreview/" + ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview/__live_preview__?v=" + LiveOutputService.frameRev
    }

    // ---- THE ONE COMPOSITION THIS TILE DRAWS FROM --------------------------
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
    function refreshOnAirSlideOnce() {
        // The key first, and only from scalars: a hit must not touch the slide (reading it
        // converts the whole passage into JS, once per handler, on the thread that draws).
        // That is several handlers per pick — the tile's own Connections alone has seven.
        const walking = LiveOutputService.stagedHasFlow || LiveOutputService.onAirHasFlow
        const key = walking
            ? "walk|" + root.styleId + "|" + root.stylePulse + "|rev:"
              + LiveOutputService.stagedRev
              + "|" + LiveOutputService.readingSetName + "|" + LiveOutputService.readingStageSerial
              + "|" + LiveOutputService.readingPage
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
            (raw && typeof raw === "object") ? raw : ({}), root.styleId)
        EngineBridge.log("trace", "RefreshOnAirSlide",
            "tile=" + root.objectName + " recomposed key=" + key.slice(0, 60)
            + " blocks0Text=" + String((composed.blocks && composed.blocks[0]
                && composed.blocks[0].text) ?? "").slice(0, 24))
        memo.key = key
        memo.value = composed
        root.onAirSlide = composed
    }
    // A plain object mutated in place: its IDENTITY never changes, so writing it
    // cannot re-enter the binding above (a property write there would).
    readonly property var slideMemo: ({ key: "", value: {} })
    onStylePulseChanged: root.refreshOnAirSlide()
    onStyleIdChanged: root.refreshOnAirSlide()
    Connections {
        target: LiveOutputService
        function onMediaChanged() {
            // ONE LINE PER TAKE-STATE CHANGE (never per frame): what this tile decided to draw, so a black
            // tile is read from engine.log instead of guessed at.
            EngineBridge.log("info", "TilePicture",
                "tile=" + root.name + " onAir=" + LiveOutputService.mediaOnAir
                + " localOnAir=" + root.mediaOnAir + " video=" + LiveOutputService.mediaIsVideo
                + " live=" + LiveOutputService.live + " engineComposed=" + root.engineComposed
                + " slide=" + root.hasSlidePreview + " overlays=" + root.overlaysOn
                + " route=" + RoutingService.mediaRouteStateForName(String(root.name))
                + " styleImage=" + (root.onAirSlide.styleImagePainted === true))
            root.refreshOnAirSlide()
        }
        // mediaTick fires with every decoded frame and every position update. The composition depends on
        // WHAT is on air, not on which frame of it: recomposing the whole slide per frame, for every tile,
        // was ~0.4 s of interface-thread work per frame (the meters and animations froze with it).
        function onMediaTick() {
            const key = LiveOutputService.mediaOnAir + "|" + LiveOutputService.mediaIsAudio
                        + "|" + LiveOutputService.mediaName
            if (key === root.lastMediaKey)
                return
            root.lastMediaKey = key
            root.refreshOnAirSlide()
        }
        // inputChanged fires with EVERY video frame (the revision counter rides it). The slide's
        // composition depends on WHICH input is taken, not on its pixels: recomposing the whole
        // slide per video frame, for every tile, was GUI-thread work that stalled the picture.
        function onInputChanged() {
            const key = LiveOutputService.inputLabel + "|" + LiveOutputService.inputKind
                        + "|" + LiveOutputService.inputLive
            if (key === root.lastInputKey)
                return
            root.lastInputKey = key
            root.refreshOnAirSlide()
        }
        function onReadingPageChanged() { root.refreshOnAirSlide() }
        function onStagedChanged() { root.refreshOnAirSlide() }
        function onOnAirChanged() { root.refreshOnAirSlide() }
        function onLiveChanged() { root.refreshOnAirSlide() }
    }
    property string lastInputKey: ""
    property string lastMediaKey: ""
    Component.onCompleted: root.refreshOnAirSlide()
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0
    readonly property int readingDrawnLines: contentPreview.readingDrawnLines
    readonly property int readingExpectedLines: contentPreview.readingExpectedLines
    readonly property string readingDrawnAt: String(root.objectName) + "@"
                                            + Number(contentPreview.readingDrawnBoxW).toFixed(0)
                                            + "x" + Number(contentPreview.readingDrawnBoxH).toFixed(0)

    // ---- What the feed does NOT bury: the backdrop pass ------------------
    readonly property var backdropMemo: ({ key: "", value: [] })
    property var backdropIndexes: []
    function refreshBackdropIndexes() {
        const bs = root.onAirSlide && root.onAirSlide.blocks ? root.onAirSlide.blocks : []
        if (bs.length === 0 || typeof EngineBridge === "undefined") {
            root.backdropIndexes = []
            return
        }
        let key = ""
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            key += b.kind + "," + b.x + "," + b.y + "," + b.width + "," + b.height + ";"
        }
        const memo = root.backdropMemo
        if (key === memo.key) {
            root.backdropIndexes = memo.value
            return
        }
        const plan = EngineBridge.backdropBlockIndexes(bs)
        memo.key = key
        memo.value = plan
        root.backdropIndexes = plan
    }
    onOnAirSlideChanged: {
        root.refreshBackdropIndexes()
        root.refreshMarkedBlocks()
    }
    property var markedBlocks: []
    function refreshMarkedBlocks() {
        const bs = root.onAirSlide && root.onAirSlide.blocks ? root.onAirSlide.blocks : []
        const under = root.backdropIndexes
        if (under.length === 0) {
            root.markedBlocks = bs
            return
        }
        const out = []
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            out.push(under.indexOf(i) >= 0 ? Object.assign({}, b, { underVideo: true }) : b)
        }
        root.markedBlocks = out
    }
    readonly property var backdropBlocks: root.markedBlocks.filter(b => b.underVideo === true)

    // ---- Taken input layer (the Media pane's click-to-preview) ----------
    readonly property bool blacked: LiveOutputService.blackedOut
    readonly property bool inputTaken: !root.blacked && LiveOutputService.inputLabel !== ""
    // While live and a CAMERA is taken, the ENGINE has already composed this output's picture (the video
    // AND its slide: exactly what NDI sends). The tile shows that frame, loaded off the interface thread,
    // instead of decoding the camera's JPEG here for every frame and drawing the slide over it a second
    // time — that per-frame work on the interface thread is what made the audio meters stutter. A MEDIA
    // FILE is never taken from the engine's preview feed: the shared LiveMediaSurface draws the file's
    // own frames (straight from the one player) and the slide composes over it here, before and after
    // GO LIVE alike, so the tile keeps up with the file instead of the output's preview feed.
    readonly property bool engineComposed: !root.blacked && LiveOutputService.live && root.hasFrame
                                           && root.inputTaken
    readonly property url inputSource: inputTaken && !engineComposed
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    // ---- Media-on-air STATE (the Media pane's take-to-program) -----------
    // GATED, not merely "is a file playing": a tile whose output has its Media layer off draws no file,
    // shows no name pill and meters nothing — the take's notice is the whole story of that tile.
    readonly property bool mediaOnAir: gate.mediaOnAir

    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
    }

    function familyLabel(type) {
        if (type === "media") return qsTr("Media")
        if (type === "scripture") return qsTr("Scripture")
        if (type === "table") return qsTr("Table")
        if (type === "shows" || type === "") return qsTr("Shows")
        return qsTr("content")
    }

    readonly property string effectiveFit: root.onAirSlide.mediaFit || "contain"
    readonly property int effectiveFillMode: effectiveFit === "cover" ? Image.PreserveAspectCrop
                                            : effectiveFit === "fill" ? Image.Stretch
                                                                      : Image.PreserveAspectFit

    readonly property bool framePriority: root.hasFrame
                                           && root.onAirSlide.framePriority === true

    // ---- THE OUTPUT'S OWN STYLE BACKGROUND AND THE EMPTY-STATE RULE (see EmptyFrameState) ----
    // The tile and the output window are the same frame at two sizes, so what a cleared frame is, and
    // what a frame that is not clear paints, has ONE home now: this tile and that window each used to
    // carry their own copy, and the copies drifted (the window asked only whether a file was on air in
    // the app, and both asked the engine's buffer whether a frame EXISTED rather than what it held —
    // GO LIVE then turned a cleared frame from checkers into a black rectangle).
    EmptyFrameState {
        id: frameState
        enabled: root.isEnabled
        blacked: root.blacked
        styleId: root.styleId
        slide: root.onAirSlide
        hasSlidePreview: root.hasSlidePreview
        mediaOnAir: root.mediaOnAir
        inputTaken: root.inputTaken
        engineComposed: root.engineComposed
        hasFrame: root.hasFrame
    }
    // The style-background scenario reads these off the TILE (Main.qml's bsReport), so they stay as the
    // shared rule's answers — not a second copy of it.
    readonly property var styleBackground: frameState.styleBackground
    readonly property string styleBackgroundColor: frameState.styleBackgroundColor
    readonly property string styleBackgroundImage: frameState.styleBackgroundImage
    readonly property bool styleBackgroundPaints: frameState.styleBackgroundPaints
    Connections {
        target: StyleListModel
        function onRosterChanged() { root.stylePulse++ }   // the slide composition re-reads with it
    }

    // WHAT AN EMPTY TILE PAINTS (see previewPane): switched off, cleared, a transparent style — or
    // nothing composed on a style that paints nothing. The rule itself lives in EmptyFrameState so this
    // tile and the output window cannot answer differently.
    readonly property bool frameClear: frameState.frameClear

    // Set by a host that gives this tile more height than 16:9 (see MonitorWall's
    // solo page): the picture keeps its 16:9 frame and is LETTERBOXED inside the
    // tile — centred, with the tile's own ground above and below — so a taller
    // page never crops the frame to fill it.
    property bool fillHeight: false

    implicitWidth: 182
    implicitHeight: 2 + previewPane.height

    // ---- The L/R meter strips --------------------------------------------
    radius: 8
    border.color: "#2b2e3d"
    border.width: 1
    color: Theme.rowBg
    // Disabled screens dim everywhere — same model, same state.
    opacity: root.isEnabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: 120 } }

    Rectangle {
        id: previewPane
        objectName: "selfTestTilePane"
        x: 1
        y: root.fillHeight ? 1 + Math.max(0, (root.height - 2 - height) / 2) : 1
        width: parent.width - 2
        // The frame is always 16:9; a taller tile places it in the middle and the
        // bars above and below are the tile's own background (letterbox, no crop).
        height: (root.width - 2) * 9 / 16
        clip: true
        radius: 7
        // THE TWO EMPTY STATES, drawn as themselves: transparent (checkerboard, see frameClear) or the
        // colour the output actually paints — the composition's when there is a slide, otherwise the
        // style's own background, and black when neither paints a colour (EmptyFrameState.frameColor).
        color: root.frameClear ? "transparent" : frameState.frameColor

        Checkerboard {
            objectName: "selfTestTileCheckers"
            anchors.fill: parent
            visible: root.frameClear
        }

        // The style's background IMAGE. With a slide the composition decides (it steps aside for
        // clearBackgroundOnText); with NO slide there is no composition to ask, so the roster's own
        // background image is the one this output paints — the same picture, not a black tile. A
        // SWITCHED-OFF output is the exception and stays in here: it sends nothing (see frameClear),
        // so its style's picture must not cover the checkerboard that says so.
        Image {
            objectName: "selfTestTileStyleImage"   // the layer the style-bg scenario reads
            anchors.fill: parent
            readonly property bool rosterImage: frameState.rosterStyleImage
            visible: frameState.styleImageVisible
            source: frameState.styleImageSource
            fillMode: Image.PreserveAspectCrop
        }

        DesignPreview {
            objectName: "selfTestTileBackdropPreview"
            anchors.fill: parent
            // A CLEAR FRAME IS NOT "NOTHING TO SHOW" WHEN THERE IS A SLIDE. framePriority only says the
            // engine frame CAN stand in for the design; for a keyed output that frame is a transparent
            // stage (frameClear), so hiding both layers left the tile empty while the pick was on air —
            // the reported "go live clears the lower third". With content composed, the composition draws.
            visible: !root.blacked && root.hasSlidePreview
                     && (!root.framePriority || root.frameClear) && !root.engineComposed
            showCheckerboard: false
            background: "transparent"
            backgroundImage: ""
            blocks: root.backdropBlocks
            paintOnly: true
        }

        // A taken camera/screen on the GPU (see LiveInputSurface): the same frames the HDMI window shows.
        LiveInputSurface {
            id: inputSurface
            objectName: "selfTestTileInputSurface"
            anchors.fill: parent
            active: root.inputTaken && !root.engineComposed
        }

        Image {
            id: inputImage
            objectName: "selfTestTileInputFeed"
            anchors.fill: parent
            visible: root.inputTaken && !root.engineComposed && !inputSurface.pathGpu
            source: inputSurface.pathGpu ? "" : root.inputSource
            fillMode: Image.PreserveAspectCrop
            cache: false   // every rev IS a new frame
            // gui-ok: the camera/NDI tap map the provider reads is interface-thread state (only while an input is taken)
            asynchronous: false
        }

        // THE MEDIA ON AIR — the one shared surface the output windows and the stage use too. A video
        // file's decoded frames reach this tile's VideoOutput on the GPU (no per-tile QImage per frame),
        // and a machine with no GPU falls back inside it. Shown before and after GO LIVE: the slide and
        // the overlays compose over it here, whichever path drew the frame.
        LiveMediaSurface {
            objectName: "selfTestTileMediaFrame"
            anchors.fill: parent
            outputName: String(root.name)  // the gate asks THIS output's route before drawing the file
            active: root.mediaOnAir && !LiveOutputService.mediaIsAudio
            fit: root.effectiveFit
        }

        // Warm-up placeholder while an input is taken but no frame has
        // decoded yet (the shared pane's camera ring art, inline).
        Item {
            anchors.centerIn: parent
            visible: root.inputTaken && !LiveOutputService.inputLive
            width: 48; height: 48

            Rectangle {
                anchors.centerIn: parent
                width: 34; height: 34; radius: 17
                color: "transparent"
                border.width: 2.4
                border.color: Theme.accent
            }
            Rectangle {
                anchors.centerIn: parent
                width: 10; height: 10; radius: 5
                color: Theme.accent
            }
        }

        // ---- AUDIO CENTRE PREVIEW --------------------------------------
        Item {
            id: audioCentre
            objectName: "selfTestTileAudioCentre"
            anchors.fill: parent
            z: 50
            visible: root.mediaOnAir && LiveOutputService.mediaIsAudio && !root.blacked

            readonly property bool playing: LiveOutputService.mediaState === "playing"
            readonly property real progress: LiveOutputService.mediaDuration > 0
                ? Math.max(0, Math.min(1, LiveOutputService.mediaPosition / LiveOutputService.mediaDuration))
                : 0

            // A quiet wash so the card reads over a checkerboard or a style
            // background without pretending to be video.
            Rectangle {
                anchors.fill: parent
                // Near-solid on purpose: it covers the slide's file-path text
                // block, which must not read through the track card.
                color: Qt.alpha(Theme.panelBg, 0.94)
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 300)
                spacing: 8

                IconGlyph {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "audio"
                    color: Theme.textSecondary
                    width: 40; height: 40; fit: true
                    strokeWidth: 1.4
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                    text: LiveOutputService.mediaName
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }

                // One big control — play/pause is the only thing a person
                // reaches for on a track card.
                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 40; height: 40
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Qt.alpha(Theme.accent, playHover.hovered ? 0.22 : 0.14)
                        border.width: 1
                        border.color: Qt.alpha(Theme.accent, 0.35)
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: LiveOutputService.mediaState === "playing" ? "pause" : "play"
                        color: Theme.textPrimary
                        width: 16; height: 16; fit: true
                    }
                    HoverHandler { id: playHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: LiveOutputService.mediaTogglePlay() }
                }

                Item {
                    width: parent.width
                    height: 14

                    Text {
                        id: tileElapsed
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: LiveOutputService.formatMediaTime(LiveOutputService.mediaPosition)
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Text {
                        id: tileTotal
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: LiveOutputService.formatMediaTime(LiveOutputService.mediaDuration)
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }
                    Item {
                        id: tileSeek
                        anchors.left: tileElapsed.right
                        anchors.right: tileTotal.left
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        height: 14
                        function seekTo(x) {
                            if (tileSeek.width <= 0) return
                            LiveOutputService.mediaSeek(Math.max(0, Math.min(1, x / tileSeek.width)))
                        }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width; height: 3; radius: 1.5
                            color: Theme.chip
                        }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width * audioCentre.progress
                            height: 3; radius: 1.5
                            color: Theme.accent
                        }
                        TapHandler {
                            onTapped: function (point) { tileSeek.seekTo(point.position.x) }
                        }
                        DragHandler {
                            onCentroidChanged: if (active) tileSeek.seekTo(centroid.position.x)
                        }
                    }
                }
            }
        }

        OverlayLayer {
            underSlide: true
            blacked: root.blacked
            // the engine's frame already carries the overlays; the gate decides whether they belong
            visible: !root.engineComposed && root.overlaysOn
        }

        DesignPreview {
            id: contentPreview
            objectName: "selfTestTileDesignPreview"
            anchors.fill: parent
            // (the same rule as the backdrop layer: a clear engine frame must not hide the composition)
            visible: !root.blacked && root.hasSlidePreview
                     && (!root.framePriority || root.frameClear) && !root.engineComposed
            // Only Scroll mode draws the passage flow. In Slide mode the selected
            // page stays intact even if an earlier scroll left flow data on it.
            animateLineSteps: LiveOutputService.scrollMode
            readingStyleId: root.styleId !== undefined && root.styleId !== null
                            ? String(root.styleId) : ""
            showBindPlaceholders: false
            showCheckerboard: false
            // THE SAME COVER AS THE COLOUR, one layer up: a style's background IMAGE painted here lands
            // ON TOP of the file (this layer is above LiveMediaSurface) and a dark one is exactly a black
            // tile that only the engine's frame could fill in — i.e. a picture that appeared on GO LIVE.
            // While the file is the picture the style background is drawn by the backdrop layer UNDER it.
            backgroundImage: (root.mediaOnAir || root.onAirSlide.frameTransparent === true)
                ? "" : (root.onAirSlide.styleImagePainted === true ? root.onAirSlide.styleImage : "")
            blocks: root.markedBlocks
            // MEDIA IS THE PICTURE. The file draws UNDER the slide's text (the backdrop layer above draws
            // the slide's background behind it), so while a file is on air this layer must not paint the
            // slide's own background back over it — an opaque one there is exactly a black tile with the
            // media's name pill and nothing else.
            background: (root.mediaOnAir || root.onAirSlide.frameTransparent === true)
                        ? "transparent" : (root.onAirSlide.background ?? "transparent")
        }

        LiveFrameImage {
            id: frameImage
            objectName: "selfTestTileFrame"
            anchors.fill: parent
            // THE SAME PICTURE BEFORE AND AFTER GO LIVE. The engine's frame may only replace the tile's own
            // composition for a TAKEN INPUT (engineComposed): a media file is drawn by LiveMediaSurface on
            // both sides of GO LIVE, so pressing it cannot swap the renderer under a file that is already
            // playing. The old `mediaOnAir && !live` term did exactly that swap — it let the engine frame
            // take the tile the moment the app went live, which is why GO LIVE changed the picture.
            // AND NEVER OVER A CLEARED FRAME, for the same reason: the engine's preview buffer exists for
            // every output from the moment the session goes live, and for one that composes nothing it is
            // an empty frame whose alpha the image path flattens — so GO LIVE turned this tile from the
            // transparency checkerboard into a black rectangle that said the opposite.
            visible: !root.blacked && root.hasFrame && !root.frameClear
                     && (root.engineComposed
                         || ((root.framePriority || !root.hasSlidePreview)
                             && !root.inputTaken
                             && !root.mediaOnAir))
            source: root.frameSource
            fillMode: Image.Stretch
            // Loaded off the interface thread and double buffered (LiveFrameImage); the provider hands back
            // the frame already scaled to this tile's size, so stretch keeps the mapping 1:1 with the pane.
        }

        OverlayLayer {
            blacked: root.blacked
            visible: !root.engineComposed && root.overlaysOn
        }

        // (The LIVE pill was removed by request — the red border alone marks
        // the active output.)
    }

    // ---- Program-mix meter (FreeShow's own output-preview design) ---------
    component MeterBar: Item {
        id: bar
        required property int channelIndex
        y: previewPane.y + 4
        width: 4
        height: previewPane.height - 8
        visible: root.isEnabled

        readonly property string mediaMeterLabel: LiveOutputService.mediaMeterLabel
        readonly property real mediaLevel: {
            // A FILE THAT CANNOT REACH THIS OUTPUT HAS NO LEVEL ON ITS METER (see mediaGateOpen).
            if (!LiveOutputService.mediaOnAir || !root.mediaGateOpen)
                return 0
            const list = EngineBridge.inputLevels
            for (let i = 0; i < list.length; ++i) {
                const snap = list[i]
                if (!snap || String(snap.label || "") !== bar.mediaMeterLabel)
                    continue
                if (!snap.peaks || snap.peaks.length < 1)
                    continue
                const channels = Math.min(Number(snap.channelCount) || snap.peaks.length,
                                          snap.peaks.length)
                const ch = channels <= 1 ? 0 : Math.min(bar.channelIndex, channels - 1)
                return Math.max(0, Math.min(1, Number(snap.peaks[ch]) || 0))
            }
            return 0
        }

        readonly property real rawLevel: {
            // NOTHING IS REACHING THIS OUTPUT: no strip at all, whatever is playing elsewhere.
            if (!root.mediaGateOpen)
                return 0
            // A file on air owns the strip (see mediaLevel).
            if (bar.mediaLevel > 0)
                return bar.mediaLevel
            if (!LiveOutputService.live)
                return bar.mediaLevel
            const list = EngineBridge.inputLevels
            // ONLY WHAT THE OPERATOR SELECTED IN THE MEDIA TAB (a metered card, or a monitored
            // bus's audio). Any microphone with an open tap used to animate the live output —
            // including one that the Audio & Video page opened for its own level bar.
            const selected = LiveOutputService.monitoredAudio
            let loudest = 0
            for (let i = 0; i < list.length; ++i) {
                const snap = list[i]
                if (!snap || snap.peaks === undefined || snap.peaks.length < 1)
                    continue
                if (String(snap.label || "").trim() === "")
                    continue
                if (selected.indexOf(String(snap.label).trim()) < 0)
                    continue
                const channelCount = Number(snap.channelCount) || snap.peaks.length
                const sourceChannel = channelCount === 1 ? 0 : bar.channelIndex
                const gainChannel = channelCount === 1 ? 0 : bar.channelIndex
                if (sourceChannel >= snap.peaks.length)
                    continue
                const label = String(snap.label || "").trim()
                for (let row = 0; row < AudioInputListModel.rowCount(); ++row) {
                    const input = AudioInputListModel.getInput(row)
                    if (!input || String(input.sublabel || "").trim() !== label
                        || input.muted || Number(input.level) <= 0)
                        continue
                    const gain = AudioInputListModel.channelGain(row, gainChannel)
                    if (Db.isChannelOff(gain))
                        continue
                    const v = Math.max(0, Math.min(1, snap.peaks[sourceChannel]))
                              * gain * Math.max(0, Math.min(1, Number(input.level) / 100))
                    if (v > loudest) loudest = v
                    break
                }
            }
            return loudest
        }

        property real smoothed: 0
        property real peakValue: 0
        property real peakHeldAt: 0
        readonly property bool active: rawLevel > 0.01

        Timer {
            interval: 33
            // ONLY WHILE THERE IS SOMETHING TO ANIMATE: a silent bar at rest ran this 30x a second per tile
            // for nothing. It restarts the moment the level rises, and stops once the decay has settled.
            running: bar.visible && (bar.rawLevel > Db.floorLinear || bar.smoothed > 0 || bar.peakValue > 0)
            repeat: true
            onTriggered: {
                if (bar.rawLevel <= Db.floorLinear) {
                    bar.smoothed = 0
                    bar.peakValue = 0
                    bar.peakHeldAt = Date.now()
                    return
                }
                const target = bar.rawLevel
                bar.smoothed = target > bar.smoothed
                    ? target : bar.smoothed + (target - bar.smoothed) * 0.2

                const now = Date.now()
                if (bar.smoothed >= bar.peakValue) {
                    bar.peakValue = bar.smoothed
                    bar.peakHeldAt = now
                } else if (now - bar.peakHeldAt > 2000) {
                    bar.peakValue = Math.max(bar.smoothed, bar.peakValue - 0.02)
                }
            }
        }

        readonly property Gradient barGradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Theme.meterClip }
            GradientStop { position: 0.16; color: Theme.meterHot }
            GradientStop { position: 0.45; color: Theme.meterLoud }
            GradientStop { position: 1.0; color: Theme.meterQuiet }
        }

        // Ghost — the full range, always faintly visible.
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            opacity: 0.18
            gradient: bar.barGradient
        }

        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * Db.dbPct(bar.smoothed)
            clip: true

            Rectangle {
                width: parent.width
                height: bar.height
                anchors.bottom: parent.bottom
                radius: width / 2
                gradient: bar.barGradient
            }
        }

        // Peak-hold tick.
        Rectangle {
            visible: bar.peakValue > 0.01
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2
            y: parent.height * (1 - Db.dbPct(bar.peakValue)) - 1
            color: Theme.controlInk
            opacity: 0.55
        }

        // Signal indicator — a thin on/off tick at the strip's foot, the
        // vertical form AudioMeter.svelte's own signal-dot takes.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: Theme.meterQuiet
            opacity: bar.active ? 1 : 0.15
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }
    }

    MeterBar {
        channelIndex: 0
        anchors.left: previewPane.left
        anchors.leftMargin: 4
    }
    MeterBar {
        channelIndex: 1
        anchors.right: previewPane.right
        anchors.rightMargin: 4
    }

    Rectangle {
        anchors.top: previewPane.top
        anchors.topMargin: 4
        anchors.left: previewPane.left
        anchors.leftMargin: 4
        width: gateRow.width + 12
        height: gateRow.height + 6
        radius: 9
        color: "#b3101219"
        visible: root.onAirSlide.refusedByStyle === true

        Row {
            id: gateRow
            anchors.centerIn: parent
            spacing: 4

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: Theme.warning
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Style blocks %1").arg(root.familyLabel(root.onAirSlide.contentType))
                color: "#9aa0b5"
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }
        }
    }

    readonly property bool feedStalled: {
        void LiveOutputService.feedHealthRev
        return LiveOutputService.live && root.isEnabled
            && LiveOutputService.outputFeedStalled(root.name)
    }

    Rectangle {
        objectName: "selfTestTileStallChip"
        anchors.top: previewPane.top
        anchors.topMargin: 4
        anchors.left: previewPane.left
        anchors.leftMargin: 4
        width: stallRow.width + 12
        height: stallRow.height + 6
        radius: 9
        visible: root.feedStalled
        color: "#cc2b0d0d"

        Row {
            id: stallRow
            anchors.centerIn: parent
            spacing: 5

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: Theme.dangerLight
                SequentialAnimation on opacity {
                    running: root.feedStalled
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.25; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("No live feed")
                color: Theme.dangerLight
                font.family: Theme.fontFamily
                font.pixelSize: 10
                font.weight: Font.DemiBold
                textFormat: Text.PlainText
            }
        }
    }

    // ---- THIS TILE'S OWN RUNTIME, AS A COLOUR ---------------------------------
    OutputRuntimeDot {
        anchors.fill: previewPane
        id: runtimeIndicator
        objectName: "selfTestTileRuntime"
        outputName: root.name
        outputEnabled: root.isEnabled
        showState: false
        showDot: false
        hoverPadding: 6
    }
    readonly property string runtimeState: runtimeIndicator.state
    readonly property color runtimeDot: runtimeIndicator.dotColor

    Rectangle {
        anchors.top: previewPane.top
        anchors.topMargin: 4
        anchors.right: previewPane.right
        anchors.rightMargin: 4
        width: markerRow.width + 12
        height: markerRow.height + 6
        radius: 9
        visible: root.inputTaken || root.mediaOnAir
        color: "#b3101219"

        Row {
            id: markerRow
            anchors.centerIn: parent
            spacing: 4

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 16; height: 16; radius: 8
                color: Theme.armedReadyBg
                visible: root.mediaOnAir
                IconGlyph {
                    anchors.centerIn: parent
                    name: "play"
                    color: Theme.armedReadyInk
                    fit: true
                    width: 8; height: 8
                }
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: Theme.accent
                visible: root.inputTaken
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 110)
                elide: Text.ElideRight
                color: "#9aa0b5"
                font.family: Theme.fontFamily
                font.pixelSize: 10
                text: root.mediaOnAir ? LiveOutputService.mediaName : LiveOutputService.inputLabel
            }
        }
    }
}
