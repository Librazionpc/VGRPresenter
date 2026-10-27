import QtQuick
import VGRPresenterUI

// One output-monitor tile — the card in the Show screen's wall and the Edit
// screen's ITEMS tab, driven by OutputListModel in both places. The tile
// shows TRUTH: the on-air slide's design blocks (the SAME { blocks, background }
// shape the ReferencePane preview renders, served by LiveOutputService.onAirSlide)
// drawn through DesignPreview, OR the distributed frame when the content is
// media-like (a camera/media block draws only as a placeholder tile, so the
// real pixels win), OR the checkerboard "nothing" texture when off air.
//
// SCALABLE: the tile fills whatever width its wall assigns; internals are
// anchored; the preview pane holds a true 16:9. Height derives from width.
Rectangle {
    id: root

    // OutputListModel roles
    required property string name
    required property string badge
    required property string styleId
    required property bool active
    required property bool isEnabled
    // The model object itself — this tile's row of the output roster. The pane
    // does NOT read a style background role from it any more (the composition
    // answers the style's look), but the row is still what says which output
    // this tile IS: its name, kind and buffer roles.
    required property var model

    // The distributed frame (image://livepreview re-fetched on every frameRev
    // bump). An output wearing a STYLE reads ITS OWN gated buffer
    // (frameBuffer role → its per-output pass under its own content rules);
    // an unstyled output mirrors the shared PREVIEW feed (single-pipeline
    // software renderer: the frame the preview output received IS what the
    // real outputs got). frameRev pulses on every main-pass frame; the
    // per-output passes run right after it in the same loop iteration, so
    // one rev serves both.
    readonly property string ownBuffer: (root.model !== null && root.model !== undefined)
                                        ? String(root.model.frameBuffer ?? "") : ""
    // A FRAME EXISTS OFF AIR TOO. This used to read `LiveOutputService.live &&
    // frameRev > 0`, which made GO LIVE the switch that revealed the picture:
    // double-click a media file and the tile kept drawing the style's own
    // background until the live loop started publishing. GO LIVE's job is to
    // copy the preview to the CONFIGURED OUTPUTS (NDI/HDMI/windows) — it
    // decides nothing about this tile. The engine's idle preview publishes the
    // compositor's picture while nothing is on air (a taken media file is the
    // one layer this tile no longer paints itself, see the note further down),
    // so the take is the frame as soon as it is taken. A taken camera/screen
    // keeps its own tap layer (the Image below outranks the frame), and with
    // no take at all the tile stays on its QML design composition — those
    // states are unchanged.
    readonly property bool hasFrame: LiveOutputService.frameRev > 0
                                     && (LiveOutputService.live || root.mediaOnAir)
    readonly property url frameSource: {
        if (!hasFrame)
            return ""
        // OFF AIR there is no per-output pass (those run on the live loop), so a
        // styled output's own buffer holds nothing — read the SHARED preview feed,
        // which the idle preview publishes for exactly this: seeing the take
        // before it goes live. While LIVE, the per-output buffer is the gated
        // picture this output really sends.
        return (LiveOutputService.live && ownBuffer !== "")
            ? "image://livepreview/" + ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev
    }

    // The on-air slide AS DESIGN BLOCKS — the same data the preview pane's
    // DesignPreview draws, straight from the runtime's current slide (no
    // re-resolving the on-air title through a service, so every content kind
    // works: scripture, The Table, shows). A STAGED pick (a click in
    // ShowCenter/the project sidebar) always wins here, live or not — it's
    // "what I most recently picked," and clicking a NEW song while
    // something is already live must still update this tile (reported
    // live as "not responsive": staging while live used to be invisible,
    // since this only ever fell back to stagedSlide while NOT live —
    // staging a second pick after GO LIVE updated stagedSlide correctly,
    // the tile just never looked at it in that state). goLive() clears the
    // stage the instant it commits one, so right after a real go-live this
    // still reads as onAirSlide with no visual jump — it only diverges once
    // something NEW gets staged on top of an already-live show. This
    // tile's own hasFrame/frameSource stay gated on LiveOutputService.live
    // regardless, so a real bound-screen output window never sees a
    // staged-only pick, only this in-app tile does.
    // ---- THE ONE COMPOSITION THIS TILE DRAWS FROM --------------------------
    // Everything the pane below renders — the layout that owns the slide, the
    // background the frame paints, whether the style's image is stepped aside,
    // whether this style paints anything of its own at all, whether the gates
    // refuse the family — is answered by ONE call (LiveOutputService
    // .previewWithStyle -> SceneBuilder::ComposeSlide), which is the same
    // composition the engine builds every real output's scene from.
    //
    // ONE CALL, ALWAYS, including when there is nothing staged or on air: an
    // empty slide still answers the STYLE's own facts, which is why the tile no
    // longer needs a second source (the model's style role) for its pane colour,
    // its checkerboard gate or its style image. Two sources was the whole
    // problem this property exists to end: the tile and the output window each
    // derived those answers for themselves and drifted from the frame on air.
    //
    // WHICH SLIDE: a staged local pick belongs to each output tile (that is the
    // tile's own preview), otherwise the on-air slide. THIS OUTPUT'S style id is
    // always the style the composition is asked for — never the active output's,
    // or a tile would paint another output's look.
    readonly property var onAirSlide: {
        void root.stylePulse
        // THE TAKE IS AN INPUT OF THE COMPOSITION. SceneBuilder::ComposeSlide
        // reads CompositorState — is a file/camera up, is its picture
        // transparent, does a take clear the style's background — so a take
        // CHANGES what this slide composes to, and none of that is a
        // QML-notifiable property. The binding used to re-run only on a slide or
        // style change, so after a take the tile kept drawing the PRE-take
        // answer (clearedByTake=false, styleImagePainted=true,
        // styleColor=<the style's poster>) until GO LIVE replaced the slide:
        // one tile painted checkers where its neighbour painted its card
        // colour, for the same picture on the same slide.
        void LiveOutputService.mediaOnAir
        void LiveOutputService.mediaRev
        void LiveOutputService.inputLabel
        // ...and the PAGE the walk is on. A page commit hands this tile the next
        // page's blocks, so this binding has to re-run when that happens — see
        // the memo key below for why it is the PAGE and not the offset.
        void LiveOutputService.readingPage
        const stagedRaw = LiveOutputService.stagedSlideRaw
        const raw = (stagedRaw && Object.keys(stagedRaw).length > 0)
            ? stagedRaw : LiveOutputService.onAirSlideRaw
        // PINNED PER PAGE WHILE A READING WALK RUNS — not per walk. A page commit
        // hands over the next page's blocks, and recomposing them on every one of
        // the ~20 block-list assignments inside a page turn (engine.log, 13 ms) is
        // the interface-thread storm this memo exists to absorb. So the key holds
        // the composition still for every step INSIDE a page, and lets it through
        // when the page itself changes (`readingPage`).
        //
        // WHY NOT THE WHOLE WALK, which is what it used to be: the reading block
        // draws the service's passage at `readingScroll`, so it always showed the
        // current words — but every OTHER block of the page was frozen at the page
        // the walk STARTED on. On a pick longer than the run's window the passage
        // then slid out from under it, and this tile (the surface the operator
        // actually watches) kept drawing that first page statically while the
        // engine and the preview had moved on: the wall's paragraph number, its
        // reference and its words all read as the old page — reported as "the
        // metadata is not updating the para values" and "the left is still showing
        // while the right has finished". The boundary landing is already seamless
        // without the pin (that is what the run's own base arithmetic is for — the
        // words at the top of the box do not move), so a per-page recompose costs
        // one composition per page turn and nothing at all per step.
        const walking = raw && typeof raw === "object"
            && String(raw.flowText ?? "") !== ""
        const key = walking
            ? "walk|" + root.styleId + "|" + root.stylePulse + "|"
              + LiveOutputService.readingSetName + "|" + LiveOutputService.readingPage
              + "|" + LiveOutputService.live
              + "|" + LiveOutputService.mediaOnAir + "|" + LiveOutputService.mediaRev
              + "|" + LiveOutputService.inputLabel
            : ""
        const memo = root.slideMemo
        if (key !== "" && key === memo.key)
            return memo.value
        const composed = LiveOutputService.previewWithStyle(
            (raw && typeof raw === "object") ? raw : ({}), root.styleId)
        memo.key = key
        memo.value = composed
        return composed
    }
    // A plain object mutated in place: its IDENTITY never changes, so writing it
    // cannot re-enter the binding above (a property write there would).
    readonly property var slideMemo: ({ key: "", value: {} })
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    // ---- What the feed does NOT bury: the backdrop pass ------------------
    // WHICH of the on-air blocks the ENGINE paints under the compositor's video
    // layer is the engine's own answer (EngineBridge.backdropBlockIndexes →
    // SceneBuilder::PlanBlockLayers), ASKED rather than re-derived here: this
    // tile must order its layers exactly as every real output does, or the
    // operator sees the slab on the tile and the feed on air (the class of
    // report this whole area keeps producing). Those blocks' own PAINT is drawn
    // by the backdrop layer below the taken-input image; their text — and every
    // block the rule left as decoration — stay in the block render above it.
    // ONE ANSWER PER LAYOUT, NOT PER NOTIFICATION. This binding re-runs every
    // time the slide map is replaced, and a page commit replaces it several times
    // in a few milliseconds (engine.log: ~20 block-list assignments inside 13 ms
    // at a page turn). Each evaluation was converting every block back to an
    // engine ContentBlock and re-running SceneBuilder::PlanBlockLayers ON THE
    // INTERFACE THREAD — the storm of that plus the recomposition is what dropped
    // the frame the operator sees as a blink when the preview moves to the next
    // item.
    //
    // The plan reads the LAYOUT (kind, geometry, stacking) and never the words, so
    // a commit that only changes text cannot change the answer: it is memoized by
    // that layout signature. The memo is a plain object mutated in place — its
    // IDENTITY never changes, so writing it cannot invalidate this binding (a
    // property write here would re-enter it).
    readonly property var backdropMemo: ({ key: "", value: [] })
    readonly property var backdropIndexes: {
        const bs = root.onAirSlide && root.onAirSlide.blocks ? root.onAirSlide.blocks : []
        if (bs.length === 0 || typeof EngineBridge === "undefined")
            return []
        let key = ""
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            key += b.kind + "," + b.x + "," + b.y + "," + b.width + "," + b.height + ";"
        }
        const memo = root.backdropMemo
        if (key === memo.key)
            return memo.value
        const plan = EngineBridge.backdropBlockIndexes(bs)
        memo.key = key
        memo.value = plan
        return plan
    }
    // The same list with those blocks MARKED: the marker travels with the block,
    // so the block render can skip exactly their paint and the backdrop pass can
    // draw exactly them, with no parallel index list to keep in step with a list
    // that gets filtered on the way through.
    readonly property var markedBlocks: {
        const bs = root.onAirSlide && root.onAirSlide.blocks ? root.onAirSlide.blocks : []
        const under = root.backdropIndexes
        if (under.length === 0)
            return bs
        const out = []
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            out.push(under.indexOf(i) >= 0 ? Object.assign({}, b, { underVideo: true }) : b)
        }
        return out
    }
    readonly property var backdropBlocks: root.markedBlocks.filter(b => b.underVideo === true)

    // ---- Taken input layer (the Media pane's click-to-preview) ----------
    // The service owns the PAL tap; this tile composites the feed as one
    // more IMAGE layer (the same way the style's PNG/JPG background rides
    // above the base colour), UNDER the on-air content — camera-behind-lyrics.
    // A BLACKOUT SHOWS NOTHING ON THE WALL EITHER. The engine's clear carrier
    // composes an empty frame (no slide, no style backdrop, and no compositor
    // layers — see SceneBuilder::ComposeSlide), but this tile ALSO draws its own
    // QML layers over that frame: the taken input, the style's colour/image, the
    // block preview and the overlay stack. Without this gate the wall would keep
    // showing the camera and the overlays while every real output had gone
    // empty — the one surface the operator checks would lie about the screens.
    readonly property bool blacked: LiveOutputService.blackedOut
    readonly property bool inputTaken: !root.blacked && LiveOutputService.inputLabel !== ""
    // Pointed at the provider THE WHOLE TIME the input is taken — NOT only
    // once frames are live: inputLive flips on the Image's own Ready status,
    // so gating the source on it deadlocked the warm-up (empty URL → no
    // request → no decode → no confirmation → the ring spun forever). The
    // provider answers a not-yet-flowing tap with a 1×1 transparent frame,
    // which reads as "not Ready enough" (sourceSize 1) and keeps the
    // placeholder up until real pixels arrive.
    readonly property url inputSource: inputTaken
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    // First-frame detection lives in the SERVICE (its pump polls the
    // provider's decode cache) — the previous tile-side confirm round-trip
    // (Image status → confirmInputFrame() → inputRev bump → THIS URL) was a
    // binding loop; QML only reads here now.

    // ---- Media-on-air STATE (the Media pane's take-to-program) -----------
    // The tile paints no media layer of its own any more — the engine frame
    // below carries the compositor's media layer, which is what every real
    // output shows (see the note where that layer used to be). This flag
    // survives for the things that are ABOUT the state rather than the
    // picture: the transport readout, the style background's clear-on-text
    // rule, and the pane's own badges. The two holds are mutually exclusive
    // (taking media clears a taken input and vice versa, service-side), so a
    // taken input still outranks it here.
    readonly property bool mediaOnAir: !root.blacked && LiveOutputService.mediaOnAir
                                       && !root.inputTaken

    // A style that gains an image on disk WITHOUT a model mutation (a pick in
    // the Styles dialog before Save, styles hydrating after boot) still has to
    // repaint this tile. The composition is a call, so it re-runs only when one
    // of its inputs moves: bumping this counter on the model's style-change
    // signal makes the pane re-ask it — with a fresh QFile::exists() inside, in
    // the composition, where the image's presence is decided.
    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
    }

    // The family's display name, for the style-gate chip below (the same four
    // families the Styles screen's chips and the engine's gates use —
    // SlideRules::ContentType / FamilyTemplateSlot, asked rather than copied; an
    // untagged slide is Shows content, as everywhere else).
    function familyLabel(type) {
        if (type === "media") return qsTr("Media")
        if (type === "scripture") return qsTr("Scripture")
        if (type === "table") return qsTr("Table")
        if (type === "shows" || type === "") return qsTr("Shows")
        return qsTr("content")
    }

    // FreeShow's clearStyleBackgroundOnText: the style opts its OWN
    // background image out of the way once the slide on air CARRIES content —
    // so a style whose image carries its own baked-in text (a branded
    // church-name PNG) doesn't permanently collide with live text painted over
    // it. Only the IMAGE steps aside; the flat colour still shows.
    // The "actually has something on it" half of that sentence was never
    // actually checked — this suppressed the image at ALL times the flag
    // was on, including going live with nothing staged/on air at all
    // (reported live: "it turns blank... it's meant to still show the bg
    // because nothing is staged" — a real go-live-with-nothing should show
    // the branded image same as fully idle, only real content should ever
    // clear it).
    //
    // NO `active` gate here: the ENGINE suppresses this image for EVERY
    // styled output while content is on air (SceneBuilder's
    // clearBackgroundOnText rule), so gating the suppression on the active
    // output alone made non-active outputs wearing the same style render
    // differently — the live "same style, different result" report: the
    // active tile hid the branded image while the others baked it under the
    // slide. Content on air decides, for all of them alike.
    // THE RULE IS THE ENGINE'S ANSWER, not a second derivation here. The tile
    // and the output window each used to work this out for themselves —
    // `clearOnText && (hasSlidePreview || inputTaken || mediaOnAir)` — which is a
    // DIFFERENT content test from the engine's (a slide that CARRIES blocks),
    // and it showed: going live with nothing staged (the blank carrier) has a
    // preview but no content, so the tile hid the branded backdrop while the
    // engine went on painting it. The composition decides, once, and hands the
    // answer over with the slide's preview.
    // THE STYLE'S IMAGE, as the composition answered it: which image to paint and
    // whether it paints at all for the slide that is up. `styleImagePainted`
    // carries the engine's clear-on-text step-aside (and the fresh on-disk check)
    // with it, so there is nothing left here to derive — see the composition
    // property above.

    // A TAKE OWNS THE FRAME, AND THE COMPOSITION SAYS SO. This used to be a
    // pair of local flags ("videoUnderneath" = input taken, "videoLayerUp" =
    // input or media) gating the pane's colour, its style image and the slide's
    // own background — a second answer to a question SceneBuilder already
    // answers, and one that could disagree with the frame: a take whose picture
    // is opaque gets frameTransparent=false and clearedByTake=true, which is
    // exactly what the pane's colour/image checks now read. One answer, no
    // surface-local "is something showing" test left.

    // THE PICTURE'S OWN FILL, as the composition answered it: the taken file's
    // own choice, else contain — ONE shape for a take on every output (the
    // precedence lives in LiveOutputService::slideToVariantMap, beside the map
    // SceneBuilder lays the frame out by). The tile used to re-state that order
    // on its own, so a preview could letterbox what the output cropped.
    readonly property string effectiveFit: root.onAirSlide.mediaFit || "contain"
    readonly property int effectiveFillMode: effectiveFit === "cover" ? Image.PreserveAspectCrop
                                            : effectiveFit === "fill" ? Image.Stretch
                                                                      : Image.PreserveAspectFit

    // Pixel-kind decision: when the on-air slide carries media-like blocks
    // (camera/media/audio/image), DesignPreview could only draw the source's
    // NAME as a placeholder tile — the distributed FRAME is the honest picture
    // for those, so it wins. Text/shape/clock/timer slides draw true in
    // DesignPreview and take the crisp block rendering (the frame can lag a
    // slide change; the blocks cannot).
    // WHICH KINDS those are is the composition's answer, not this tile's: the
    // output window carried a second, identical copy of the kind test, and a
    // rule with two copies is a rule that eventually answers two ways (see
    // SlideRules::NeedsDistributedFrame). `hasFrame` stays this tile's own read —
    // whether a distributed frame exists at all is service state, not the
    // slide's — and the composition has already said there IS a slide to draw.
    readonly property bool framePriority: root.hasFrame
                                           && root.onAirSlide.framePriority === true

    // Caller sets width (or anchors); height follows as border + pane +
    // border (the meters and the taken-input marker OVERLAY the pane — see
    // below). No footer: the pane seats flush under the wall's tab pill.
    implicitWidth: 182
    implicitHeight: 2 + previewPane.height

    // ---- The L/R meter strips --------------------------------------------
    // The strips OVERLAY the preview pane's edges. They are fed by the INPUT
    // taps the app already holds (a Media card, a Settings row) and read only
    // while this output is ON AIR — a tap is not audio anybody is hearing, so
    // off air there is no input audio in the program and the bars rest at
    // silence (see MeterBar.rawLevel).
    //
    // (2026-10) There is no program-mix meter any more. It read
    // EngineBridge.outputLevels, which came from AudioMixer::ProgramLevel — the
    // mixer only published from the WASAPI render callback of setAudioRender,
    // which nothing ever called, so the branch read a flat zero for its whole
    // life and this tile's bars were really driven by the input taps below.
    // `meterThis`/startOutputMeter existed to keep that program meter (and its
    // pump) alive while live, and the pump's ticks were what silently opened a
    // WASAPI capture per routed input — the "go live is opening WASAPI" report.
    // Both are gone; the tile needs no bridge call at all now.
    radius: 8
    border.color: "#2b2e3d"
    border.width: 1
    color: Theme.rowBg
    // Disabled screens dim everywhere — same model, same state.
    opacity: root.isEnabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: 120 } }

    // 16:9 preview pane — flush inside the 1px border, always the right
    // aspect. On air:
    // the on-air slide drawn block-true (DesignPreview — the SAME renderer the
    // ReferencePane preview uses, so it "renders perfectly" by construction),
    // with the distributed frame taking over for media content. The style's
    // own background (colour + image) paints UNDER everything — off air a
    // styled output shows ITS look, not the transparency checkerboard.
    Rectangle {
        id: previewPane
        // Self-test hook (run_style_bg_test.sh): the layer-order check walks
        // this pane's children by name to prove the transparency checkerboard
        // really sits BELOW the media/block layers (so it can never bury a
        // pushed media frame). Names are per-instance, like the tile's own.
        objectName: "selfTestTilePane"
        x: 1
        y: 1
        width: parent.width - 2
        height: width * 9 / 16
        clip: true
        radius: 7
        // The style's colour (the engine's OutputStyleSpec), from the
        // composition: the pane's base. A TAKE CLEARS IT: whatever is showing
        // on the feeds is the picture, and the style's branded colour is not
        // part of it ("the style already said shows should clear bg — change it
        // so anything that wants to show removes the style's bg"). The empty
        // margins around a letterboxed file stay EMPTY, not styled.
        // A TAKE CLEARS IT, answered by the composition itself: clearedByTake
        // zeroes styleColor, so the pane paints exactly what the frame painted.
        // (`videoLayerUp` used to force transparent here — a second decision on
        // top of the composed one.)
        color: root.blacked || root.onAirSlide.styleColor === "" ? "transparent"
                                                                : root.onAirSlide.styleColor

        // TRANSPARENCY CONVENTION: the checkerboard lives at the BOTTOM of this
        // pane's stack — under the style image, the taken input/media layer, the
        // block render and the distributed frame alike — so it can only ever
        // show where NOTHING paints. It used to be drawn by the DesignPreview
        // above those layers (and by a second copy above the block render),
        // which is opaque: with a style whose background is "transparent" (a
        // keyable template) the checkers painted straight over a pushed media /
        // camera frame, so the tile showed transparency where the media was
        // meant to show — while the real output window, which never draws them
        // (showCheckerboard: false), showed the media correctly. Ownership is
        // now the pane's alone, in the one z-order position that cannot hide
        // content; every DesignPreview in here draws with checkers off.
        // Visible wherever the style paints NO background — an explicitly
        // "transparent" one, or one that names no colour at all (both are
        // keyable frames in the engine, see ComposeSlide) — and for an UNSTYLED
        // output's clear slide; never while a video layer underneath IS the
        // picture, exactly as before.
        // THE COMPOSITION'S ANSWER, not a second opinion about it:
        // `frameTransparent` is "this frame is see-through where nothing
        // paints" — a style that CHOSE transparency, a slide that names no
        // background, and a take whose PICTURE carries alpha (SceneBuilder
        // composes all three; an opaque photo or video is NOT transparent, its
        // letterbox margins are the output's empty pixels). Reading the style's
        // own flags instead is what put checkerboard pillarbox bars behind an
        // opaque taken photo, on one tile and not its neighbour.
        Checkerboard {
            objectName: "selfTestTileCheckers"
            anchors.fill: parent
            // A blackout is the transparency convention's own case: nothing is
            // painting, so the tile shows the same "no picture" texture it shows
            // for an empty frame, instead of the style backdrop.
            visible: root.blacked || root.onAirSlide.frameTransparent === true
            // NO local shades or cell size: this used to paint two OPAQUE greys
            // (#3a3c48 / #25262f) at 9px, which made the tile's board a solid
            // pattern of boxes while every other surface showed the app's faint
            // one-half-transparent board — the same slide reading two ways. It
            // is the shared board now, FreeShow's own values, like the previews.
        }

        // The style's background IMAGE above the colour, under the content —
        // the engine paints it behind every frame; the block-only preview
        // cannot, so the tile does (cover-fit, like SceneBuilder's CoverRect).
        // WHETHER it paints (and which file) is the composition's answer.
        Image {
            anchors.fill: parent
            // `styleImagePainted` is the composition's whole answer: the file is
            // on disk AND the engine paints it for this slide (clear-on-text
            // step-aside and a take's clear included). A second take gate here
            // could only disagree with the frame.
            visible: !root.blacked && root.onAirSlide.styleImagePainted === true
            source: root.onAirSlide.styleImagePainted === true
                    ? "file:///" + root.onAirSlide.styleImage : ""
            fillMode: Image.PreserveAspectCrop
        }

        // The TAKEN INPUT's live frames — the camera/screen feed as one more
        // image layer, under the on-air content (camera-behind-lyrics), above
        // the style background. Fill the pane: feeds are 16:9-shaped like the
        // pane itself. Placeholder glyphs from the shared pane's art keep the
        // warm-up seconds honest.
        // THE BACKDROP PASS — the blocks the ENGINE paints under the
        // compositor's video layer, drawn where the engine draws them: under the
        // taken-input layer below, above the style's own colour/image above.
        // Without this the tile painted EVERY on-air block over the feed while
        // the outputs painted the design's backdrops under it, so the tile and
        // the outputs disagreed about what covers a live feed. `paintOnly`
        // because a backdrop block's TEXT belongs above the feed: the block
        // render further down draws it (with these blocks' own boxes skipped
        // there, so one box is never painted on both sides of the feed).
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
            asynchronous: false
        }

        // THE TAKEN MEDIA FILE — painted from the SAME layer the centre Preview
        // pane paints: the service's own decoded frame (image://mediaplay,
        // rev-paced), PreserveAspectFit, so a portrait photo stays portrait and
        // the margins stay empty. That is the picture the operator already trusts
        // in the middle of the window, and "use whatever renders the centre
        // preview for the rest" is exactly right: one decode, one provider, one
        // look here and there. The ENGINE's composed frame still takes over while
        // LIVE (it is what NDI/HDMI actually send), so the tile never claims a
        // picture the outputs do not have.
        Image {
            objectName: "selfTestTileMediaFrame"
            anchors.fill: parent
            visible: root.mediaOnAir && !LiveOutputService.live && !root.inputTaken
            source: root.mediaOnAir ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""
            // THE FILE'S OWN FILL — see effectiveFillMode: a portrait photo the
            // operator marked "cover" is cropped here, not in a bar of style
            // colour the way the output crops it.
            fillMode: root.effectiveFillMode
            cache: false          // every mediaRev IS a new frame
            asynchronous: false   // a sync load cannot blank the tile between revs
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
        // An audio file has no picture, so the tile used to show whatever the
        // slide's own text blocks happened to say (the file's PATH, wrapping
        // across the frame) over an empty background — the operator's whole
        // read of "is the music running" was a file path. This is the centre
        // preview for audio, the way a media player shows a track that has no
        // artwork: the file's NAME, one big play/pause, and the scrubber with
        // elapsed / total underneath. It is drawn only while audio is what's
        // on the wall (mediaIsAudio), so a slide, a video or a taken camera
        // never grows a track card.
        Item {
            id: audioCentre
            objectName: "selfTestTileAudioCentre"
            anchors.fill: parent
            // Above every sibling layer: the slide the engine builds for a
            // taken audio file still carries the file's PATH as a text block,
            // and that block is what used to be the whole "audio" preview.
            // The card is the honest picture of a track, so it must not sit
            // under the path it replaces.
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

                // Scrubber + both times, the same read as the toolbar's own
                // audio strip under the wall (anchors, not fixed widths, so
                // the track is whatever the two time labels leave).
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

        // MEDIA-ON-AIR IS NOT DRAWN HERE ANY MORE. The tile used to paint its
        // own copy of the taken file's frames from image://mediaplay (the
        // service's decoder), while every real output got theirs from the
        // ENGINE's composited frame — two sources for one picture, and the tile
        // showed media the outputs did not have (reported live: "it shows in
        // the monitor preview but not on the output — NDI goes black"). The
        // engine frame below IS the output's own frame (it already carries the
        // media compositor layer), so the tile now shows exactly what every
        // real output shows: if the engine has no media layer, neither does the
        // tile, and the two can never disagree. AUDIO has no frame at all — its
        // own signal is the output tile's L/R meters moving, not a picture.
        // Overlays on air, UNDER-SLIDE group (OverlayLibraryService's own
        // per-design placeUnderSlide flag) — a lower-third-behind-the-text
        // kind of layer: above the video/media layer, below the slide's own
        // text. See LiveOutputService.activeOverlays for the on/off state
        // (multiple can be live, stacked in take order) and the OVER-SLIDE
        // group further down for the (more common) above-text placement.
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

        // Rendered on-air slide: the engine's own blocks through the shared
        // renderer, with its own checkerboard OFF — a clear background here
        // means "the style's colour/image underneath shows through", and the
        // tile paints that (the pane below).
        DesignPreview {
            objectName: "selfTestTileDesignPreview"
            anchors.fill: parent
            visible: !root.blacked && root.hasSlidePreview && !root.framePriority
            // THE READING STEP SLIDES HERE: this IS the picture of what Alt+Down
            // just re-staged (a text slide draws block-true, on air as well as
            // staged), so the words travel one line and settle instead of the
            // tile cutting to the next line on the key press.
            animateLineSteps: true
            // ...AT THIS OUTPUT'S OWN STYLE: the step's position is shared as a
            // character offset, and this tile converts it into the lines THIS
            // style draws (see DesignPreview.stepLine). Naming the style here is
            // what keeps the two output tiles on the same words when their styles
            // wrap differently.
            readingStyleId: String(root.styleId)
            // This IS on-air content, never a template/library card — a
            // bound field with nothing to show renders blank, not the raw
            // "{bind}" placeholder syntax (go-live-with-nothing bakes in
            // the style's own template, whose bound field then has no real
            // slide text to resolve).
            showBindPlaceholders: false
            // Checkers OFF: the pane owns the transparency convention (see the
            // Checkerboard at the bottom of previewPane). This item is drawn
            // ABOVE the taken input/media layer, so any checkerboard it painted
            // was opaque and buried a pushed media frame — the reported "the
            // checkerboard area is where the media is meant to show".
            showCheckerboard: false
            // The style's own image rides the block render (mirrors the
            // engine: the style's colour OR the slide's composed colour,
            // then the style image, then content — a clear slide background
            // leaves the style's colour underneath, exactly as on air).
            // Suppressed the same as the outer style-bg Image above (both
            // paint the identical image — this is DesignPreview's own copy,
            // under its blocks) so a video layer underneath is never hidden
            // by it. The clear-on-text step-aside rides `styleImagePainted`:
            // the composition's answer, not a second test here.
            backgroundImage: root.onAirSlide.styleImagePainted === true
                ? root.onAirSlide.styleImage : ""
            // Marked blocks (the engine's own backdrop answer, above) draw here
            // WITHOUT their own paint: the backdrop pass below the feed already
            // painted it, and painting it again in this pass would put a covered
            // stage's slab back OVER the feed — the whole defect. Their text
            // still draws here, on top, exactly as the engine composes it.
            blocks: root.markedBlocks
            // The slide's composed background — `frameTransparent` is the
            // composition's own "nothing paints here", so a take (or any other
            // frame that owns the picture) arrives already stepped aside.
            background: root.onAirSlide.frameTransparent === true
                        ? "transparent" : (root.onAirSlide.background ?? "transparent")
        }

        // (The transparency checkerboard for an explicitly transparent style —
        // which also lets the distributed frame's alpha-0 pixels read as
        // transparency instead of a black picture — is the pane's, at the BOTTOM
        // of this stack. Nothing between it and a frame/block/text layer, so it
        // can never paint over the picture.)

        // The distributed frame: the ENGINE's own picture for this output, and
        // — since the tile stopped painting media itself (see the
        // "MEDIA-ON-AIR IS NOT DRAWN HERE ANY MORE" note above) — the ONLY
        // layer that carries a taken media file at all. So media on air must
        // NOT hide it: gating this on !mediaOnAir was left behind by the
        // layer it was written for, and with that layer gone the pane had
        // nothing left to draw but the style's own background — the reported
        // "media is on the monitor preview, I press GO LIVE and it goes black",
        // strobing between black and the style's background image as this
        // layer blinked in and out under it.
        // A TAKEN camera/screen input is the one case that still outranks the
        // frame: its tap is the PAL's, drawn by the inputImage layer below, and
        // an identically-fed engine frame would simply cover it.
        Image {
            id: frameImage
            // Self-test hook (run_tile_facts_test.sh): the distributed frame is
            // a layer of its own, and which tiles draw it instead of the taken
            // file's own image is exactly what a two-tile disagreement turns on.
            objectName: "selfTestTileFrame"
            anchors.fill: parent
            visible: !root.blacked && root.hasFrame
                     && (root.framePriority || !root.hasSlidePreview)
                     && !root.inputTaken
                     // OFF AIR a taken file is drawn by the mediaplay layer above
                     // (the same one the centre preview uses); this engine frame is
                     // the LIVE picture, and drawing both would put the engine's
                     // copy over the trusted one.
                     && !(root.mediaOnAir && !LiveOutputService.live)
            source: root.frameSource
            fillMode: Image.Stretch
            // SYNCHRONOUS on purpose: every frameRev bump is a NEW url, and an
            // async load BLANKS the item until the new frame lands — over the
            // style background underneath, one live feed therefore read as a
            // strobe between the picture and the background. The provider hands
            // back an already-decoded, already-scaled in-memory QImage, so the
            // load is a copy; the taken-input layer above already loads its own
            // frames synchronously for the same reason.
            asynchronous: false
            cache: false
            // The provider hands back an ARGB32 of the exact requested size;
            // stretch keeps the mapping 1:1 with the pane.
        }

        // Overlays on air, OVER-SLIDE group — above everything, including
        // the slide's own text (FreeShow's own overlays-above-text order;
        // matches the MonitorWall toolbar's clear-button z-order comment).
        // Same model/lookup as the UNDER-SLIDE group above.
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

        // NO SECOND CHECKERBOARD. There used to be one here — above the whole
        // stack, with its own test ("unstyled, nothing on air, no take") — beside
        // the pane's own at the bottom. Two copies of the transparency
        // convention is two gates for one fact, and this one could paint OVER
        // whatever was already drawn. The pane's checkerboard, gated on the
        // composition's `frameTransparent`, covers every state these conditions
        // described (an empty pane IS a transparent frame) and can never cover
        // content.

        // (The LIVE pill was removed by request — the red border alone marks
        // the active output.)
    }

    // ---- Program-mix meter (FreeShow's own output-preview design) ---------
    // FreeShow's Preview.svelte metres its output the exact same way this tile
    // does (a live "main" tap over the preview), via the SAME AudioMeter.svelte
    // used everywhere else in their app — vertical mode: ONE continuous strip
    // pinned to the pane's right edge, full height, not the old two-strip/
    // discrete-LED design. A "ghost" of the full gradient stays faintly visible
    // at rest (so the scale reads even at silence), the lit portion reveals
    // BOTTOM-UP as level rises, and a peak-hold tick marks the recent high
    // (holds 2s, then eases down — src/.../drawer/audio/AudioMeter.svelte's
    // updateMeterChannel). Overlays the pane; appears only while audio
    // metering is engaged anywhere (an audio card clicked in Media, or the
    // program-mix tap while live).
    // One bar per channel — LEFT edge = left channel (peaks[0]), RIGHT edge
    // = right channel (peaks[1]). A mono source (only one peak published)
    // mirrors that single channel on both sides rather than leaving the
    // second bar dark, so the pair still reads as "L / R either side" per
    // the ask, not as a broken second channel.
    component MeterBar: Item {
        id: bar
        required property int channelIndex
        y: previewPane.y + 4
        width: 4
        height: previewPane.height - 8
        // Was gated on whether a metering tap was running somewhere — meant
        // the whole strip vanished outside GO LIVE instead of sitting at its
        // own "ghost" resting state (which the gradient below already draws for
        // exactly this: a readable scale even at silence). Always present
        // chrome now; it only animates once a real tap feeds rawLevel.
        visible: root.isEnabled

        // THE FILE ON AIR IS A SOURCE TOO. Its audio is played by Qt's own
        // QAudioOutput, which no capture-device tap can see — the take holds a
        // STREAM meter on the file instead (LiveOutputService::applyMediaTake),
        // published into the same inputLevels list under this one reserved
        // label. Read FIRST: while a video or audio file is up, it IS the
        // program's audio, and the strip must move with it (the report "when I
        // played the audio from the media section the monitor out audio levels
        // did not animate").
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
            // ONE source decides this bar: the loudest currently TAPPED input,
            // honoring the source mute, fader, and matching channel gain so a
            // closed L/R side stays dark. A program-mix reading
            // (EngineBridge.outputLevels) used to win here whenever it was
            // non-zero — and it never was: the mixer only published from a
            // render callback nothing ever started, so that branch was taken
            // and returned a flat 0 permanently, which is exactly the "the
            // preview meter never moves" behaviour. Gone with the program mix
            // itself (see the meter note on the tile's root).
            //
            // ONLY WHILE THIS OUTPUT IS ON AIR. A tap is not audio anybody is
            // hearing: the rows in Settings · Audio & Video meter their devices
            // so their strips move while you aim them, and the Media pane's
            // cards hold their own. Reading those here animated the preview
            // with nothing selected and nothing live — "just connecting an
            // audio source made the monitor preview start animating", with no
            // wall-side state that Clear all could release. Off air there is no
            // input audio in the program, so the bars rest at silence.
            if (!LiveOutputService.live)
                return bar.mediaLevel
            const list = EngineBridge.inputLevels
            let loudest = 0
            for (let i = 0; i < list.length; ++i) {
                const snap = list[i]
                if (!snap || snap.peaks === undefined || snap.peaks.length < 1)
                    continue
                // A snapshot with NO device name is not attributable to any
                // input row, so it can only be the machine's default input —
                // never a source this output is showing. Drawing it here put a
                // moving meter on the preview with nothing selected; the bridge
                // refuses those taps now, and this keeps the reading honest if
                // one ever reaches the list again.
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

        // Fast attack (jumps up immediately), slow release — the exact easing
        // AudioMeter.svelte's updateMeterChannel uses, ticked at the same
        // ~30fps (33ms) it throttles its rAF loop to.
        property real smoothed: 0
        property real peakValue: 0
        property real peakHeldAt: 0
        readonly property bool active: rawLevel > 0.01

        // `smoothed`/`peakValue` are raw LINEAR amplitude (0..1) — the WASAPI
        // tap's own domain, kept for the envelope math. The drawn height uses
        // the shared -60..0 dB mapping (Db.dbPct) instead of raw amplitude —
        // one copy of the formula, not three.

        Timer {
            interval: 33
            running: bar.visible
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

        // The gradient every layer below shares — bottom (quiet) = cyan,
        // through green/amber, top (loud) = red. Qt's Vertical orientation
        // puts position 0 at the top, 1 at the bottom, so the stops run in
        // the opposite order CSS's `linear-gradient(0deg, ...)` lists them.
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

        // Lit portion — clipped to the smoothed level, bottom-anchored; the
        // gradient rectangle inside is the FULL strip height so the revealed
        // colors line up with the ghost behind it.
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

    // STYLE GATE CHIP. A style's content gates are a Styles-screen decision
    // (SceneBuilder::StyleAdmitsSlide, mirrored for the preview in
    // LiveOutputService::styleAdmits), and a refused family renders as a
    // background-only frame on EVERY output — including this tile, which
    // composes under the same rule. That is correct behaviour, but a
    // background-only frame is indistinguishable from a broken one, so the
    // tile says which family the style blocks, by name, right on the pane.
    // Sits opposite the input/media marker so the two never overlap.
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

    // STALLED FEED marker — THIS output has stopped receiving frames, so the
    // preview below is not what the audience is seeing: it was blacked out at
    // the device (LiveOutputService hands the output a black frame the moment
    // it stalls, so a frozen picture never reads as a live feed). This chip
    // exists because the tile would otherwise be the one place left showing a
    // picture — the operator needs the tile to say "this one is dead" rather
    // than quietly painting its last good frame. Top-LEFT: the taken-input
    // marker owns the top-right.
    //
    // It reads through feedHealthRev — an invokable alone is opaque to QML's
    // dependency tracking, so the void of the revision is what makes the
    // binding re-run when the service's stall set changes.
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
    // The wall used to speak only when something had already died (the "No live
    // feed" chip): a tile that was working said nothing at all, so "is THIS one
    // actually sending?" could only be answered in Settings · Outputs. It is now
    // a DOT (green = frames landing, amber = up but blacked out or stalled, grey =
    // off air / waiting / switched off) whose hover detail gives the numbers — the
    // frames this output has received and how long ago the last one landed.
    //
    // The dot itself is the shared component (components/OutputRuntimeDot.qml,
    // built on HoverDetail), which the Outputs rows use too: the state, the
    // colour and the tip live in one place instead of being re-derived here.
    // `runtimeState` / `runtimeDot` are kept as read-throughs because the
    // self-tests and the tile-facts diagnostic read them off the tile.
    // The dot AND its state word ("live", "off air", "blackout", "no feed", …)
    // on a dark chip so the word stays readable over video — the numbers are the
    // hover detail, never printed here.
    Rectangle {
        anchors.left: previewPane.left
        anchors.leftMargin: 8           // clear of the left meter strip
        anchors.bottom: previewPane.bottom
        anchors.bottomMargin: 6
        width: runtimeIndicator.implicitWidth + 12
        height: runtimeIndicator.implicitHeight + 6
        radius: 9
        color: "#b3101219"

        OutputRuntimeDot {
            id: runtimeIndicator
            objectName: "selfTestTileRuntime"
            anchors.centerIn: parent
            outputName: root.name
            outputEnabled: root.isEnabled
            showState: true
        }
    }
    readonly property string runtimeState: runtimeIndicator.state
    readonly property color runtimeDot: runtimeIndicator.dotColor

    // Taken-input marker (the old name footer's right side): purple dot /
    // play glyph + label, so the layer's presence reads even when the feed
    // is hidden behind on-air content — a monitor op must see WHY the
    // output shows a video. Overlaid on the pane's top-right on a dark
    // chip, so it stays readable over video; the name footer is gone and
    // the pane seats flush under the wall's tab pill.
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
                // The key the operator has taken, in its own blue wash and ink
                // (Theme.armedReadyBg / armedReadyInk — the pair the flow
                // runner's Run button wears). Both this colour and `visible` key
                // on the same flag, so the circle only ever paints the blue: the
                // violet it used to fall back to was unreachable.
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
