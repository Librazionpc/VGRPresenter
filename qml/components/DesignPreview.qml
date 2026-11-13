import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

Item {
    id: root

    property var blocks: []
    property string background: "transparent"
    property bool paintOnly: false
    property string backgroundImage: ""
    property bool showCheckerboard: true
    // What time the clocks show; the host ticks it (one shared ticker rather than one per card).
    property date now: new Date()
    property real checkerSize: 13
    property bool showBindPlaceholders: true

    // ---- THE READING STEP (LiveOutputService.lineStep) ----------------------
    property bool animateLineSteps: false
    property string readingStyleId: ""
    readonly property real stepLine: {
        const ch = LiveOutputService.readingChar
        if (!root.animateLineSteps || root.readingStyleId === "" || ch < 0)
            return LiveOutputService.readingScroll
        return LiveOutputService.readingLineForStyle(root.readingStyleId, ch)
    }
    // ---- WHAT THIS SURFACE REALLY LAID OUT (diagnostic, one line per wrap) ---
    property int readingDrawnLines: -1
    property int readingExpectedLines: -1
    property real readingDrawnBoxW: 0
    property real readingDrawnBoxH: 0
    // The signature of the last ReadingDrawn line this surface wrote — see
    // noteReadingDrawn: the log is change-driven, not lineCount-driven.
    property string readingDrawnLogged: ""
    // ---- DO THIS SURFACE'S OWN LINE BREAKS MATCH THE SHARED TABLE? -------------
    // The engine's outputs draw the service's line table exactly (ForcedLineBreaks); this surface
    // lets Qt Text wrap (only a wrapped line can be justified, and the line bands read this
    // layout). So the agreement is CHECKED: the lines Text drew against the table's. One log
    // line per distinct answer.
    property var breakCheckFlow: null
    property var breakCheckBlock: null
    property string breakCheckLogged: ""
    function queueBreakCheck(flow, block) {
        if (!block.readingBlock || !root.animateLineSteps)
            return
        root.breakCheckFlow = flow
        root.breakCheckBlock = block
        breakCheckTimer.restart()
    }
    Timer {
        id: breakCheckTimer
        interval: 500        // the table is measured off-thread; check once it has settled
        repeat: false
        onTriggered: root.verifyBreaks()
    }
    function verifyBreaks() {
        const flow = root.breakCheckFlow, block = root.breakCheckBlock
        if (!flow || !block || typeof EngineBridge === "undefined" || !root.animateLineSteps)
            return
        if (String((block.meta ?? {}).list ?? "") !== "" || flow.lineCount < 1)
            return           // list formatting rewrites the text: its offsets are not the table's
        const style = String(root.readingStyleId)
        let tableLines = 0
        try { tableLines = LiveOutputService.readingRunLineCount(style) } catch (e) { return }
        if (tableLines <= 0 || !LiveOutputService.readingColumnTableMeasured(style))
            return           // table not measured yet (or still the pre-seat stand-in: nothing to agree with)
        const label = (root.objectName !== "" ? root.objectName : (style === "" ? "(ref)" : style.slice(0, 8)))
        // THE TABLE IS THE WALK'S PASSAGE'S. A surface showing anything else (the staged preview of another
        // pick, the reference pane, a page mid-swap) has no business agreeing with it: comparing its line
        // count to this table reported "DIFFERS" for text that was never the walk's — noise that hid real
        // disagreements. Only a surface whose text IS the walk's passage (or a slice cut from it) is checked.
        const passage = String(LiveOutputService.readingFlowText ?? "")
        const flowText = String(flow.text ?? "")
        const showsWalk = flow.sliced === true ? passage.indexOf(flowText) >= 0 : flowText === passage
        if (!showsWalk)
            return
        // ONLY A SURFACE THAT IS IN STEP WITH ITS COLUMN CAN DISAGREE WITH IT. The table is measured at the
        // column's solved size; a surface that is hidden, not yet laid out, or still drawing at another
        // size (the column unsolved — colPx -1 — or a stage/preview copy drawn at its own fit) wraps
        // differently by construction. Counting those as "DIFFERS" buried the real ones, so they are skipped.
        const columnPx = Number(LiveOutputService.readingColumnPxFor(style))
        const inStep = root.visible && root.width > 0 && columnPx > 0
                       && columnPx === Math.floor(Number(flow.font.pixelSize))
        if (!inStep)
            return
        // (QML Text cannot say where it broke a line — positionAt exists only on TextEdit — so
        // the check is the line COUNT: one differently-placed break nearly always moves it.)
        // (a slice carries only its own lines: the table's lines in that range, not all of them)
        const expected = flow.sliced === true ? root.sliceLines : tableLines
        const verdict = flow.lineCount !== expected
            ? "DIFFERS in line count: text=" + flow.lineCount + " table=" + expected
              + " sliced=" + (flow.sliced === true) + " first=" + root.sliceFirst
              + " flowW=" + (flow.width - flow.leftPadding - flow.rightPadding).toFixed(1)
              + " px=" + Number(flow.font.pixelSize).toFixed(2)
              + " colPx=" + Number(LiveOutputService.readingColumnPxFor(style)).toFixed(2)
              + " box=" + block.width + "x" + block.height + " chars=" + flowText.length
              + " sliceStyle=" + String(root.sliceStyle).slice(0, 8) + " sliceTable=" + root.sliceTable
              + " tableNow=" + tableLines + " stage=" + root.sliceStage + "/" + LiveOutputService.readingStageSerial
            : "agrees lines=" + expected
        const sig = label + "|" + verdict + "|" + flow.text.length
        if (root.breakCheckLogged === sig)
            return
        root.breakCheckLogged = sig
        EngineBridge.log("info", "ReadingBreaks", "surface=" + label + " " + verdict)
    }
    function noteReadingDrawn(flow, block) {
        if (!block.readingBlock)
            return
        root.queueBreakCheck(flow, block)
        readingDrawnLines = flow.lineCount
        readingDrawnBoxW = block.width
        readingDrawnBoxH = block.height
        if (typeof EngineBridge === "undefined")
            return
        const style = String(root.readingStyleId)
        let table = -1
        let svcSize = -1
        try { table = LiveOutputService.readingRunLineCount(style) } catch (e) { table = -2 }
        try { svcSize = LiveOutputService.readingRunSizeForStyle(style) } catch (e) { svcSize = -2 }
        readingExpectedLines = flow.sliced === true ? root.sliceLines : table
        const drawnSig = style + "|" + flow.lineCount + "|" + table + "|" + block.width
            + "x" + block.height + "|" + Number(flow.font.pixelSize).toFixed(2)
        if (root.readingDrawnLogged === drawnSig)
            return
        root.readingDrawnLogged = drawnSig
        EngineBridge.log("trace", "ReadingDrawn",
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
    // ---- THE PASSAGE SLICE THIS SURFACE LAYS OUT ---------------------------------------------------
    // The service hands every surface the lines it can show (plus a margin) instead of the whole 3-6k
    // characters: laying the full passage out in a justified Text, once per surface on every pick, was the
    // bulk of a double-click's cost. The slice starts at a line start of THIS style's table, so its breaks
    // are the full text's own (see LiveOutputService.readingFlowSliceFor).
    property bool sliceWhole: true
    property int sliceFirst: 0
    property int sliceLines: 0
    property string sliceText: ""
    property string sliceKey: ""
    // The numbers the slice was last validated against (see refreshSlice: the per-tick path compares these, not the text).
    property var sliceStage: -1
    property string sliceStyle: ""
    property int sliceVisible: -1
    property int sliceTable: -1
    // The slice is a bounded stand-in (no line table for this style yet): valid until the table's tell changes `sliceTable`.
    property bool sliceProvisional: false
    // WHEN THIS SURFACE RE-SLICES, STAGGERED. Every surface (each monitor tile, the pane) re-slices at the same line, and
    // a new slice is a justified re-layout of ~3 screens of text on EACH of them, inside ONE tick: ~35 ms on a real
    // passage — a hitch every few lines on the tiles while the engine output stayed smooth (a short passage never
    // slices, which is why it ran clean). Each surface now re-slices a few lines EARLIER than the minimum, by its own
    // random margin picked when its slice is made, so the shifts land on different ticks and each costs a few
    // milliseconds instead of all of them at once.
    property int shiftSkew: 0
    property bool sliceAtEnd: false
    // A list format rewrites the text, so the table's line starts are not its line starts:
    // that one block keeps the whole passage (it is the case `sliced` already excludes).
    function sliceFor(block) {
        const list = String(block.meta !== undefined && block.meta !== null ? (block.meta.list ?? "") : "")
        if (list !== "")
            return TextFormatService.applyList(String(LiveOutputService.readingFlowText ?? ""), list)
        return TextFormatService.applyList(root.sliceText, "")
    }
    // THE ONE THING A TELL MAY TOUCH IS THE WINDOW, NEVER THE GLYPHS. `sliceText` is the
    // surface's passage text; a scroll tick that stays inside the current window leaves it
    // alone, so the justified Text is not laid out again (which is what a tell used to cost).
    // A SURFACE NOBODY CAN SEE DOES NOT LAY THE PASSAGE OUT. Every DesignPreview in the app re-sliced on every stage
    // tell (the hidden panes and tabs and off-screen cards too), and each cut is a justified layout of the
    // passage: 15-40 ms, a dozen of them inside one pick was ~300 ms of the interface thread. A hidden or
    // zero-size surface only remembers that it is behind; it re-slices the moment it can be seen.
    property bool sliceBehind: false
    function refreshSlice() {
        if (!root.visible || root.width <= 0) {
            root.sliceBehind = true
            return
        }
        root.sliceBehind = false
        if (!root.animateLineSteps || LiveOutputService.readingBudgetFitActive !== true) {
            const flow = String(LiveOutputService.readingFlowText ?? "")   // (only this branch needs the whole text)
            root.sliceWhole = true
            root.sliceFirst = 0
            root.sliceLines = 0
            root.sliceKey = ""
            root.sliceAtEnd = true
            if (root.sliceText !== flow)
                root.sliceText = flow
            return
        }
        const style = String(root.readingStyleId)
        const visible = Math.max(3, LiveOutputService.readingVisibleLinesFor(style))
        const first = Math.max(0, Math.floor(root.stepLine))
        const stage = LiveOutputService.readingStageSerial
        // THE TABLE THE SLICE WAS CUT FROM. A style's line table lands (or is replaced by the column solving
        // at another size) after the stage serial has already moved, so a slice cut against the old table — or
        // against none, which hands back the whole passage — kept laying out the wrong lines until the walk
        // happened to leave it. Its line count is one number: when it changes, the slice is cut again.
        let table = 0
        try { table = LiveOutputService.readingRunLineCount(style) } catch (e) { table = -1 }
        const roomBelow = root.sliceFirst + root.sliceLines - (first + visible)
        // THE PER-TICK PATH COSTS NUMBERS, NOT THE PASSAGE. This check runs on EVERY surface on EVERY tick of
        // every glide (twice: the position change and the service's tell). It used to build its key from the
        // passage text itself (`flow`, fetched from C++ into JS, sliced, concatenated) — a cost that grows with the
        // passage (a 3-6k character Bible chapter or sermon page against a short probe) and was multiplied by
        // every monitor tile: the reason a real passage's glide cost ~35 ms per tick while a short one did not,
        // so the surfaces had to be throttled to a few frames a second (they read as a jitter next to the
        // engine's own smooth output). The stage serial already says "the passage changed"; the text is only
        // fetched when it (or the style / size / window) did.
        if (stage === root.sliceStage && style === root.sliceStyle && table === root.sliceTable
                && visible === root.sliceVisible && first >= root.sliceFirst
                && (root.sliceWhole || root.sliceProvisional || roomBelow >= 2 + root.shiftSkew || root.sliceAtEnd))
            return                      // still comfortably inside the slice
        const flowText = String(LiveOutputService.readingFlowText ?? "")
        const key = stage + "|" + flowText.length + "|" + flowText.slice(0, 24)
                    + "|" + style + "|" + visible + "|" + table
        if (key === root.sliceKey && first >= root.sliceFirst
                && (root.sliceWhole || root.sliceProvisional || roomBelow >= 2 + root.shiftSkew || root.sliceAtEnd)) {
            root.sliceStage = stage; root.sliceStyle = style; root.sliceVisible = visible; root.sliceTable = table
            return                      // the same passage after all (a serial moved, the text did not)
        }
        root.sliceStage = stage; root.sliceStyle = style; root.sliceVisible = visible; root.sliceTable = table
        const slice = LiveOutputService.readingFlowSliceFor(style, Math.max(0, first - visible), visible * 3)
        // A new random margin for THIS slice (0 .. visible-4 lines earlier than the bare minimum), so the surfaces' next
        // shifts are spread over different ticks. The new slice leaves `visible` lines of room, so the margin can
        // never exceed what it holds.
        root.shiftSkew = Math.floor(Math.random() * Math.max(1, visible - 3))
        root.sliceKey = key
        root.sliceProvisional = slice.provisional === true
        // (a provisional slice begins AT the walk: its first line is where the walk is, so it sits at the box's top)
        root.sliceFirst = root.sliceProvisional ? first : slice.first
        root.sliceLines = slice.lines
        root.sliceWhole = slice.whole === true
        root.sliceAtEnd = slice.atEnd === true
        const text = String(slice.text ?? "")
        if (root.sliceText !== text)
            root.sliceText = text
    }
    onStepLineChanged: root.refreshSlice()
    onReadingStyleIdChanged: root.refreshSlice()
    onAnimateLineStepsChanged: root.refreshSlice()
    onVisibleChanged: if (root.visible && root.sliceBehind) root.refreshSlice()
    onWidthChanged: if (root.sliceBehind && root.width > 0) root.refreshSlice()
    Connections {
        target: LiveOutputService
        function onReadingStageChanged() { root.refreshSlice() }
        // ...and a tell, because a LIVE pick hands the surface its passage without staging
        // anything. This is now a key comparison: `sliceText` only changes when the window
        // does, and only a change of THAT re-lays the text out.
        function onReadingScrollChanged() { root.refreshSlice() }
    }
    Component.onCompleted: root.refreshSlice()
    property string readingFitKey: ""
    property real readingFitSize: 0
    // The last size the walk's COLUMN itself gave this surface (never a fit of the whole text, never the block's own
    // font size): what it keeps drawing at while the next pick's size is solved off-thread, so a new pick does not
    // flash tiny and then correct itself.
    property real columnFitSize: 0
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
    readonly property int readingBlockIndex: {
        const bs = root.blocks !== undefined && root.blocks !== null ? root.blocks : []
        const longest = root.stepTextLength
        if (longest <= 0)
            return -1
        if (typeof LiveOutputService !== "undefined"
                && LiveOutputService.readingBudgetFitActive === true) {
            for (let i = 0; i < bs.length; ++i) {
                const b = bs[i]
                if (b && (b.kind === undefined || b.kind === "text")
                        && String(b.bind ?? "") === "text"
                        && String(b.text ?? "") !== "")
                    return i
            }
        }
        for (let i = 0; i < bs.length; ++i) {
            const b = bs[i]
            if (!b || (b.kind !== undefined && b.kind !== "text"))
                continue
            if (String(b.text ?? "").length === longest)
                return i
        }
        return -1
    }

    readonly property real stageW: StageGrid.w
    readonly property real stageH: StageGrid.h
    readonly property real unit: width / StageGrid.w

    implicitHeight: StageGrid.heightForWidth(width)
    clip: true

    // The canvas's clock/timer formatting, shared (this one never ticks; the host feeds `now`).
    LiveClock { id: fmt; running: false }
    property var probeSink: null
    // Fully transparent? Works for "transparent", "#00000000" (what a live colour reads back as) and real colours alike.
    function isClear(c) { return c === undefined || c === null || c === "" || Qt.color(c).a === 0 }

    function cornersPath(w, h, r) {
        return "M 0 0 L " + r + " 0 A " + r + " " + r + " 0 0 0 0 " + r + " Z "
             + "M " + w + " 0 L " + w + " " + r + " A " + r + " " + r + " 0 0 0 " + (w - r) + " 0 Z "
             + "M " + w + " " + h + " L " + (w - r) + " " + h + " A " + r + " " + r + " 0 0 0 " + w + " " + (h - r) + " Z "
             + "M 0 " + h + " L 0 " + (h - r) + " A " + r + " " + r + " 0 0 0 " + r + " " + h + " Z"
    }

    Item {
        width: root.stageW; height: root.stageH
        scale: root.unit
        transformOrigin: Item.TopLeft

        Checkerboard {
            anchors.fill: parent
            tileSize: root.checkerSize
            hostScale: root.unit
            visible: root.showCheckerboard && root.isClear(root.background)
        }
        Rectangle {
            anchors.fill: parent
            visible: !root.isClear(root.background)
            color: root.background
        }
        Image {
            anchors.fill: parent
            visible: root.backgroundImage !== ""
            source: root.backgroundImage !== "" ? "file:///" + root.backgroundImage : ""
            fillMode: Image.PreserveAspectCrop
        }

        Repeater {
            model: root.blocks ? root.blocks.length : 0

            delegate: Item {
                id: el
                required property int index
                readonly property var modelData: (root.blocks && index < root.blocks.length)
                                                 ? root.blocks[index] : ({})
                readonly property string kind: modelData.kind
                readonly property var meta: modelData.meta !== undefined ? modelData.meta : {}
                readonly property var style: modelData.style !== undefined ? modelData.style : {}
                readonly property real linePitch: measureLineBox.contentHeight * fitMeasure.lineHeight
                // THE READING BLOCK: the longest text (the same rule the service
                // steps and the engine glides) — the only block the line step moves.
                readonly property bool readingBlock: el.kind === "text"
                    && String(modelData.text ?? "").length > 0
                    && String(modelData.text ?? "").length === root.stepTextLength
                    && el.index === root.readingBlockIndex
                readonly property bool drawsFlow: el.readingBlock && el.kind === "text"
                    && !root.paintOnly && root.animateLineSteps
                    && String(el.modelData.text ?? "") !== ""
                    && (LiveOutputService.readingBudgetFitActive === true
                        ? String(LiveOutputService.readingFlowText ?? "") !== ""
                        : String(LiveOutputService.readingFlowText ?? "")
                              .indexOf(String(el.modelData.text ?? "")) >= 0)
                onDrawsFlowChanged: EngineBridge.log("trace", "DrawsFlowChanged",
                    "drawsFlow=" + el.drawsFlow
                    + " readingBlock=" + el.readingBlock
                    + " modelText=" + String(el.modelData.text ?? "").slice(0, 24)
                    + " flowText=" + String(LiveOutputService.readingFlowText ?? "").slice(0, 24)
                    + " fitAnswer=" + el.fitAnswer)
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
                readonly property bool paintUnderFeed: modelData.underVideo === true

                opacity: el.style.opacity !== undefined ? el.style.opacity : 1

                x: modelData.x; y: modelData.y
                width: modelData.width !== undefined ? modelData.width : 160
                height: modelData.height !== undefined ? modelData.height : 40
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

                property real fitProbe: el.meta.fontSize ?? 16
                property real fitAnswer: (el.readingBlock && root.animateLineSteps
                                          && LiveOutputService.readingBudgetFitActive)
                                         ? Number(el.meta.fontSize ?? 16)
                                         : (el.readingBlock && root.animateLineSteps
                                          && root.readingFitSize > 0
                                          && root.readingFitKey === el.readingFitKey)
                                         ? root.readingFitSize : (el.meta.fontSize ?? 16)
                property bool fitSolving: false
                readonly property string readingFitKey: {
                    const reading = el.readingBlock && root.animateLineSteps
                    const budgetFit = reading && typeof LiveOutputService !== "undefined"
                                      && LiveOutputService.readingBudgetFitActive === true
                    const parts = []
                    if (reading && typeof LiveOutputService !== "undefined") {
                        parts.push(String(LiveOutputService.readingSetName ?? ""))
                        parts.push(String(budgetFit))
                        if (budgetFit) {
                            parts.push(String(LiveOutputService.readingWordBudget ?? 40))
                        } else {
                            // Slide mode fits each selected slide's own text. Reuse
                            // the size only when the slide text is unchanged.
                            parts.push(String(fitText.text ?? ""))
                        }
                        parts.push(String(Number((LiveOutputService.readingJumpSerial
                                                  ?? LiveOutputService.readingStageSerial) ?? 0)))
                    }
                    parts.push(el.width, el.height, el.style.padding ?? 0,
                               el.meta.fontFamily ?? "",
                               MetadataFields.lineHeight(el.meta),
                               MetadataFields.lineSpacing(el.meta),
                               MetadataFields.letterSpacing(el.meta))
                    if (!reading || !budgetFit)
                        parts.push(el.meta.fontSize ?? 16)
                    return parts.join("|")
                }
                // A SCROLL SURFACE SIZES ITS WORDS BY THE BUDGET'S WORDS — ALWAYS, NOT ONLY ONCE SOMETHING IS STAGED.
                // Before this, a pane's preview (a single click only selects: nothing is staged, so the walk's budget
                // fit is not active yet) fitted the WHOLE selected paragraph into its box — a long one came out tiny —
                // and only a double-click (the stage) brought the real size. Now the preview fits the same first
                // `budget` words of its own text that the staged column will be fitted on, so a click, the ‹ ›, Enter
                // and a double-click all show the same size.
                readonly property bool budgetFitOn: LiveOutputService.readingBudgetFitActive === true
                readonly property int scrollBudget: (el.readingBlock && root.animateLineSteps)
                                                    ? Math.max(3, Number(LiveOutputService.readingWordBudget ?? 40)) : 0
                readonly property string scrollProbe: {
                    if (el.scrollBudget <= 0)
                        return ""
                    // A word with the break that FOLLOWS it (a paragraph's new line
                    // included), so the probe takes the lines the real page takes.
                    const source = el.budgetFitOn ? String(LiveOutputService.readingFlowText ?? "")
                                                  : String(fitText.text ?? "")
                    const words = source.match(/\S+\s*/g) || []
                    if (words.length === 0)
                        return ""
                    const probe = []
                    for (let i = 0; i < el.scrollBudget; ++i)
                        probe.push(words[i % words.length])
                    return probe.join("").trim()
                }
                readonly property var fitInputs: [
                    fitText.text, el.width, el.height, el.fitMode, el.scrollBudget,
                    el.meta.fontSize ?? 16, el.meta.fontFamily ?? "",
                    el.meta.fontWeight ?? "", el.meta.bold === true, el.meta.italic === true,
                    el.meta.textCase ?? "none", el.meta.underline === true,
                    el.meta.strikethrough === true,
                    MetadataFields.lineHeight(el.meta), MetadataFields.lineSpacing(el.meta),
                    MetadataFields.letterSpacing(el.meta), MetadataFields.wordSpacing(el.meta),
                    MetadataFields.oneLine(el.meta), el.style.padding ?? 0
                ]
                property bool scrollColumnPending: false
                // THE PASSAGE IS NOT SHOWN AT A PLACEHOLDER SIZE. The column's size is solved off the interface
                // thread, so the first ask after a pick is answered "not yet" and the surface drew the passage at the
                // block's own font size (16 — a fraction of the real one) until the answer came; and because the
                // surface only asked again on the NEXT stage change, a cold first pick stayed that small until the
                // second double-click. Now: (1) the answer's arrival (the service's tell) re-asks at once, and (2) the
                // passage waits, hidden, for the real size — with a failsafe so a size that never comes still shows.
                readonly property bool fitWaiting: el.scrollColumnPending && root.columnFitSize <= 0 && !el.fitWaitGaveUp
                property bool fitWaitGaveUp: false
                Timer {
                    id: fitWaitTimer
                    interval: 1500
                    onTriggered: el.fitWaitGaveUp = true
                }
                onScrollColumnPendingChanged: {
                    if (el.scrollColumnPending) {
                        el.fitWaitGaveUp = false
                        fitWaitTimer.restart()
                    } else {
                        fitWaitTimer.stop()
                        el.fitWaitGaveUp = false
                    }
                }
                Connections {
                    target: LiveOutputService
                    function onReadingStageChanged() { el.maybeResolveScroll() }
                    // The solved size is announced with readingScrollChanged (the surfaces that asked get to ask
                    // again). Only a surface still WAITING answers it: a glide's every tick is not a fit.
                    function onReadingScrollChanged() {
                        if (el.scrollColumnPending)
                            el.maybeResolveScroll()
                    }
                }
                function maybeResolveScroll() {
                    const active = LiveOutputService.readingBudgetFitActive === true
                    if (active && el.readingBlock && root.animateLineSteps
                            && (el.scrollColumnPending || root.readingFitKey !== el.readingFitKey))
                        el.solveFit()
                }
                onFitInputsChanged: {
                    if (fitSolving)
                        return
                    const scrollBudgetFit = el.readingBlock && root.animateLineSteps
                                            && typeof LiveOutputService !== "undefined"
                                            && LiveOutputService.readingBudgetFitActive === true
                    if (scrollBudgetFit)
                        el.solveFit()
                    else
                        Qt.callLater(el.solveFit)
                }
                Component.onCompleted: {
                    if (fitSolving)
                        return
                    const scrollBudgetFit = el.readingBlock && root.animateLineSteps
                                            && typeof LiveOutputService !== "undefined"
                                            && LiveOutputService.readingBudgetFitActive === true
                    if (scrollBudgetFit)
                        el.solveFit()
                    else
                        Qt.callLater(el.solveFit)
                }
                // A SURFACE NOBODY CAN SEE DOES NOT BISECT ITS FONT SIZE. The fit is eleven text layouts; every hidden
                // surface (the wall's other page, closed panes, off-screen cards) ran it on every pick — about twenty of
                // them, 8-60 ms each, ~300 ms of the interface thread inside one pick. The one hidden surface that
                // MUST still answer is a reading block once the walk's column is active: that answer is the column's
                // size, asked of the service (solved off-thread, no layout here) and it is what seats the output's
                // column. Everything else waits until it is shown (fitBehind).
                property bool fitBehind: false
                function solveFit() {
                    // Run through Qt.callLater, so it can fire AFTER the preview that asked has been torn down (a pick
                    // replaces surfaces): the host id then reads undefined and every line below threw "Cannot read
                    // property 'animateLineSteps' / 'visible' of undefined" into the log.
                    if (typeof root === "undefined" || root === null)
                        return
                    const columnAnswer = el.readingBlock && root.animateLineSteps
                                         && LiveOutputService.readingBudgetFitActive === true
                    if ((!root.visible || el.width <= 0) && !columnAnswer) {
                        el.fitBehind = true
                        return
                    }
                    el.fitBehind = false
                    el.solveFitInner()
                }
                Connections {
                    target: root
                    function onVisibleChanged() {
                        if (root.visible && el.fitBehind)
                            el.solveFit()
                    }
                }
                function solveFitInner() {
                    if (fitSolving)
                        return
                    fitSolving = true
                    const hostAlive = typeof root !== "undefined" && root !== null && root !== undefined
                    if (hostAlive && root.probeSink)
                        root.probeSink.gridFitSolves++
                    if (hostAlive && el.readingBlock && root.animateLineSteps
                            && LiveOutputService.readingBudgetFitActive === true) {
                        if (root.readingFitSize > 0 && root.readingFitKey === el.readingFitKey) {
                            fitAnswer = root.readingFitSize
                            root.columnFitSize = root.readingFitSize   // (the key carries the budget flag: this IS a column answer)
                        } else {
                            const columnSize = Number(LiveOutputService.scrollColumnSize(
                                                          root.readingStyleId, el.modelData))
                            if (columnSize > 0) {
                                root.readingFitKey = el.readingFitKey
                                root.readingFitSize = columnSize
                                root.columnFitSize = columnSize
                                fitAnswer = columnSize
                                el.scrollColumnPending = false
                            } else {
                                fitAnswer = root.columnFitSize > 0 ? root.columnFitSize : (el.meta.fontSize ?? 16)
                                el.scrollColumnPending = true
                            }
                        }
                        fitProbe = fitAnswer
                        fitSolving = false
                        return
                    }
                    const pad = el.style.padding ?? 0
                    const bw = el.width - pad * 2, bh = el.height - pad * 2
                    let lo = 3, hi = 400
                    const emptyBox = bw <= 0 || bh <= 0
                    const tokenOrZero = MetadataFields.isToken(fitText.text)
                    const fixedPlaceholder = tokenOrZero && el.fitMode !== "grow"
                    const scrollFit = hostAlive && el.scrollProbe !== ""
                    if (scrollFit && root.readingFitSize > 0
                        && root.readingFitKey === el.readingFitKey) {
                        fitAnswer = root.readingFitSize
                        fitProbe = fitAnswer
                        fitSolving = false
                        return
                    }
                    // Slide-mode reading keeps its normal fitted size across pages
                    // within the same operator pick.
                    if (!tokenOrZero && hostAlive && el.readingBlock && root.animateLineSteps
                        && root.readingFitSize > 0 && root.readingFitKey === el.readingFitKey) {
                        fitAnswer = root.readingFitSize
                        fitProbe = fitAnswer
                        fitSolving = false
                        return
                    }
                    // A walk that already has its size returned above; all other text is
                    // fitted against this box (including a visible placeholder in Grow).
                    if (emptyBox || fixedPlaceholder) {
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
                    fitAnswer = (scrollFit || el.fitMode === "grow")
                                ? lo : Math.min(el.meta.fontSize ?? 16, lo)
                    fitProbe = fitAnswer
                    if (!tokenOrZero && hostAlive && el.readingBlock && root.animateLineSteps) {
                        root.readingFitKey = el.readingFitKey
                        root.readingFitSize = fitAnswer
                        if (typeof LiveOutputService !== "undefined")
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
                    // THE PROBE IS WHAT A SCROLL COLUMN IS MEASURED ON (see
                    // el.scrollProbe): the budget's words, not this page's.
                    text: el.scrollProbe !== "" ? el.scrollProbe : fitText.text
                    font: MetadataFields.textFont(el.meta, el.fitProbe)
                    lineHeight: MetadataFields.lineHeightWithGap(el.meta, measureLineBox.contentHeight)
                    lineHeightMode: Text.ProportionalHeight
                    // The twin measures the layout the label below draws: a
                    // one-line block is measured unwrapped, as it renders.
                    wrapMode: MetadataFields.oneLine(el.meta) ? Text.NoWrap : Text.WordWrap

                    Text {
                        id: measureLineBox
                        visible: false
                        text: "Xg"
                        font: fitMeasure.font
                        lineHeight: 1
                        lineHeightMode: Text.ProportionalHeight
                    }
                }
                LineBandLayer {
                    anchors.fill: parent
                    visible: el.kind === "text" && !root.paintOnly && !el.drawsFlow
                    layout: fitText
                    color: String(el.meta.lineBg ?? "")
                    radius: MetadataFields.lineRadius(el.meta)
                    textCase: el.meta.textCase ?? "none"
                }
                Text {
                    id: fitText
                    visible: el.kind === "text" && !root.paintOnly && !el.drawsFlow
                    anchors.fill: parent
                    leftPadding: el.style.padding ?? 0; rightPadding: el.style.padding ?? 0
                    // (a block from the canvas may carry no text at all)
                    readonly property string ownText: (el.modelData.text ?? "") !== "" ? el.modelData.text
                        : ((root.showBindPlaceholders && (el.modelData.bind ?? "") !== "") ? "{" + el.modelData.bind + "}" : "")
                    text: TextFormatService.applyList(ownText, el.meta.list ?? "")
                    color: el.meta.color ?? "#f2f4fa"
                    font: MetadataFields.textFont(el.meta, el.fitAnswer)
                    fontSizeMode: Text.FixedSize
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
                    verticalAlignment: {
                        const a = el.meta.verticalAlign ?? "center"
                        return a === "top" ? Text.AlignTop : a === "bottom" ? Text.AlignBottom : Text.AlignVCenter
                    }
                    wrapMode: MetadataFields.oneLine(el.meta) ? Text.NoWrap : Text.WordWrap
                }

                // ---- THE READING PASSAGE (the Alt+Down / Alt+Up step) ------
                Item {
                    id: flowWindow
                    visible: el.drawsFlow && !el.fitWaiting
                    x: 0
                    y: LiveOutputService.readingBudgetFitActive
                       ? (el.style.padding ?? 0) : el.pageTop
                    width: el.width
                    // A BOOK PAGE SHOWS WHOLE LINES: in Scroll the column fills its text area down to the last
                    // line that fits entirely (the engine's outputs do the same), so a tall box does not show
                    // the next paragraph's first line half-cut while a shorter one does not.
                    // (The line count is the service's — the rule the font solver and NDI/HDMI use — not a
                    // count from this surface's own pitch, which is a hair larger and lost a line.)
                    height: {
                        if (!LiveOutputService.readingBudgetFitActive)
                            return fitMeasure.contentHeight
                        const inner = Math.max(0, el.height - 2 * (el.style.padding ?? 0))
                        // (el.fitAnswer: re-evaluate once the column has been solved)
                        const lines = el.fitAnswer > 0
                            ? LiveOutputService.readingVisibleLinesFor(String(root.readingStyleId)) : 0
                        return lines > 0 && el.linePitch > 0
                               ? Math.min(inner, lines * el.linePitch) : inner
                    }
                    clip: true

                    // The passage starts at the top of the window and travels UP by
                    // the offset, one measured line at a time.
                    Item {
                        id: flowBox
                        // (a slice starts at line `sliceFirst` of the passage, so the offset is relative to it)
                        y: -(root.stepLine - (flowText.sliced ? root.sliceFirst : 0)) * el.linePitch
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
                            // Only the lines this surface can show (a list format rewrites the text, so its
                            // offsets are no longer the table's: that case lays out the whole passage).
                            readonly property bool sliced: !root.sliceWhole && String(el.meta.list ?? "") === ""
                            // The SLICE, always — never the walk's own passage: a binding that
                            // reads readingFlowText re-evaluates on every tell of every glide,
                            // and each re-evaluation is a full justified layout of the passage.
                            text: root.sliceFor(el)
                            font: fitText.font
                            fontSizeMode: Text.FixedSize
                            color: fitText.color
                            leftPadding: fitText.leftPadding
                            rightPadding: fitText.rightPadding
                            // SCROLL READS LIKE A BOOK PAGE: with the max-words column the passage is
                            // fully justified on every surface whatever the template says (the
                            // engine's scenes do the same), so screens differ only in size.
                            horizontalAlignment: LiveOutputService.readingBudgetFitActive
                                                 ? Text.AlignJustify : fitText.horizontalAlignment
                            // TOP, always: the passage is one column starting at
                            // the page's top edge, not a block centred in the box.
                            verticalAlignment: Text.AlignTop
                            wrapMode: fitText.wrapMode
                            lineHeight: fitText.lineHeight
                            lineHeightMode: fitText.lineHeightMode
                            onLineCountChanged: root.noteReadingDrawn(flowText, el)
                            onTextChanged: root.queueBreakCheck(flowText, el)
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

                Loader {
                    anchors.fill: parent
                    active: el.kind === "bus" && !root.paintOnly
                    sourceComponent: Rectangle {
                        radius: 6
                        color: "#000000"
                        clip: true

                        BusMonitor {
                            id: busMon
                            busRow: busMon.rowForBusId(el.meta.busId ?? 0)
                        }
                        Image {
                            anchors.fill: parent
                            source: busMon.previewSource
                            fillMode: Image.PreserveAspectCrop
                            cache: false
                            asynchronous: false
                            visible: busMon.previewing
                        }
                        Text {
                            visible: !busMon.previewing
                            anchors.centerIn: parent
                            width: parent.width - 8
                            horizontalAlignment: Text.AlignHCenter
                            text: (el.modelData.text ?? "") !== "" ? el.modelData.text : qsTr("Bus")
                            color: "#8a90a6"
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                    }
                }

                // The picture is drawn by the ONE component the template editor draws it with, so its
                // opacity, crop, flip, filters and fit read the same here as on the canvas and on the output.
                MediaPictureLayer {
                    id: picturePreview
                    readonly property bool isPicture: el.kind === "image" || el.kind === "video"
                    visible: isPicture && !root.paintOnly && ready
                    anchors.fill: parent
                    meta: picturePreview.isPicture ? el.meta : ({})
                    fit: el.style.mediaFit !== undefined ? el.style.mediaFit
                         : ((el.meta && el.meta.media && el.meta.media.fit) ? el.meta.media.fit : "contain")
                }

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
                readonly property bool isPlaceholder: !screenWide && !["text", "shape", "clock", "timer", "camera", "image", "video", "bus"].includes(el.kind)
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