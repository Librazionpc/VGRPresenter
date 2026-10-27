import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// Draws a DESIGN - an overlay or a template - from the BLOCKS the ENGINE keeps for it
// (DesignLibraryService.designs(): { key, kind, text, x, y, width, height, bind, meta, style }).
//
// The blocks are in the Edit screen's stage units (StageGrid.w x StageGrid.h, 754 x 428); this item lays the stage out at its own
// width (set the width and the height follows). They may be plain objects (a library card) or the Edit screen's live
// CanvasItem objects (a slide thumbnail) - either way every property is read through bindings, so a live item re-draws here
// the moment it changes, without the list being rebuilt. It draws the same kinds the Edit screen draws, with the
// same meta keys, so a card preview and the canvas agree:
//   text ..... meta { fontFamily, fontSize, bold, italic, underline, strikethrough, align, color, autoSize }
//   shape .... meta { shapeType } rectangle | circle | line | triangle | arrow | star | hexagon
//   clock .... meta { format, showSeconds, showDate, style: "analog" }
//   timer .... meta { mode, durationSeconds }
//   camera / media / audio ... the canvas's own tile: the source's name (block text) over the kind's icon
//   vignette . the screen's edges tinted: style.backgroundColor = the tint, meta.inset = how far in
//   corners .. the four screen corners cut round: style.backgroundColor = the colour, meta.inset = radius
// A template's bound block (bind set) shows the name of the field that feeds it, so the preview reads
// as a layout rather than empty text.
//
// It is ONE renderer for every small picture of content in the app: the library cards, and the Edit screen's slide list
// (the LIVE thumbnails - SlideCanvasStore.blocksOf() re-reads the slide whenever anything on it changes). It follows the
// canvas's own rules for text (weight, case, list, alignment, shrink/grow to fit), clocks and timers (the same LiveClock
// formatting), shapes and transparency, so what the thumbnail shows is what the slide is.
Item {
    id: root

    property var blocks: []
    property string background: "transparent"
    // LAYER SPLIT FOR A LIVE FEED (the monitor tile's use). WHICH blocks paint
    // under a taken camera/screen feed is the ENGINE's rule, not this item's
    // (SceneBuilder::PlanBlockLayers, asked through
    // EngineBridge.backdropBlockIndexes) — a block it puts there arrives here
    // MARKED (block.underVideo === true), so the marker travels with the block
    // and no surface has to index into a list it also filters. This item then
    // only knows how to DRAW the two passes that marker implies:
    //   * paintOnly: draw this pass's item PAINTS only, no item content — the
    //     pass below the feed, which is under the text as well as the video.
    //   * blocks marked underVideo are drawn WITHOUT their own paint (a text
    //     block's box, a shape's fill) in every normal pass, because their paint
    //     is below the feed while their text stays above it.
    property bool paintOnly: false
    // A background IMAGE painted over the colour, under the blocks — the
    // engine's own order (SceneBuilder: background colour object, then the
    // style image cover-fit, then content). Hosts pass the STYLE's image
    // here so a preview mirrors what the output renders behind the blocks;
    // empty = none (library cards, canvas thumbnails).
    property string backgroundImage: ""
    // The transparency checkerboard behind a clear background. The output
    // monitor tile turns it OFF: there the pane under the preview paints the
    // OUTPUT STYLE's colour/image, and checkers would cover it (a styled
    // output must never read as "transparent/unstyled").
    property bool showCheckerboard: true
    // What time the clocks show; the host ticks it (one shared ticker rather than one per card).
    property date now: new Date()
    // Checkerboard CELL size in SCREEN pixels — FreeShow's fixed 13px board at
    // every preview size. The Checkerboard is handed this preview's own `unit`
    // as its hostScale, so the cell lands at this many real pixels whatever the
    // card's size (see the Checkerboard call below).
    property real checkerSize: 13
    // A bound text block with no literal text shows "{bind}" as a stand-in —
    // correct for a library card or the Edit canvas (it reads as "this is a
    // template field," not broken text), wrong for REAL on-air content (the
    // Main Output tile, a real bound-screen output window): going live with
    // nothing queued composited the style's own template, whose bound field
    // had nothing to resolve, and the raw placeholder syntax leaked onto the
    // live picture as literal text. Hosts rendering actual on-air/staged
    // content turn this off; every editor/library surface keeps it on.
    property bool showBindPlaceholders: true

    // ---- THE READING STEP (LiveOutputService.lineStep) ----------------------
    // A host that draws the stepped content itself (the Main Output tile) opts
    // in: its reading block then paints the PASSAGE the page is cut from and
    // slides it by LiveOutputService.readingScroll — the same passage, at the
    // same offset, that the engine draws on the HDMI window and the NDI feed.
    // Opt-in because every other host — a library card, a canvas thumbnail — has
    // no reading step behind it and must keep drawing the page.
    property bool animateLineSteps: false
    // WHICH STYLE THIS SURFACE DRAWS AT. The reading position itself is a character
    // offset into the passage (LiveOutputService.readingChar), which means the same
    // words everywhere; the LINE that character sits on depends on the style's own
    // type size and wrap — so a host that owns a style (the monitor tile and the
    // output window, one per output) names it here and this file converts. Empty =
    // the reference (active style) grid, for hosts that have no style of their own.
    property string readingStyleId: ""
    // ...and THIS surface's own line for that position: the conversion below, or
    // the reference grid's number when no style is named. Everything that draws or
    // measures the move reads this one value.
    readonly property real stepLine: {
        const ch = LiveOutputService.readingChar
        if (!root.animateLineSteps || root.readingStyleId === "" || ch < 0)
            return LiveOutputService.readingScroll
        return LiveOutputService.readingLineForStyle(root.readingStyleId, ch)
    }
    // ---- WHAT THIS SURFACE REALLY LAID OUT (diagnostic, one line per wrap) ---
    //
    // `stepLine` above is a line index in the SERVICE's table for this style;
    // the column under it is a Qt Text laid out in this box. Those are the same
    // grid only while the size, the box and the wrap agree, and when they do not
    // the drawn words lag the step: the reader reports the output "going slower"
    // than its neighbour and "finishing" with lines still below the box. The
    // number that decides it is the column's OWN line count against the table's,
    // so both are printed together, here, for the style this surface draws at.
    // (Information level, like the ReadRun/LineStep diagnostics beside it.)
    property int readingDrawnLines: -1
    // ...and WHICH box it laid that column out in, in stage units: two surfaces of
    // one style can hold the passage in different blocks (the reading block is the
    // longest TEXT, and a fill-built page can make a different block longest), and
    // the table can only be measured in one of them.
    property real readingDrawnBoxW: 0
    property real readingDrawnBoxH: 0
    function noteReadingDrawn(flow, block) {
        readingDrawnLines = flow.lineCount
        readingDrawnBoxW = block.width
        readingDrawnBoxH = block.height
        if (typeof EngineBridge === "undefined")
            return
        const style = String(root.readingStyleId)
        let table = -1
        let svcSize = -1
        try { table = LiveOutputService.readingRunLineStarts(style).length } catch (e) { table = -2 }
        try { svcSize = LiveOutputService.readingRunSizeForStyle(style) } catch (e) { svcSize = -2 }
        EngineBridge.log("info", "ReadingDrawn",
            "style=" + (style === "" ? "(ref)" : style.slice(0, 8))
            + " drawn=" + flow.lineCount + " table=" + table
            + " flowPx=" + Number(flow.font.pixelSize).toFixed(2)
            + " flowW=" + (flow.width - flow.leftPadding - flow.rightPadding).toFixed(2)
            + " svcSize=" + Number(svcSize).toFixed(2)
            + " box=" + block.width + "x" + block.height
            + " pageFit=" + Number(block.fitAnswer).toFixed(2)
            + " pitch=" + Number(block.linePitch).toFixed(2)
            + " chars=" + flow.text.length)
    }
    // How long that text is, so a delegate can tell whether IT is the reading
    // block. Only that one draws the passage: every text block used to shift by
    // the same pitch, which moved a title by the BODY's line height — a visible
    // hop out of place on the very blocks the step is not supposed to touch.
    // THE READING COLUMN'S TYPE SIZE, HELD ACROSS A PAGE COMMIT.
    //
    // A walk that crosses from 1 into 1-1 commits that page: its blocks arrive,
    // its delegate is new, and the fit solver answers for ITS text — so the
    // words came back at a slightly different size, laid out from the top of the
    // new page. Reported as "the moment it moves to 1-1 it refreshes and opens
    // 1-1 like I clicked" — the scroll never continues into the next page, it
    // restarts there.
    //
    // One walk is one column, so its type size is one number: the size the first
    // page of the walk solved is reused for every page after it, and the offset
    // (readingScroll) is then the only thing that moves. The size is only
    // re-solved when it genuinely does not fit the new page's box — a longer page
    // still gets the smaller size it needs, rather than being clipped to keep a
    // number constant. Keyed by the design inputs (box + type), so a different
    // template or a resized box starts over.
    property string readingFitKey: ""
    property real readingFitSize: 0
    readonly property int stepTextLength: {
        const bs = root.blocks !== undefined && root.blocks !== null ? root.blocks : []
        let longest = 0
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            if (!b || (b.kind !== undefined && b.kind !== "text"))
                continue
            const length = String(b.text ?? "").length
            if (length > longest)
                longest = length
        }
        return longest
    }
    // WHICH BLOCK OWNS THE PASSAGE — the FIRST of the longest-text blocks.
    //
    // "the longest text" is not unique on a fill-built page: the same paragraph
    // arrives in the content slot AND in a band (measured on a real walk — one
    // tile reported laying the passage out in 754x124 while drawing it in 150x42,
    // 39 lines against 271), and a `length === stepTextLength` test then marks BOTH
    // as the reading block. Two columns scroll in two different boxes, each reports
    // its own size and box to the service, and which one the eye (and the harness)
    // reads flips between them — the reported "this is not how the template is
    // defined" and the un-fixable grid mismatch, because ONE style can only be
    // measured in ONE box. The passage belongs in exactly one box, so the first
    // such block owns it.
    readonly property int readingBlockIndex: {
        const bs = root.blocks !== undefined && root.blocks !== null ? root.blocks : []
        const longest = root.stepTextLength
        if (longest <= 0)
            return -1
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            if (!b || (b.kind !== undefined && b.kind !== "text"))
                continue
            if (String(b.text ?? "").length === longest)
                return i
        }
        return -1
    }
    // NO CLOCK AND NO OVERLAY HERE ANY MORE. The move used to be played by a
    // second copy of the words on top of the picture (an old-window snapshot
    // translated by `blockShift`, cross-faded at the end), which is exactly what
    // read as a slide with a snap: two snapshots of a re-flowed string can only
    // be animated to LOOK like a scroll. The offset now comes from the service,
    // animated once, and the block's own passage is slid under its own box (see
    // el.drawsFlow below) — the words on screen are the words that exist, and
    // when the move lands there is nothing to hand over.

    // The stage's grid, from its ONE definition (qml/settings/StageGrid.qml) —
    // this item draws a picture OF THE STAGE, so it follows the stage's own
    // proportion (StageGrid's rule 2), never the frame's axis-by-axis mapping.
    readonly property real stageW: StageGrid.w
    readonly property real stageH: StageGrid.h
    readonly property real unit: width / StageGrid.w

    implicitHeight: StageGrid.heightForWidth(width)
    clip: true

    // The canvas's clock/timer formatting, shared (this one never ticks; the host feeds `now`).
    LiveClock { id: fmt; running: false }
    // SCROLL-COST PROBE (VGR_LAYOUT_TEST, tools/selftest/run_layout_probe.sh): every text block
    // that runs its fit solver reports it to `probeSink`, the library pane hosting the card. One
    // solve is eleven bisection steps, each re-measuring the invisible twin Text's layout — the
    // single most expensive thing building a card does — so a scroll that builds six cards of
    // three text blocks each reports +198 solves into the pane's `gridFitSolves`, and that is
    // what the templates grid pays per newly-revealed card.
    //
    // The counter lives on the PANE, not on this preview: a card is destroyed and recreated by
    // every filter and every scroll (that is the very cost being measured), so a counter on the
    // preview would fall to zero with each recycled card and a delta over a step could come out
    // NEGATIVE. null (every other host in the app) simply counts nowhere.
    property var probeSink: null
    // Fully transparent? Works for "transparent", "#00000000" (what a live colour reads back as) and real colours alike.
    function isClear(c) { return c === undefined || c === null || c === "" || Qt.color(c).a === 0 }

    function cornersPath(w, h, r) {
        return "M 0 0 L " + r + " 0 A " + r + " " + r + " 0 0 0 0 " + r + " Z "
             + "M " + w + " 0 L " + w + " " + r + " A " + r + " " + r + " 0 0 0 " + (w - r) + " 0 Z "
             + "M " + w + " " + h + " L " + (w - r) + " " + h + " A " + r + " " + r + " 0 0 0 " + w + " " + (h - r) + " Z "
             + "M 0 " + h + " L 0 " + (h - r) + " A " + r + " " + r + " 0 0 0 " + r + " " + h + " Z"
    }

    // The stage, shrunk to fit. A transparent background shows the shared
    // transparency checkerboard (the Edit canvas's convention), anything else
    // paints its colour.
    Item {
        width: root.stageW; height: root.stageH
        scale: root.unit
        transformOrigin: Item.TopLeft

        Checkerboard {
            anchors.fill: parent
            // This subtree is painted in STAGE units and scaled by `unit`, so the
            // board is told that scale: it keeps the cell at checkerSize real
            // pixels and rasterizes at the size the card actually covers on
            // screen — not at 754×428 stage units for a 120px card, which is
            // what every design card in a grid used to do.
            tileSize: root.checkerSize
            hostScale: root.unit
            visible: root.showCheckerboard && root.isClear(root.background)
        }
        Rectangle {
            anchors.fill: parent
            visible: !root.isClear(root.background)
            color: root.background
        }
        // The style's background IMAGE — over the colour, under every block,
        // cover-fit (SceneBuilder's CoverRect). Only when the image is set:
        // a slide's own composed colour stays visible through a transparent
        // image slot.
        Image {
            anchors.fill: parent
            visible: root.backgroundImage !== ""
            source: root.backgroundImage !== "" ? "file:///" + root.backgroundImage : ""
            fillMode: Image.PreserveAspectCrop
        }

        Repeater {
            // THE SLOT LIST, NOT THE CONTENT LIST.
            //
            // `model: root.blocks` made every block a BRAND-NEW delegate whenever
            // the array was replaced — and the array is replaced on every selector
            // move, every re-stage, every style edit. Between the destroy and the
            // create there is a frame with no delegate at all (and a new one's fit
            // not yet solved), and THAT is the blink: "the moment the selector
            // moves down it will blink". The picture was not changing size, it was
            // being torn down and rebuilt.
            //
            // The model is the SLOT COUNT, and each delegate reads its block by
            // `index` instead (el.modelData below). A change that keeps the count —
            // the common case: same template, new words, a moved reference — now
            // updates the delegates that already exist, in place. Only a block
            // actually appearing or disappearing (a heading neither page has) still
            // rebuilds, which is the one case where a new node is genuinely needed.
            model: root.blocks ? root.blocks.length : 0

            delegate: Item {
                id: el
                required property int index
                // The block this slot currently holds. A binding, not the model
                // item: that is what lets the same delegate be re-used for the
                // next page's block at this index (see the model comment).
                readonly property var modelData: (root.blocks && index < root.blocks.length)
                                                 ? root.blocks[index] : ({})
                readonly property string kind: modelData.kind
                readonly property var meta: modelData.meta !== undefined ? modelData.meta : {}
                readonly property var style: modelData.style !== undefined ? modelData.style : {}
                // The line advance THIS block paints at, measured from its own
                // fitted size (measureLineBox is the font's line box at
                // fitProbe = fitAnswer; fitMeasure.lineHeight is the multiplier
                // with the additive gap already folded in) — the distance one
                // line of the reading step travels, in this block's own type.
                readonly property real linePitch: measureLineBox.contentHeight * fitMeasure.lineHeight
                // THE READING BLOCK: the longest text (the same rule the service
                // steps and the engine glides) — the only block the line step moves.
                readonly property bool readingBlock: el.kind === "text"
                    && String(modelData.text ?? "").length > 0
                    && String(modelData.text ?? "").length === root.stepTextLength
                    && el.index === root.readingBlockIndex
                // DOES THIS BLOCK DRAW THE READING PASSAGE? Only the reading
                // block of a host that follows the step (the Main Output tile),
                // and only while the service has a passage for it. Everything
                // else about this file draws the page it always drew.
                // DOES THIS BLOCK DRAW THE READING PASSAGE? Only the reading
                // block of a host that follows the step (the Main Output tile),
                // and only while the service has a passage for it. Everything
                // else about this file draws the page it always drew.
                //
                // ...and only if the passage really holds THIS block's page: a
                // flow left over from another slide would put the wrong words in
                // the box. It is a CONTAINMENT test, not a prefix test, because the
                // passage is now a RUN of pages around the one being read (see
                // LiveOutputService's reading run): the current page can sit
                // anywhere inside it, and the offset that positions the column is
                // measured from the run's start, so whichever page the block holds,
                // the line at the top of the box is the line being read.
                readonly property bool drawsFlow: el.readingBlock && el.kind === "text"
                    && !root.paintOnly && root.animateLineSteps
                    && String(el.modelData.text ?? "") !== ""
                    && String(LiveOutputService.readingFlowText ?? "")
                           .indexOf(String(el.modelData.text ?? "")) >= 0
                // ONE-SHOT DIAGNOSTIC: every TRANSITION of this gate, named —
                // this is the EXACT moment the visible surface hands off
                // between the animated flow (flowText) and the plain static
                // page (fitText), on a NEW delegate every page-boundary
                // commit (the Repeater rebuilds when root.blocks changes).
                // false->true->false in quick succession, or landing on
                // false right after a commit, is what a cut/refresh at the
                // boundary would look like from here (user report, A/B
                // tested and confirmed independent of ReferencePane's own
                // selector: "it's still refreshing... not continuing the
                // flow").
                onDrawsFlowChanged: EngineBridge.log("info", "DrawsFlowChanged",
                    "drawsFlow=" + el.drawsFlow
                    + " readingBlock=" + el.readingBlock
                    + " modelText=" + String(el.modelData.text ?? "").slice(0, 24)
                    + " flowText=" + String(LiveOutputService.readingFlowText ?? "").slice(0, 24)
                    + " fitAnswer=" + el.fitAnswer)
                // THE PAGE'S TOP EDGE — where the page's first line sits, from
                // the block's own vertical alignment. The passage begins exactly
                // there, so at offset 0 it draws where the page drew.
                readonly property real pageTop: {
                    const pad = el.style.padding ?? 0
                    const v = el.meta.verticalAlign ?? "center"
                    const h = fitMeasure.contentHeight
                    if (v === "top")
                        return pad
                    if (v === "bottom")
                        return el.height - pad - h
                    return (el.height - h) / 2
                }
                readonly property bool screenWide: kind === "vignette" || kind === "corners"
                // The box behind a block the canvas draws one for: a text or a clock (a fill of nothing is just its border).
                // (Shapes, timers and camera / media tiles draw their own; a vignette / corners block is never a box.)
                readonly property bool hasBox: kind === "text" || kind === "clock"
                // Set by the layer that renders a live feed (the monitor tile),
                // from the ENGINE's own answer: this block's paint belongs under
                // the feed, so it is not painted again above it.
                readonly property bool paintUnderFeed: modelData.underVideo === true

                // The Filters card's Opacity (CanvasItemStyle.opacity, round-
                // tripped onto the plain block's style object) — applied here
                // so every consumer of this file (the monitor tile, the
                // output window, design-card thumbnails) fades a block the
                // same way the edit canvas does, from the one shared style
                // field instead of each rendering path re-deriving it.
                opacity: el.style.opacity !== undefined ? el.style.opacity : 1

                x: modelData.x; y: modelData.y
                width: modelData.width !== undefined ? modelData.width : 160
                height: modelData.height !== undefined ? modelData.height : 40
                // A block never paints outside its own box — the Edit canvas clips a
                // bound placeholder for the same reason it is pinned to the item's own
                // type size below: it can be wider than a small box.
                clip: true

                // fill + border (the box the Edit screen draws behind a text or a clock)
                Rectangle {
                    visible: el.hasBox && !el.paintUnderFeed
                    anchors.fill: parent
                    color: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "transparent"
                    radius: el.style.cornerRadius ?? 0
                    border.width: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 0
                    border.color: el.style.borderColor !== undefined ? el.style.borderColor : "#ffffff"
                }

                // --- text (and a bound template field's placeholder) ---
                // Auto-size runs through the SAME explicit fit solver as the edit
                // canvas (EditScreen.qml's recomputeFit): Qt's Text.Fit converged
                // differently in this scaled-down subtree than on the canvas, and
                // grown text could spill below its box on one surface but not the
                // other. A tiny bisection over candidate sizes, re-measuring an
                // invisible twin each step, gives both surfaces the same answer
                // (the largest size whose real laid-out height/width fit the box).
                property real fitProbe: el.meta.fontSize ?? 16
                // The first rendered frame of a committed page already carries
                // the walk's size (see root.readingFitSize): the solver runs
                // immediately for the reading block, but a frame at the fallback
                // size before it would be the same pop the latch exists to stop.
                property real fitAnswer: (el.readingBlock && root.animateLineSteps
                                          && root.readingFitSize > 0
                                          && root.readingFitKey === el.readingFitKey)
                                         ? root.readingFitSize : (el.meta.fontSize ?? 16)
                property bool fitSolving: false
                // What the latched size is keyed by: the BOX and the TYPE, never
                // the text — the text is exactly what changes at a page commit,
                // and re-solving for it is the refresh this whole block prevents.
                // WHAT THE LATCH IS KEYED BY: the BOX and the TYPE — never the text,
                // which is exactly what changes at a page commit.
                //
                // ...and never the PAGE'S OWN FONT SIZE while a walk is drawing this
                // block: a fill-built page (The Table, the Bible) carries its size per
                // page — measured on a real walk: 45.84, 43.90, 42.93 and 40.99 in one
                // passage — and a key that includes it lets every page re-solve the
                // column, which is both the pop this latch exists to stop and a
                // different line table under the reader (the table is measured at ONE
                // size; a column drawn at four is four grids).
                readonly property string readingFitKey: {
                    const parts = [el.width, el.height, el.style.padding ?? 0,
                                   el.meta.fontFamily ?? "",
                                   MetadataFields.lineHeight(el.meta),
                                   MetadataFields.lineSpacing(el.meta),
                                   MetadataFields.letterSpacing(el.meta)]
                    if (!(el.readingBlock && root.animateLineSteps))
                        parts.push(el.meta.fontSize ?? 16)
                    return parts.join("|")
                }
                readonly property var fitInputs: [fitText.text, el.width, el.height, el.fitMode,
                                                  el.meta.fontSize ?? 16,
                                                  MetadataFields.lineHeight(el.meta),
                                                  MetadataFields.lineSpacing(el.meta),
                                                  MetadataFields.letterSpacing(el.meta),
                                                  el.style.padding ?? 0]
                // EVERY text block solves — "none" included: the box is the master and the
                // text must always sit inside it (a "none" block renders at its set size
                // while that fits, and SQUEEZES the moment it doesn't; nothing is ever cut).
                //
                // DEFERRED (Qt.callLater), except for the ONE case where that defer is
                // itself the bug: the reading block on a surface that animates line steps
                // (el.readingBlock && root.animateLineSteps — the SAME pair drawsFlow
                // gates on). A page-boundary commit destroys this delegate and builds a
                // brand new one for the new page; deferred, its FIRST rendered frame uses
                // the un-fitted fallback size (el.meta.fontSize) — not yet the solver's
                // answer — for however long it takes Qt.callLater to run, right as
                // readingScroll also resets to 0 for the new page. Two discontinuities
                // landing in the same frame (a font-size pop on top of the page swap) is
                // what read as "it['s] as if the app clicks" instead of a continued scroll.
                // Solving immediately for just this one case costs one bisection on a page
                // boundary (rare: one per Alt+Down that crosses one) — every other consumer
                // of this file (library cards, canvas thumbnails, grids — see the
                // scroll-cost probe above) keeps the deferred/coalesced solve unchanged.
                onFitInputsChanged: {
                    if (fitSolving)
                        return
                    if (el.readingBlock && root.animateLineSteps)
                        el.solveFit()
                    else
                        Qt.callLater(el.solveFit)
                }
                Component.onCompleted: {
                    if (fitSolving)
                        return
                    if (el.readingBlock && root.animateLineSteps)
                        el.solveFit()
                    else
                        Qt.callLater(el.solveFit)
                }
                function solveFit() {
                    if (fitSolving)
                        return
                    fitSolving = true
                    // (scroll-cost probe: one bisection over the twin Text, per solve. The delegate can
                    // be DESTROYED between the Qt.callLater that queued this and the call — the pane it
                    // reports to goes with it — so `typeof` guards the dead scope, the same way
                    // DropdownPanel's stale-row guards do.)
                    const hostAlive = typeof root !== "undefined" && root !== null && root !== undefined
                    if (hostAlive && root.probeSink)
                        root.probeSink.gridFitSolves++
                    const pad = el.style.padding ?? 0
                    const bw = el.width - pad * 2, bh = el.height - pad * 2
                    let lo = 3, hi = 400
                    // A BOUND PLACEHOLDER IS NOT CONTENT, so the DECLARED size is the
                    // answer in every mode (the Edit canvas's recomputeFit carries the same
                    // rule, with the measurements) — the mode names describe what may happen
                    // to content, and a token is not content. Asking "is this text a
                    // stand-in?" rather than "is the mode grow?" also keeps the rule from
                    // needing two spellings for one idea ("grow" here, "growToFit" there).
                    // The delegate is clipped to its box, so the token is cut at the edge
                    // instead of shrinking the type.
                    const tokenOrZero = bw <= 0 || bh <= 0 || MetadataFields.isToken(fitText.text)
                    // STILL THE SAME WALK: keep the size the first page settled
                    // on, so the committed page lands at the size the reader was
                    // already looking at. Measured here, not assumed — a page that
                    // genuinely does not fit at that size still gets its own
                    // (smaller) answer, and the held size is then only the upper
                    // bound the bisection may come down from.
                    let held = false
                    let readingFitHeld = 0
                    if (!tokenOrZero && hostAlive && el.readingBlock && root.animateLineSteps
                            && root.readingFitSize > 0
                            && root.readingFitKey === el.readingFitKey) {
                        readingFitHeld = root.readingFitSize
                        fitProbe = readingFitHeld
                        // HELD MEANS HELD. The walk's one type size IS the first page's
                        // own solve, and the old "unless this page genuinely does not fit"
                        // escape re-solved a longer page smaller — which re-flows the
                        // column mid-walk, i.e. re-measures the line table under the
                        // reader: the size the table was measured at changed, so the
                        // drawn column and the table became different grids again
                        // (measured: 45.84 → 40.99 across one walk, pageFit against
                        // table mismatches of 3 lines). A longer page's extra lines
                        // simply scroll through the box, which is what a passage is.
                        held = true
                    }
                    if (tokenOrZero) {
                        lo = el.meta.fontSize ?? 16
                    } else if (!held) {
                        for (let i = 0; i < 11; ++i) {
                            const mid = (lo + hi) / 2
                            fitProbe = mid
                            if (fitMeasure.contentWidth <= bw && fitMeasure.contentHeight <= bh)
                                lo = mid
                            else
                                hi = mid
                        }
                    } else {
                        lo = held ? readingFitHeld : (el.meta.fontSize ?? 16)
                    }
                    // grow: fill the box; shrink/none: at most the set size (the squeeze).
                    fitAnswer = el.fitMode === "grow" ? lo : Math.min(el.meta.fontSize ?? 16, lo)
                    fitProbe = fitAnswer
                    if (hostAlive && el.readingBlock && root.animateLineSteps) {
                        root.readingFitKey = el.readingFitKey
                        root.readingFitSize = fitAnswer
                        // THE NUMBER THE PASSAGE'S TABLE MUST BE MEASURED AT, handed to
                        // the service that owns that table. This surface is the one
                        // that DRAWS the column, so its solve is the authority: the
                        // service re-deriving the size from its own solver put the
                        // type 10% larger than the design's page (measured 44.48 px
                        // drawn against a 40 px page) and left the table a different
                        // grid from the words on screen.
                        // ...with the BOX it draws in, because the service's own
                        // composition can pick a different block for the same style
                        // (measured: 150x41 against 754x124 for one output) and a
                        // wrap measured in the wrong box counts different lines.
                        LiveOutputService.noteReadingFit(root.readingStyleId, fitAnswer,
                                                         el.width, el.height,
                                                         el.style.padding ?? 0)
                    }
                    fitSolving = false
                }
                Text {
                    id: fitMeasure
                    visible: false
                    width: el.width - (el.style.padding ?? 0) * 2
                    text: fitText.text
                    font.family: fitText.font.family
                    font.pixelSize: el.fitProbe
                    font.weight: fitText.font.weight
                    font.italic: fitText.font.italic
                    font.capitalization: fitText.font.capitalization
                    font.letterSpacing: fitText.font.letterSpacing
                    font.wordSpacing: fitText.font.wordSpacing
                    // The same line STEP the label below draws (see
                    // MetadataFields.lineHeightWithGap): the twin must measure
                    // the spacing that is painted, or the fit solve answers for
                    // a layout nobody sees.
                    lineHeight: MetadataFields.lineHeightWithGap(el.meta, measureLineBox.contentHeight)
                    lineHeightMode: Text.ProportionalHeight
                    // The twin measures the layout the label below draws: a
                    // one-line block is measured unwrapped, as it renders.
                    wrapMode: MetadataFields.oneLine(el.meta) ? Text.NoWrap : Text.WordWrap

                    // This font's OWN line box at the probe size — the number Qt
                    // multiplies by lineHeight. Measured with a Text (line height
                    // 1 on one glyph) rather than TextMetrics, which exposes no
                    // ascent/descent; the two would only differ if Qt changed
                    // what it means by "a line box", and then this follows it.
                    Text {
                        id: measureLineBox
                        visible: false
                        text: "Xg"
                        font: fitMeasure.font
                        lineHeight: 1
                        lineHeightMode: Text.ProportionalHeight
                    }
                }
                // The per-LINE background band (the Lines card's "Background
                // color"), painted UNDER the glyphs by the same text as rich
                // text — Qt bands each wrapped line fragment at that line's own
                // width, so the tile shows what the output draws (the engine
                // paints one band per laid-out line, TextStyle::lineBg).
                LineBandLayer {
                    anchors.fill: parent
                    // The bands follow the GLYPHS they band — and while this block
                    // draws the passage, that is the flow's own band layer below
                    // (the words are one translated column there, so a band left
                    // here would stay put while its words slid).
                    visible: el.kind === "text" && !root.paintOnly && !el.drawsFlow
                    layout: fitText
                    color: String(el.meta.lineBg ?? "")
                    radius: MetadataFields.lineRadius(el.meta)
                    textCase: el.meta.textCase ?? "none"
                }
                Text {
                    id: fitText
                    // HIDDEN WHILE THE PASSAGE IS DRAWN (el.drawsFlow): the flow
                    // contains this page's words already, so drawing both puts two
                    // copies of every line in one box.
                    visible: el.kind === "text" && !root.paintOnly && !el.drawsFlow
                    anchors.fill: parent
                    leftPadding: el.style.padding ?? 0; rightPadding: el.style.padding ?? 0
                    // (a block from the canvas may carry no text at all)
                    readonly property string ownText: (el.modelData.text ?? "") !== "" ? el.modelData.text
                        : ((root.showBindPlaceholders && (el.modelData.bind ?? "") !== "") ? "{" + el.modelData.bind + "}" : "")
                    text: TextFormatService.applyList(ownText, el.meta.list ?? "")
                    color: el.meta.color ?? "#f2f4fa"
                    // THE ENGINE'S OWN ANSWER (FontService -> FontManager), not
                    // the raw name and not Qt's substitution: a template whose
                    // font this machine lacks must draw here in the SAME face the
                    // frame on air uses, or the card and the output disagree on
                    // advances, wrapping and the auto-size solve.
                    font.family: FontService.resolve(el.meta.fontFamily)
                    // Always the solver's answer: grow fills the box, shrink/none render
                    // at the set size and squeeze only on overflow — the text ALWAYS sits
                    // inside its box, on every surface this item draws for.
                    font.pixelSize: el.fitAnswer
                    fontSizeMode: Text.FixedSize
                    font.weight: el.meta.bold ? Font.Bold
                        : el.meta.fontWeight === "Regular" ? Font.Normal
                        : el.meta.fontWeight === "SemiBold" ? Font.DemiBold
                        : el.meta.fontWeight === "Bold" ? Font.Bold : Font.Medium
                    font.italic: el.meta.italic === true
                    font.capitalization: el.meta.textCase === "upper" ? Font.AllUppercase
                                       : el.meta.textCase === "lower" ? Font.AllLowercase
                                       : el.meta.textCase === "capitalize" ? Font.Capitalize : Font.MixedCase
                    font.underline: el.meta.underline === true
                    font.strikeout: el.meta.strikethrough === true
                    // Same ONE reading as the Edit canvas and the Text panel —
                    // 0/absent is the font's own line height, never a hidden 1.2.
                    font.letterSpacing: MetadataFields.letterSpacing(el.meta)
                    // Word space: QML's own font.wordSpacing, the same additive
                    // px value the engine adds after every space glyph.
                    font.wordSpacing: MetadataFields.wordSpacing(el.meta)
                    lineHeight: MetadataFields.lineHeightWithGap(el.meta, labelLineBox.contentHeight)
                    // The same font's own line box, measured the same way (see
                    // measureLineBox above) so the label and the twin agree.
                    Text {
                        id: labelLineBox
                        visible: false
                        text: "Xg"
                        font: fitText.font
                        lineHeight: 1
                        lineHeightMode: Text.ProportionalHeight
                    }
                    horizontalAlignment: {
                        const a = el.meta.align ?? "center"
                        return a === "left" ? Text.AlignLeft : a === "right" ? Text.AlignRight
                             : a === "justify" ? Text.AlignJustify : Text.AlignHCenter
                    }
                    // Mirrors the canvas's own EditableCanvasLabel/EditScreen.qml verticalAlignment binding -
                    // defaults centered, a template can set "top" (or "bottom") on a block that reads better
                    // anchored to an edge (a big sermon-paragraph box, say) instead of floating mid-box.
                    verticalAlignment: {
                        const a = el.meta.verticalAlign ?? "center"
                        return a === "top" ? Text.AlignTop : a === "bottom" ? Text.AlignBottom : Text.AlignVCenter
                    }
                    wrapMode: MetadataFields.oneLine(el.meta) ? Text.NoWrap : Text.WordWrap
                    // (No elide: the solver above guarantees the laid-out text fits the
                    // box, so there is nothing to truncate — swallowed tails with an
                    // ellipsis were this line's doing.)
                }

                // ---- THE READING PASSAGE (the Alt+Down / Alt+Up step) ------
                //
                // The block draws the PASSAGE its page was cut from — this page
                // and the one after it, as one string — and slides it under its
                // own box by LiveOutputService.readingScroll LINES. That offset is
                // the same number the engine is handed for the HDMI window and the
                // NDI feed, so the tile and the projector are two views of one
                // scroll rather than two animations that have to agree.
                //
                // WHY THIS IS THE SCROLL AND NOT AN ANIMATION OF ONE: the words
                // are one laid-out column that exists once and moves (el.clip cuts
                // it at the box's edges). There is no second copy, no cross-fade
                // and no hand-off at the end of the move — so nothing can snap when
                // it lands, and the type size never changes, because the size the
                // solver chose for the PAGE still governs (linePitch is measured at
                // exactly that size).
                // THE VIEWPORT IS THE PAGE'S OWN RECTANGLE, not the block's box: a
                // box can be taller than the page it was fitted for (a shrink-mode
                // page leaves slack at the bottom), and clipping to the BOX would
                // leak the lines below into that slack — the passage would show
                // next-page text on a slide nobody has stepped yet. At offset 0
                // this window IS the page; a step moves the words inside it.
                Item {
                    id: flowWindow
                    visible: el.drawsFlow
                    x: 0
                    y: el.pageTop
                    width: el.width
                    height: fitMeasure.contentHeight
                    clip: true

                    // The passage starts at the top of the window and travels UP by
                    // the offset, one measured line at a time.
                    Item {
                        id: flowBox
                        // THIS STYLE's line for the shared position (see stepLine):
                        // two outputs styled at different sizes used to scroll the
                        // same LINE number, which is a different word in each — one
                        // showed the passage's next sentence while the other was
                        // still on the previous one.
                        y: -root.stepLine * el.linePitch
                        width: parent.width
                        height: flowText.implicitHeight

                        LineBandLayer {
                            anchors.fill: parent
                            visible: !root.paintOnly
                            layout: flowText
                            color: String(el.meta.lineBg ?? "")
                            radius: MetadataFields.lineRadius(el.meta)
                            textCase: el.meta.textCase ?? "none"
                        }

                        Text {
                            id: flowText
                            width: parent.width
                            // The passage as the service assembled it, with the
                            // block's own list formatting applied — the same call
                            // fitText makes.
                            text: TextFormatService.applyList(
                                      String(LiveOutputService.readingFlowText ?? ""),
                                      el.meta.list ?? "")
                            // EVERYTHING ABOUT THE TYPE IS THE PAGE'S: family,
                            // size (the solver's answer), spacing, case, wrap. Only
                            // the string is longer, so the page's own lines land
                            // exactly where they did and the lines below it are the
                            // ones that scroll into view.
                            font: fitText.font
                            fontSizeMode: Text.FixedSize
                            color: fitText.color
                            leftPadding: fitText.leftPadding
                            rightPadding: fitText.rightPadding
                            horizontalAlignment: fitText.horizontalAlignment
                            // TOP, always: the passage is one column starting at
                            // the page's top edge, not a block centred in the box.
                            verticalAlignment: Text.AlignTop
                            // REVERTED to fitText.wrapMode (WordWrap) — NoWrap only
                            // made sense paired with flowTextFor's wrapped-lines
                            // build (now reverted — see its own comment: that build
                            // broke el.drawsFlow's prefix match against the block's
                            // raw text, which is what actually caused "not moving").
                            // With the raw text restored here too, NoWrap would draw
                            // each authored paragraph as one unbroken line clipped
                            // at the box edge.
                            wrapMode: fitText.wrapMode
                            lineHeight: fitText.lineHeight
                            lineHeightMode: fitText.lineHeightMode
                            // WHAT THIS COLUMN REALLY BECAME (see noteReadingDrawn):
                            // the drawn line count against the service table's, and
                            // the size each was laid out at — the two numbers that
                            // decide whether the step and the words are one grid.
                            onLineCountChanged: root.noteReadingDrawn(flowText, el)
                            Component.onCompleted: root.noteReadingDrawn(flowText, el)
                        }
                    }
                }

                // --- shapes: the Edit screen's drawing - the fill is the item's background (transparent draws nothing, so an
                //     outline-only frame stays an outline), the stroke its border ---
                Rectangle {   // rectangle / rounded / anything unrecognised
                    visible: el.kind === "shape" && !el.isCircle && !el.isLine && !el.isGlyph
                             && !el.paintUnderFeed
                    anchors.fill: parent
                    radius: el.style.cornerRadius ?? 0
                    color: el.fillColor
                    border.color: el.strokeColor
                    border.width: el.strokeWidth
                }
                Rectangle {   // circle - the square that fits, centred
                    visible: el.kind === "shape" && el.isCircle && !el.paintUnderFeed
                    anchors.centerIn: parent
                    width: Math.min(parent.width, parent.height); height: width
                    radius: width / 2
                    color: el.fillColor
                    border.color: el.strokeColor
                    border.width: el.strokeWidth
                }
                Rectangle {   // line - a thin bar across the middle
                    visible: el.kind === "shape" && el.isLine && !el.paintUnderFeed
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width; height: Math.max(2, el.strokeWidth || 3)
                    color: el.fillColor
                }
                Text {        // triangle / arrow / star / hexagon
                    visible: el.kind === "shape" && el.isGlyph && !el.paintUnderFeed
                    anchors.centerIn: parent
                    text: el.shapeType === "triangle" ? "▲" : el.shapeType === "arrow" ? "→" : el.shapeType === "star" ? "★" : "⬡"
                    color: el.fillColor
                    font.pixelSize: Math.min(el.width, el.height) * 0.7
                }

                // --- clock (digits, or an analog face) - formatted and sized like the canvas's own ---
                Column {
                    visible: el.kind === "clock" && el.meta.style !== "analog" && !root.paintOnly
                    anchors.centerIn: parent
                    spacing: 2

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: fmt.formatClock(root.now, el.meta.format !== "24", el.meta.showSeconds !== false)
                        color: el.meta.color ?? "#9b8ff5"
                        font.family: Theme.fontFamily
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.max(10, Math.min(el.width, el.height) * 0.22)
                    }
                    Text {
                        visible: el.meta.showDate === true
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Qt.formatDate(root.now, "dddd, MMMM d")
                        color: el.meta.color !== undefined ? Qt.alpha(el.meta.color, 0.7) : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }
                }
                Item {
                    visible: el.kind === "clock" && el.meta.style === "analog" && !root.paintOnly
                    anchors.centerIn: parent
                    width: Math.min(el.width, el.height) * 0.8; height: width

                    Rectangle {   // the face: a dark disc unless the clock has no background
                        anchors.fill: parent
                        radius: width / 2
                        color: root.isClear(el.style.backgroundColor) ? "transparent" : "#12131a"
                        border.width: 2
                        border.color: el.meta.color ?? "#3a4155"
                    }
                    Rectangle {   // hour
                        width: 4; height: parent.height * 0.24; radius: 2
                        color: el.meta.color ?? "#eef1f8"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getHours() % 12 + root.now.getMinutes() / 60) * 30
                    }
                    Rectangle {   // minute
                        width: 3; height: parent.height * 0.34; radius: 1.5
                        color: el.meta.color ?? "#c8cdd9"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getMinutes() + root.now.getSeconds() / 60) * 6
                    }
                    Rectangle {   // second
                        visible: el.meta.showSeconds !== false
                        width: 1.5; height: parent.height * 0.4
                        color: el.meta.color ?? "#6c5ce7"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: root.now.getSeconds() * 6
                    }
                    Rectangle {   // pin
                        anchors.centerIn: parent
                        width: 6; height: 6; radius: 3
                        color: el.meta.color ?? "#6c5ce7"
                    }
                }

                // --- timer: a dark box unless the item has a fill, then the count (the canvas's own rule) ---
                Rectangle {
                    visible: el.kind === "timer" && !root.paintOnly
                    anchors.fill: parent
                    radius: el.style.cornerRadius ?? 8
                    color: !root.isClear(el.style.backgroundColor) ? el.style.backgroundColor : "#12131a"
                    border.color: el.style.borderEnabled === true ? el.style.borderColor : "#3a4155"
                    border.width: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 1

                    Text {
                        anchors.centerIn: parent
                        text: fmt.formatDuration(fmt.timerSeconds(root.now, el.meta.mode ?? "countdown", el.meta.durationSeconds ?? 300,
                                                                  el.meta.startedAt ?? root.now.getTime()))
                        color: "#9b8ff5"
                        font.family: Theme.fontFamily
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.max(10, Math.min(el.width, el.height) * 0.28)
                    }
                }

                // --- camera: the live-preview tile the canvas shows (its source's name at the bottom) ---
                Rectangle {
                    visible: el.kind === "camera" && !root.paintOnly
                    anchors.fill: parent
                    radius: 8
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#123326" }
                        GradientStop { position: 1; color: "#07130e" }
                    }
                    // the canvas's own LIVE dot is Theme.danger red (EditScreen) — the
                    // preview paints the same dot, so it reads the same token.
                    Rectangle { x: 8; y: 9; width: 5; height: 5; radius: 3; color: Theme.danger }
                    Text { x: 18; y: 7; text: qsTr("LIVE"); color: "#e2e8f0"; font.family: Theme.fontFamily; font.pixelSize: 8; font.weight: Font.Medium }
                    Rectangle { x: 10; y: 24; width: 36; height: 34; radius: 4; color: "#14503a" }
                    Rectangle { x: 30; y: 24; width: 24; height: 34; radius: 4; color: "#0f3a2c" }
                    Text {
                        x: 10; y: 68
                        text: (el.modelData.text ?? "") !== "" ? el.modelData.text : qsTr("Camera")
                        color: "#eef0f6"; font.family: Theme.fontFamily; font.pixelSize: 9; font.weight: Font.Medium
                    }
                }

                // --- a placed picture, and a video's poster (Media Foundation's still
                // for the same file) — the engine's own thumbnail cache, the SAME url
                // EditScreen's own canvas asks for (mediaPictureUrl), so a slide
                // thumbnail/card/preview and the edit canvas show one picture, not a
                // decode here and a placeholder icon there (the gap that left every
                // thumbnail with just an "IMAGE" tile and no actual frame).
                Image {
                    id: picturePreview
                    readonly property bool isPicture: el.kind === "image" || el.kind === "video"
                    visible: isPicture && !root.paintOnly && status === Image.Ready && sourceSize.width > 1
                    anchors.fill: parent
                    asynchronous: true
                    cache: false
                    // Same Contain/Cover/Fill mapping EditScreen's own
                    // mediaPicture uses, read from the same style field.
                    fillMode: {
                        const fit = el.style.mediaFit !== undefined ? el.style.mediaFit : "contain"
                        return fit === "cover" ? Image.PreserveAspectCrop
                             : fit === "fill" ? Image.Stretch
                             : Image.PreserveAspectFit
                    }
                    source: isPicture && (el.meta.src ?? "") !== ""
                            ? "image://mediathumb/500/-1/" + encodeURIComponent(el.meta.src) : ""
                }

                // --- media / audio / any other kind, and an image/video whose thumbnail
                // isn't decoded (yet, or at all — MediaThumbnailProvider's own answer for
                // a file it can't read is a 1x1 blank): the canvas's generic tile - the
                // kind's icon and name, and the picked file ---
                Rectangle {
                    visible: (el.isPlaceholder || (picturePreview.isPicture && !picturePreview.visible)) && !root.paintOnly
                    anchors.fill: parent
                    radius: 6
                    color: "#1a1c26"
                    border.width: 1
                    border.color: "#3a4155"
                    Column {
                        anchors.centerIn: parent
                        spacing: 4
                        IconGlyph {
                            anchors.horizontalCenter: parent.horizontalCenter
                            name: el.kind === "audio" ? "music" : el.kind === "media" || el.kind === "video" ? "play" : el.kind === "image" ? "image" : "shape"
                            color: "#9b8ff5"
                            width: 16; height: 16
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: el.kind.toUpperCase()
                            color: "#aeb6c8"; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.Medium
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: (el.modelData.text ?? "") !== ""
                            text: el.modelData.text ?? ""
                            color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 10
                        }
                    }
                }

                // --- vignette: the block's tint fading in from all four edges, meta.inset deep (a CSS inset shadow) ---
                Item {
                    visible: el.kind === "vignette"
                    anchors.fill: parent
                    id: vig
                    readonly property real depth: Math.max(1, Math.min(el.meta.inset ?? 100, Math.min(el.width, el.height) / 2))
                    readonly property color clear: Qt.alpha(el.tint, 0)

                    Rectangle {   // top
                        width: parent.width; height: parent.depth
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: el.tint }
                            GradientStop { position: 1.0; color: vig.clear }
                        }
                    }
                    Rectangle {   // bottom
                        y: parent.height - parent.depth
                        width: parent.width; height: parent.depth
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: vig.clear }
                            GradientStop { position: 1.0; color: el.tint }
                        }
                    }
                    Rectangle {   // left
                        width: parent.depth; height: parent.height
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: el.tint }
                            GradientStop { position: 1.0; color: vig.clear }
                        }
                    }
                    Rectangle {   // right
                        x: parent.width - parent.depth
                        width: parent.depth; height: parent.height
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: vig.clear }
                            GradientStop { position: 1.0; color: el.tint }
                        }
                    }
                }

                // --- corners: the four screen corners cut round (meta.inset = the radius) ---
                Shape {
                    visible: el.kind === "corners"
                    anchors.fill: parent
                    ShapePath {
                        fillColor: el.tint
                        strokeColor: "transparent"
                        PathSvg { path: root.cornersPath(el.width, el.height, Math.max(1, el.meta.inset ?? 40)) }
                    }
                }

                // --- kinds only overlays use ---
                readonly property color tint: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "#000000"
                readonly property string shapeType: el.meta.shapeType ?? "rectangle"
                // text fitting, in the canvas's terms ("shrink" / "grow" also arrive as "shrinkToFit" / "growToFit")
                readonly property string fitMode: (el.meta.autoSize === "growToFit" || el.meta.autoSize === "grow") ? "grow"
                    : (el.meta.autoSize === "shrinkToFit" || el.meta.autoSize === "shrink") ? "shrink" : "none"
                readonly property bool isPlaceholder: !screenWide && !["text", "shape", "clock", "timer", "camera", "image", "video"].includes(el.kind)
                // a shape's look comes from the item's own style, exactly as on the canvas
                readonly property color fillColor: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "#3a3f55"
                readonly property color strokeColor: el.style.borderColor !== undefined ? el.style.borderColor : "#6c5ce7"
                readonly property real strokeWidth: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 0
                readonly property bool isCircle: el.shapeType === "circle"
                readonly property bool isLine: el.shapeType === "line"
                readonly property bool isGlyph: ["triangle", "arrow", "star", "hexagon"].includes(el.shapeType)
            }

        }
    }
}
