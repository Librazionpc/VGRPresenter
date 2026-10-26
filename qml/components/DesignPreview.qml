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
    // How long that text is, so a delegate can tell whether IT is the reading
    // block. Only that one draws the passage: every text block used to shift by
    // the same pitch, which moved a title by the BODY's line height — a visible
    // hop out of place on the very blocks the step is not supposed to touch.
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
            model: root.blocks

            delegate: Item {
                id: el
                required property var modelData
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
                // DOES THIS BLOCK DRAW THE READING PASSAGE? Only the reading
                // block of a host that follows the step (the Main Output tile),
                // and only while the service has a passage for it. Everything
                // else about this file draws the page it always drew.
                readonly property bool drawsFlow: el.readingBlock && el.kind === "text"
                    && !root.paintOnly && root.animateLineSteps
                    // ...and only if the passage really is THIS block's page (the
                    // engine keeps the same guard for the same reason): a flow left
                    // over from another slide would put the wrong words in the box,
                    // and the page — which is what this file drew before the step
                    // existed — is always the honest fallback.
                    && String(LiveOutputService.readingFlowText ?? "")
                           .indexOf(String(el.modelData.text ?? "")) === 0
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
                property real fitAnswer: el.meta.fontSize ?? 16
                property bool fitSolving: false
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
                    if (typeof root !== "undefined" && root && root.probeSink)
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
                    if (bw <= 0 || bh <= 0 || MetadataFields.isToken(fitText.text)) {
                        lo = el.meta.fontSize ?? 16
                    } else {
                        for (let i = 0; i < 11; ++i) {
                            const mid = (lo + hi) / 2
                            fitProbe = mid
                            if (fitMeasure.contentWidth <= bw && fitMeasure.contentHeight <= bh)
                                lo = mid
                            else
                                hi = mid
                        }
                    }
                    // grow: fill the box; shrink/none: at most the set size (the squeeze).
                    fitAnswer = el.fitMode === "grow" ? lo : Math.min(el.meta.fontSize ?? 16, lo)
                    fitProbe = fitAnswer
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
                        y: -LiveOutputService.readingScroll * el.linePitch
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
