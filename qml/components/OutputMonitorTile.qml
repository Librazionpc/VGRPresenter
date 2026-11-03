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

    readonly property string ownBuffer: (root.model !== null && root.model !== undefined)
                                        ? String(root.model.frameBuffer ?? "") : ""
    readonly property bool hasFrame: LiveOutputService.frameRev > 0
                                     && (LiveOutputService.live || root.mediaOnAir)
    readonly property url frameSource: {
        if (!hasFrame)
            return ""
        return (LiveOutputService.live && ownBuffer !== "")
            ? "image://livepreview/" + ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev
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
        const stagedRaw = LiveOutputService.stagedSlideRaw
        const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
            ? stagedRaw : LiveOutputService.onAirSlideRaw
        const walking = raw && typeof raw === "object"
            && String(raw.flowText ?? "") !== ""
        const key = walking
            ? "walk|" + root.styleId + "|" + root.stylePulse + "|"
              + LiveOutputService.readingSetName + "|" + LiveOutputService.readingStageSerial
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
        function onMediaChanged() { root.refreshOnAirSlide() }
        function onMediaTick() { root.refreshOnAirSlide() }
        function onInputChanged() { root.refreshOnAirSlide() }
        function onReadingPageChanged() { root.refreshOnAirSlide() }
        function onStagedChanged() { root.refreshOnAirSlide() }
        function onOnAirChanged() { root.refreshOnAirSlide() }
        function onLiveChanged() { root.refreshOnAirSlide() }
    }
    Component.onCompleted: root.refreshOnAirSlide()
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0
    readonly property int readingDrawnLines: contentPreview.readingDrawnLines
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
    readonly property url inputSource: inputTaken
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    // ---- Media-on-air STATE (the Media pane's take-to-program) -----------
    readonly property bool mediaOnAir: !root.blacked && LiveOutputService.mediaOnAir
                                       && !root.inputTaken

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
        y: 1
        width: parent.width - 2
        height: width * 9 / 16
        clip: true
        radius: 7
        color: root.blacked || root.onAirSlide.styleColor === "" ? "transparent"
                                                                : root.onAirSlide.styleColor

        Checkerboard {
            objectName: "selfTestTileCheckers"
            anchors.fill: parent
            visible: root.blacked || root.onAirSlide.frameTransparent === true
        }

        Image {
            anchors.fill: parent
            visible: !root.blacked && root.onAirSlide.styleImagePainted === true
            source: root.onAirSlide.styleImagePainted === true
                    ? "file:///" + root.onAirSlide.styleImage : ""
            fillMode: Image.PreserveAspectCrop
        }

        DesignPreview {
            objectName: "selfTestTileBackdropPreview"
            anchors.fill: parent
            visible: !root.blacked && root.hasSlidePreview && !root.framePriority
            showCheckerboard: false
            background: "transparent"
            backgroundImage: ""
            blocks: root.backdropBlocks
            paintOnly: true
        }

        Image {
            id: inputImage
            objectName: "selfTestTileInputFeed"
            anchors.fill: parent
            visible: root.inputTaken
            source: root.inputSource
            fillMode: Image.PreserveAspectCrop
            cache: false   // every rev IS a new frame
            // gui-ok: the camera/NDI tap map the provider reads is interface-thread state (only while an input is taken)
            asynchronous: false
        }

        LiveFrameImage {
            objectName: "selfTestTileMediaFrame"
            anchors.fill: parent
            visible: root.mediaOnAir && !LiveOutputService.live && !root.inputTaken
            source: root.mediaOnAir ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""
            fillMode: root.effectiveFillMode
            sizeToItem: false     // the provider stretches to a requested size; the file's own aspect must survive
            // (every mediaRev IS a new frame; the double buffer is what keeps it from blanking between revs)
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
        }

        DesignPreview {
            id: contentPreview
            objectName: "selfTestTileDesignPreview"
            anchors.fill: parent
            visible: !root.blacked && root.hasSlidePreview && !root.framePriority
            // Only Scroll mode draws the passage flow. In Slide mode the selected
            // page stays intact even if an earlier scroll left flow data on it.
            animateLineSteps: LiveOutputService.scrollMode
            readingStyleId: root.styleId !== undefined && root.styleId !== null
                            ? String(root.styleId) : ""
            showBindPlaceholders: false
            showCheckerboard: false
            backgroundImage: root.onAirSlide.styleImagePainted === true
                ? root.onAirSlide.styleImage : ""
            blocks: root.markedBlocks
            background: root.onAirSlide.frameTransparent === true
                        ? "transparent" : (root.onAirSlide.background ?? "transparent")
        }

        LiveFrameImage {
            id: frameImage
            objectName: "selfTestTileFrame"
            anchors.fill: parent
            visible: !root.blacked && root.hasFrame
                     && (root.framePriority || !root.hasSlidePreview)
                     && !root.inputTaken
                     && !(root.mediaOnAir && !LiveOutputService.live)
            source: root.frameSource
            fillMode: Image.Stretch
            // Loaded off the interface thread and double buffered (LiveFrameImage); the provider hands back
            // the frame already scaled to this tile's size, so stretch keeps the mapping 1:1 with the pane.
        }

        OverlayLayer {
            blacked: root.blacked
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
            if (!LiveOutputService.mediaOnAir)
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
            // A file on air owns the strip (see mediaLevel).
            if (bar.mediaLevel > 0)
                return bar.mediaLevel
            if (!LiveOutputService.live)
                return bar.mediaLevel
            const list = EngineBridge.inputLevels
            let loudest = 0
            for (let i = 0; i < list.length; ++i) {
                const snap = list[i]
                if (!snap || snap.peaks === undefined || snap.peaks.length < 1)
                    continue
                if (String(snap.label || "").trim() === "")
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