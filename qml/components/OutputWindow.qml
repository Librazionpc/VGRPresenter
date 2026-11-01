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

    // Named so a self-test can pick the OUTPUT windows out of the app's
    // top-level windows (SelfTest.windows()) and assert the alpha half of this
    // window's contract: see the one-shot log below.
    objectName: "outputWindow"

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
    // OutputListModel's res role ("1920 × 1080" style, or "" before a
    // screen has ever been bound) — Settings · Outputs · Edit's Resolution
    // picker. A screen-bound row keeps this snapped to the real display's
    // current mode (OutputListModel::setScreenName/screensChangedNow), so in
    // practice this only actually DIVERGES from the display's native size
    // when the user explicitly picks a different one in the edit dialog —
    // which must resize this window, not just relabel the Settings card.
    property string outputRes: ""
    readonly property var parsedRes: {
        const parts = root.outputRes.split("×")
        if (parts.length !== 2)
            return null
        const w = parseInt(parts[0], 10)
        const h = parseInt(parts[1], 10)
        return (w > 0 && h > 0) ? { width: w, height: h } : null
    }

    // Settings > Outputs' own per-output toggle (OutputListModel's
    // stayOnTop role) — off by default. Was a corner pill in this window;
    // moved to the real Output settings since that's where every other
    // per-output setting already lives, not a control floating on the
    // output itself.
    property bool outputStayOnTop: false
    flags: Qt.FramelessWindowHint | (root.outputStayOnTop ? Qt.WindowStaysOnTopHint : 0)

    // THE OUTPUT COMPOSITES WITH ALPHA — ALWAYS, not only while the style
    // happens to paint nothing.
    //
    // A keyable frame is a frame the engine LEFT UNPAINTED: its scene is
    // cleared to alpha 0, no background object at all, and the NDI feed
    // carries that alpha all the way out. What shows through is whatever is
    // keyed or recorded behind this window — the desktop, a downstream keyer,
    // a window capture that keeps alpha — and that is what a keyable overlay
    // MEANS on a real screen. So the window must never add an opaque layer of
    // its own, or the transparency dies at the last hop and the audience screen
    // shows black while the in-app tile (which paints its own transparency
    // convention) looks right — the reported "it only shows on the tile".
    //
    // IT USED TO ASK FIRST (`stylePaintless ? transparent : black`), which is a
    // second decision about paint sitting on top of the one the engine already
    // made. The two can disagree, and then the black wins: a style whose
    // background image is on disk but did not reach the frame (an unreadable
    // file, an output whose buffer has not painted yet, a slide that stepped the
    // image aside while the colour is unnamed) read as an OPAQUE BLACK output,
    // not as a keyable frame. The composition's own answers now drive every
    // layer this window paints (the style colour, the style image, the composed
    // slide background, the distributed frame) and each of them paints exactly
    // when the frame does — so there is nothing left for a backdrop to fill in.
    //
    // Qt turns a window colour with alpha < 255 into a translucent window, so
    // this is also what makes the window's OWN pixels honour alpha; a fully
    // painted frame covers it completely either way (a projector sees no
    // difference), and the very bottom layer stays the alpha-inspection
    // checkerboard when VGR_ALPHA_INSPECT=1 (see below).
    color: "transparent"

    // TRANSPARENCY INSPECTION (VGR_ALPHA_INSPECT=1): draws the shared
    // checkerboard as this window's bottom layer, so wherever the on-air
    // content leaves alpha transparent the checkerboard shows through — a
    // keyed feed's transparency is VISIBLE on the real audience screen instead
    // of just being "the desktop". The engine composites the same checkerboard
    // into the frames it sends to NDI/HDMI (RenderEngine::SetAlphaInspect), so
    // one switch proves alpha on every surface. OFF by default: on a real
    // output a checkerboard reads as a broken feed, not "transparent".
    readonly property bool alphaInspect: (typeof alphaInspectEnabled !== "undefined")
                                         && alphaInspectEnabled === true

    // A stays-on-top borderless window with no dismiss gesture is a trap —
    // especially testing without a dedicated second monitor, where the
    // bound "screen" can be the same one the app itself is on, burying the
    // whole UI under a black window with nothing to click. Escape or a
    // double-click hides it; re-enabling the Output (or re-binding it to a
    // screen) brings it back, since THAT is the real "turn this output back
    // on" gesture — this is just an escape hatch, not a replacement for it.
    // ONE-SHOT, GREPPABLE: what this window does with alpha, said out loud the
    // first time it comes up. The keyed-output contract has two halves — the
    // engine leaves an unpainted frame at alpha 0 (RenderEngine's transparent-
    // background clear, which the preview provider carries byte for byte) and
    // THIS window must not fill that in — and only the second half is invisible
    // to every other diagnostic in the app. Grep the engine log for
    // "OUTPUT-WINDOW alpha" after GO LIVE.
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
    // composition/stylePulse comment for the same reasoning). Also tracks
    // OutputListModel.screensRevision: a hot-plugged/unplugged monitor or a
    // display-mode change re-evaluates this (and the x/y/width/height below)
    // so the window tracks its monitor LIVE instead of only at next bind.
    readonly property var displayInfo: {
        void root.outputScreenName
        void OutputListModel.screensRevision
        return root.outputScreenName !== "" ? OutputListModel.displayFor(root.outputIndex) : ({})
    }
    readonly property bool hasDisplay: root.displayInfo.width !== undefined

    // Always positioned at the bound screen's origin, and sized to cover it
    // — a real projector/HDMI output's actual job, no half-size "test mode"
    // to second-guess — UNLESS the edit dialog's Resolution picker set an
    // explicit size that differs from the display's own (parsedRes), in
    // which case THAT drives width/height instead: picking a resolution
    // must actually resize the real output, not just relabel a Settings
    // card nothing reads.
    readonly property var targetGeometry: root.hasDisplay
        ? { x: root.displayInfo.x, y: root.displayInfo.y,
            width: root.parsedRes ? root.parsedRes.width : root.displayInfo.width,
            height: root.parsedRes ? root.parsedRes.height : root.displayInfo.height }
        : { x: 0, y: 0, width: 480, height: 270 }

    // STARTING geometry only: the window is user-movable (see the
    // DragHandler below) and the OS can also write x/y/width/height back on
    // its own, either of which silently SEVERS a QML binding exactly like an
    // imperative assignment would — so onTargetGeometryChanged below is what
    // actually keeps the window honest across a later resolution change
    // (the Settings picker, the monitor's own mode changing, or a re-plug),
    // not these bindings alone.
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

    // The window only shows while live (see `visible` above), but a go-live with
    // nothing staged still has to show SOMETHING — the style's own branded
    // colour/image, not plain black — which the composition answers for an empty
    // slide too. This counter is only the INVITATION to re-ask it when the style
    // roster moves (a style edit, a re-assignment): the composition reads the
    // roster and the disk itself, nothing here is cached from a role.
    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
        function onDataChanged() { root.stylePulse++ }
    }

    // Whether THIS output row is the app's currently-active one — mirrors
    // OutputMonitorTile.qml's own root.active dependency inside the
    // composition's clear-on-text rule (wired from OutputListModel's ActiveRole
    // by OutputWindowManager.qml, same as every other per-row property here).
    property bool outputActive: false

    // ---- On-air content — the SAME data/priority OutputMonitorTile.qml
    // reads, so this window shows exactly what the tile shows instead of
    // depending on the engine's own separate (and, historically, buggier)
    // per-output distributed frame for text. See that file's own comments
    // for the full reasoning behind each of these.
    readonly property string outputStyleId: {
        void root.stylePulse
        const output = OutputListModel.getOutput(root.outputIndex)
        return output && output.styleId ? String(output.styleId) : ""
    }
    // THE ONE COMPOSITION THIS WINDOW DRAWS FROM — the same call, with the same
    // contract, as OutputMonitorTile.qml's own `onAirSlide`: one composition per
    // surface, always asking for THIS output's style id, and answering the
    // style's own facts even when no slide is up. Nothing in this window derives
    // a preview rule for itself any more (see that file's comment for the whole
    // reasoning — two surfaces deriving the same rule is what made them
    // disagree).
    // IMPERATIVE, NOT A BINDING — same conversion, same reason, as
    // OutputMonitorTile.qml's own onAirSlide: confirmed live, "QML
    // OutputWindow: Binding loop detected for property 'onAirSlide'"
    // (OutputWindow.qml:259, the real output window — the tile's own fix
    // never touched this file). The cascade is identical: this property
    // feeding `blocks: root.onAirSlide.blocks` below, its Repeater, the
    // reading block's eager solveFit, LiveOutputService.noteReadingFit's
    // first-solve reset, stagedChanged, back to this binding — a second
    // write landing while the first was still unwinding through that same
    // chain. The re-entrancy guard (not a defer — see OutputMonitorTile's
    // own note on why a QTimer version was tried and reverted) makes the
    // re-entrant call mark itself pending and return instead of nesting a
    // second write, then runs once more after the first pass's cascade
    // fully returns — still inside one synchronous call, so there is no
    // stale frame to be seen in between.
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
            // THE TAKE IS AN INPUT OF THE COMPOSITION (see OutputMonitorTile
            // .qml's own binding for the full reasoning): SceneBuilder
            // composes differently while a file or camera is up, and none of
            // that is QML-notifiable, so without these the window kept
            // drawing its PRE-take composition until the slide changed.
            const stagedRaw = LiveOutputService.stagedSlideRaw
            const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
                ? stagedRaw : LiveOutputService.onAirSlideRaw
            root.onAirSlide = LiveOutputService.previewWithStyle(
                (raw && typeof raw === "object") ? raw : ({}), root.outputStyleId)
        } while (root.onAirSlidePending)
        root.onAirSlideBusy = false
    }
    onStylePulseChanged: root.refreshOnAirSlide()
    onOutputStyleIdChanged: root.refreshOnAirSlide()
    Connections {
        target: LiveOutputService
        function onMediaChanged() { root.refreshOnAirSlide() }
        function onMediaTick() { root.refreshOnAirSlide() }
        function onInputChanged() { root.refreshOnAirSlide() }
        function onStagedChanged() { root.refreshOnAirSlide() }
        function onOnAirChanged() { root.refreshOnAirSlide() }
    }
    Component.onCompleted: root.refreshOnAirSlide()
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    // A BLACKOUT PAINTS NOTHING IN THIS WINDOW EITHER. The engine's clear
    // carrier composes an empty frame (no slide, no style backdrop, no
    // compositor layers), but this window draws its own QML layers too — the
    // style colour/image, the taken input, the media file and the overlay
    // stack. They all stand down while the blackout is up, so the audience
    // window goes empty (its own colour is transparent, see the window setup)
    // rather than keeping the camera behind a cleared engine frame.
    readonly property bool blacked: LiveOutputService.blackedOut
    readonly property bool inputTaken: !root.blacked && LiveOutputService.inputLabel !== ""
    readonly property url inputSource: inputTaken
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    readonly property bool mediaOnAir: !root.blacked && LiveOutputService.mediaOnAir
                                       && !root.inputTaken
    readonly property url mediaSource: mediaOnAir && !LiveOutputService.mediaIsAudio
        ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""

    // THE STYLE'S OWN FACTS COME FROM THE COMPOSITION (root.onAirSlide.*), the
    // same object the monitor tile draws from and the same call the engine's
    // per-output scene is composed by. This window used to derive all of them
    // for itself off the model's style role — whether the style paints at all
    // (`styled`, tested against its own `isClearColor`), which style image to
    // paint, and the clear-on-text step-aside (a rule the tile derived a second
    // time, on a different idea of "content on air", and the engine a third).
    // Nothing below re-derives a preview rule any more.
    //
    // NO `active` gate on the step-aside either — see OutputMonitorTile.qml's
    // comment for the full reasoning: the ENGINE suppresses that image for EVERY
    // styled output while the slide on air carries content, not just the active
    // one, so gating it on outputActive made a non-active live Output bake the
    // branded image under its slide text on the real audience screen.

    // A TAKE OWNS THE FRAME, and the composition says which parts of the style
    // stop painting for it (clearedByTake zeroes styleColor, styleImagePainted
    // goes false). The window used to keep its own `videoUnderneath` flag and
    // gate both layers on it — a second answer, which is exactly the kind of
    // split this window's own comment above says it removed for every OTHER
    // rule.

    // THE PICTURE'S OWN FILL, as the composition answered it (the taken file's
    // own choice, else contain): one precedence, in LiveOutputService, beside
    // the map SceneBuilder lays the frame out by.
    readonly property string effectiveFit: root.onAirSlide.mediaFit || "contain"
    readonly property int effectiveFillMode: effectiveFit === "cover" ? Image.PreserveAspectCrop
                                            : effectiveFit === "fill" ? Image.Stretch
                                                                      : Image.PreserveAspectFit

    // Media-kind on-air blocks (camera/media/audio/image) draw as a
    // placeholder-only tile through DesignPreview — the distributed frame
    // is the honest picture for those specifically; everything else (text/
    // shape/clock/timer) draws true through DesignPreview and wins, since
    // that frame can lag a slide change while the blocks never do.
    // WHICH kinds those are comes from the composition (the SAME call and the
    // same answer OutputMonitorTile.qml reads), which is what makes this window
    // and the tile agree on every slide by construction — the kind test used to
    // be written out twice, once per surface.
    readonly property bool framePriority: root.hasFrame
                                           && root.onAirSlide.framePriority === true

    // Bottom layer: only in inspection mode, and only where nothing opaque
    // covers it (the style background / input / media / content layers below
    // all paint over it where they are opaque).
    Checkerboard {
        anchors.fill: parent
        visible: root.alphaInspect
        // THE SAME BOARD AS EVERYWHERE ELSE (FreeShow draws its one .checkered
        // behind a full-screen output preview too). It was 64px cells here to
        // "read faster" on a physical screen, which instead made the output's
        // empty pixels look like a different product from the preview's; the
        // board's own maxCells guard coarsens it if a 4K surface ever needs it.
    }

    // THE STYLE'S COLOUR — as the composition answered it. A take owns the frame
    // and the composition has already zeroed styleColor for it (clearedByTake),
    // so there is nothing left for a local "is a take up" test to add.
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
        fillMode: root.effectiveFillMode   // the file's own fill, else this output's
        cache: false
        asynchronous: false
    }

    // Overlays, UNDER-SLIDE group — same model/lookup as
    // OutputMonitorTile.qml.
    Repeater {
        model: root.blacked ? [] : LiveOutputService.activeOverlays
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
        visible: !root.blacked && root.hasSlidePreview && !root.framePriority
        // Only Scroll mode draws the passage flow, matching the engine's
        // Slide::flowText contract. Slide mode renders the selected page whole.
        animateLineSteps: LiveOutputService.scrollMode
        // ...AT THIS OUTPUT'S OWN STYLE, the same conversion the monitor tile
        // makes: the shared position is a character offset, and the line it names
        // is this style's own (see DesignPreview.stepLine). The engine raster of
        // this very output does the same arithmetic in C++.
        readingStyleId: root.outputStyleId !== undefined && root.outputStyleId !== null
                        ? String(root.outputStyleId) : ""
        showBindPlaceholders: false
        showCheckerboard: false
        backgroundImage: root.onAirSlide.styleImagePainted === true
            ? root.onAirSlide.styleImage : ""
        blocks: root.onAirSlide.blocks ?? []
        // THE COMPOSITION'S ANSWER, not this window's own idea of what a take
        // means: "is this frame transparent where nothing paints?" is composed
        // once (SceneBuilder::SlideComposition::frameTransparent) and every
        // surface reads it, so the audience screen, the monitor tile and the
        // Show Center preview cannot paint different backgrounds for one frame.
        background: root.onAirSlide.frameTransparent === true
                    ? "transparent" : (root.onAirSlide.background ?? "transparent")
    }

    // The distributed frame — wins only for media-kind on-air content (a
    // block render would just be a name-on-a-tile placeholder), or when
    // there's no slide preview at all but the service still has a frame
    // (camera/screen taken with nothing else on air). Never over a taken
    // input/media layer, which already IS the real picture.
    Image {
        anchors.fill: parent
        visible: !root.blacked && root.hasFrame
                 && (root.framePriority && !root.mediaOnAir && !root.inputTaken
                     || (!root.hasSlidePreview && !root.mediaOnAir && !root.inputTaken))
        source: root.frameSource
        fillMode: Image.Stretch
        asynchronous: true
        cache: false
    }

    // Overlays, OVER-SLIDE group — above everything, including the slide's
    // own text (FreeShow's own overlays-above-text order).
    Repeater {
        model: root.blacked ? [] : LiveOutputService.activeOverlays
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
