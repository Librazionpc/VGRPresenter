pragma Singleton
import QtQuick
// The engine's own vocabulary, for the boot check above the groups: the registry
// asks IT whether every field it offers can be filled (TemplateLibraryService
// forwards to BookFill::FamilyAnswersToken).
import VGRPresenterUI

// Every metadata field a slide's text can carry, in ONE place — the list the
// Edit screen's Metadata menu draws.
//
// Why a registry at all: the placeholders ({scripture_text}, {table_code}, …)
// used to exist only inside the shipped designs and the engine's own filler, so
// there was no way to put one into a template you were building yourself — you
// had to know the spelling and type it by hand. The palette is now a projection
// of this list, and this list is the projection of what the engine resolves
// (BookFill.cpp's Resolve()): add a field HERE and it appears in the
// menu, the drag payload and the item's text with nothing else touched.
//
// TWO FAMILIES, ONE PER TAB, and they do not share their words any more.
// The Table used to be offered the Bible's own vocabulary — its palette listed
// {scripture_text} ("verses"), {scripture_verses} ("verse numbers") and
// {scripture_name} ("the Bible's version") for a sermon, where nothing is a
// verse, there is no verse number and there is no Bible version at all (that
// last one rendered an empty line on the shipped Table layouts). A template
// author working on a sermon had to translate Bible words into sermon ideas.
// Each family now names its own parts, and each part stands alone so any
// COMBINATION can be placed wherever the design has room:
//
//   Scripture                    The Table
//   {scripture_text}             {table_text}              the body, numbered
//   {scripture_verses}           {table_paragraphs}        just the numbers
//   {scripture_reference}        {table_reference}         what this slide shows
//   {scripture_reference_full}   {table_reference_full}    the whole selection
//   {scripture_reference_last}   {table_reference_last}    …on the last slide
//   {scripture_book}             {table_citation}          the whole line
//   {scripture_book_abbr}        {table_code}              the date code
//   {scripture_chapter}          {table_year}              the year
//   {scripture_name}             {table_title}             the sermon's title
//   {scripture_number}           {table_number}            the number slot
//   {meta_copyright}             —
//
// The engine still resolves the scripture_ family for The Table (every saved
// sermon template keeps working); the palette simply stops OFFERING it there.
//
// Each field:
//   token  the literal placeholder, braces included — what lands in the item
//   label  what the menu chip says
//   group  which section of the menu it sits under ("Scripture" | "The Table")
//   hint   one line saying what it renders, shown on hover/the row's second line
//   bind   the ENGINE SLOT this field feeds — "text" | "ref" | "" (not a slot).
//          A registered layout block carries one (DesignCatalogs' Text() takes
//          it as an argument, e.g. the body's "text" and the reference line's
//          "ref"), and that is how the scene builder binds a template to a
//          slide: SceneBuilder::BindTemplateBlocks replaces a bound block with
//          the slide's own body/reference. A field the palette inserts WITHOUT
//          its bind is an unbound placeholder block, which the engine could not
//          resolve — the panel showed the field while the outputs drew an empty
//          box. Only the text and reference slots exist (there is no number or
//          year slot); everything else stays unbound and is resolved by the
//          fill (BookFill.cpp), which is why its value must reach the engine.
//
// A field the ENGINE does not resolve would be inert text on a slide, so this
// list stays limited to the placeholders the filler actually knows; adding one
// is a two-line change here plus its branch in BookFill.cpp's Resolve().
QtObject {
    id: root

    // The Bible's own. Every name here is a word a Bible reference actually has
    // (book, chapter, verse, version) so nothing has to be translated.
    readonly property var scriptureFields: [
        { token: "{scripture_text}",            label: "Text",             hint: "the verses, each led by its number", bind: "text" },
        { token: "{scripture_verses}",          label: "Verse numbers",    hint: "just the numbers: 1-3" },
        { token: "{scripture_reference}",       label: "Reference",        hint: "Genesis 1:1-3 — the verses on this slide", bind: "ref" },
        { token: "{scripture_reference_full}",  label: "Reference (all)",  hint: "the whole selection, on every slide" },
        { token: "{scripture_reference_last}",  label: "Reference (last)", hint: "the whole selection, on the last slide only" },
        { token: "{scripture_book}",            label: "Book",             hint: "Genesis" },
        { token: "{scripture_book_abbr}",       label: "Book (abbr)",      hint: "GEN" },
        { token: "{scripture_chapter}",         label: "Chapter",          hint: "1" },
        { token: "{scripture_name}",            label: "Bible version",    hint: "the Bible's name, without its bracketed part" },
        { token: "{scripture_number}",          label: "Number slot",      hint: "the number is drawn in front of its verse — leave it in front of Text" },
        { token: "{meta_copyright}",            label: "Copyright",        hint: "the version's copyright line" }
    ]

    // The Table's own. A sermon is cited as a date code plus a title, and its
    // body is a run of PARAGRAPHS, not verses — so every field here says so.
    // {table_title}/{table_year} exist because the shipped Table layouts had
    // nowhere to put the sermon's own name: the citation line mixes the code,
    // the title and the paragraph number into one string, and a design with a
    // separate slot for the title needs the title alone.
    readonly property var tableFields: [
        { token: "{table_text}",              label: "Text",              hint: "the paragraphs, each led by its number", bind: "text" },
        { token: "{table_paragraphs}",        label: "Paragraph numbers", hint: "just the numbers: 1-3" },
        { token: "{table_reference}",         label: "Reference",         hint: "47-0412 - Faith Is The Substance 1-3 — the paragraphs on this slide", bind: "ref" },
        { token: "{table_reference_full}",    label: "Reference (all)",   hint: "the whole selection, on every slide" },
        { token: "{table_reference_last}",    label: "Reference (last)",  hint: "the whole selection, on the last slide only" },
        { token: "{table_citation}",          label: "Citation",          hint: "the whole citation line: 47-0412 - Faith Is The Substance", bind: "ref" },
        { token: "{table_code}",              label: "Code",              hint: "the date code alone: 47-0412" },
        { token: "{table_code_paragraphs}",   label: "Code + paragraphs", hint: "the date code and this slide's paragraph numbers: 47-0412 1-3" },
        { token: "{table_year}",              label: "Year",              hint: "the year alone: 1947" },
        { token: "{table_title}",             label: "Title",             hint: "the sermon's title alone: Faith Is The Substance" },
        { token: "{table_number}",            label: "Number slot",       hint: "the number is drawn in front of its paragraph — leave it in front of Text" }
    ]

    // What the Metadata menu lists: group name -> fields. Read in order.
    //
    // `family` is which content family the group BELONGS to, in the engine's own
    // words (BookFill::ContentFamily — the same two tags a slide carries). It is
    // what the boot check below asks the engine with.
    readonly property var groups: [
        { name: "Scripture",  family: "scripture", fields: root.scriptureFields },
        { name: "The Table",  family: "table",     fields: root.tableFields }
    ]

    // ---- the cross-check with the engine ----------------------------------------
    //
    // These two lists are a hand-written mirror of the engine's vocabulary
    // (BookFill's Resolve), and a mirror drifts: a field the engine cannot fill for
    // its own family kept sitting in the menu looking implemented — {meta_copyright}
    // and {scripture_book_abbr} on a sermon — while a token the engine learned and
    // the menu never offered stayed invisible. The unit test beside the vocabulary
    // caught the second kind; this catches the first AT BOOT, in the app that would
    // show the empty box, and names the chips. A family that genuinely has no answer
    // ({scripture_name} in a sermon's list) must not be listed in that family's
    // group at all — every family's own list has to be fully answerable, which is
    // exactly what the engine is asked here.
    function unansweredFields() {
        const missing = []
        for (const group of root.groups)
            for (const field of group.fields)
                if (!TemplateLibraryService.metadataTokenResolves(group.family, field.token))
                    missing.push(field.token + " (" + group.name + ")")
        return missing
    }
    Component.onCompleted: {
        const missing = root.unansweredFields()
        if (missing.length > 0)
            console.error("MetadataFields: the engine's fill has no answer for " + missing.join(", ")
                          + " — those chips would render nothing; see BookFill::Resolve")
    }

    // Flat list (the drag payload and the "is this a metadata token" tests);
    // each entry keeps its group so a chip can be drawn and sorted alike.
    readonly property var all: {
        const out = []
        for (const g of root.groups)
            for (const f of g.fields)
                out.push({ token: f.token, label: f.label, hint: f.hint, group: g.name,
                           bind: f.bind !== undefined ? f.bind : "" })
        return out
    }

    // The registry entry for a literal token ("{table_text}"), or null. The
    // palette inserts by TOKEN (a click and a drag both carry the literal), so
    // this is how the insert finds the field's slot and built-in style instead
    // of guessing them from the spelling.
    function fieldForToken(token) {
        const want = String(token === undefined || token === null ? "" : token)
        for (const f of root.all)
            if (f.token === want)
                return f
        return null
    }

    // IS THIS TEXT A PLACEHOLDER — a token standing in for a value — rather than
    // content? The type-fitting solvers ask this (EditScreen.recomputeFit and
    // DesignPreview.solveFit) because a token is NOT the text the frame will draw:
    // "{table_code_paragraphs}" is 23 characters standing in for "47-0412 3".
    // Sizing the type to the TOKEN made the canvas show a size the output never
    // uses (measured: token squeezed to ~18 stage units on the canvas, the value
    // drawn at ~26 in the NDI frame, same box and centre). Registered tokens
    // answer the registry; a hand-typed `{anything}` answers the shape, so a token
    // the registry does not know still behaves like the ones it does.
    function isToken(text) {
        const want = String(text === undefined || text === null ? "" : text).trim()
        if (want === "")
            return false
        for (const f of root.all)
            if (f.token === want)
                return true
        return /\{[a-z0-9_]+\}/i.test(want)
    }

    // ---- ONE reading of a block's line height and letter spacing -----------
    //
    // `lineHeight` is QML's Text.lineHeight: a PROPORTIONAL multiplier of the
    // font's OWN natural line height. Three surfaces draw the same block — the
    // Edit canvas (EditScreen's itemTextLabel), every preview tile
    // (DesignPreview) and the engine's outputs (SceneBuilder::AddStageBlocks) —
    // and each one used to supply its own default. The Text panel showed 0 for
    // an item that never set the field (its `?? 0`), while the canvas and the
    // previews drew `?? 1.2` and the engine drew its own 1.2: the panel said
    // "Height 0" and the screen showed ~1.2x the font's natural spacing, with no
    // way to see — let alone change — the number actually in force. That is the
    // reported "the height is set to zero and look at the line height": the
    // value was never connected to what was drawn.
    //
    // 0 is NOT a line height — it would stack every line on one baseline, and in
    // QML it is a degenerate Text.lineHeight outright. It (and a missing/NaN
    // value) therefore means THE FONT'S OWN spacing, Qt's own Text default.
    // The shared TemplateTextMetrics helper owns the default and normalization.
    readonly property real naturalLineHeight: EngineBridge.templateLineHeightMultiplier(0)
    function lineHeight(meta) {
        const v = meta ? meta.lineHeight : undefined
        return EngineBridge.templateLineHeightMultiplier(Number(v))
    }

    // The same rule for letter spacing: an ADDITIVE px value whose neutral is 0
    // (the canvas, the previews and the engine all draw 0 when the field is
    // absent) — but the panel showed 1.0 for an item that never set it, so it
    // read as a value nothing on screen was using. Shown and painted from here.
    readonly property real naturalLetterSpacing: 0
    function letterSpacing(meta) {
        const v = meta ? meta.letterSpacing : undefined
        const n = Number(v)
        return (v === undefined || v === null || isNaN(n)) ? root.naturalLetterSpacing : n
    }

    // The same additive px-value rule for the gap after every SPACE. QML's Text
    // carries it as font.wordSpacing, the engine as TextStyle::wordSpacing, and
    // both neutral at 0 — read here so the panel, the fit solver, the canvas
    // label, the tile previews and the output all state one number.
    readonly property real naturalWordSpacing: 0
    function wordSpacing(meta) {
        const v = meta ? meta.wordSpacing : undefined
        const n = Number(v)
        return (v === undefined || v === null || isNaN(n)) ? root.naturalWordSpacing : n
    }

    function textFont(meta, pixelSize) {
        const m = meta || ({})
        const weight = m.bold === true ? Font.Bold
            : m.fontWeight === "Regular" ? Font.Normal
            : m.fontWeight === "SemiBold" ? Font.DemiBold
            : m.fontWeight === "Bold" ? Font.Bold : Font.Medium
        const capitalization = m.textCase === "upper" ? Font.AllUppercase
            : m.textCase === "lower" ? Font.AllLowercase
            : m.textCase === "capitalize" ? Font.Capitalize : Font.MixedCase
        return Qt.font({
            family: FontService.resolve(m.fontFamily),
            pixelSize: pixelSize,
            weight: weight,
            italic: m.italic === true,
            capitalization: capitalization,
            underline: m.underline === true,
            strikeout: m.strikethrough === true,
            letterSpacing: root.letterSpacing(m),
            wordSpacing: root.wordSpacing(m)
        })
    }

    // The same additive px-value rule for the air BETWEEN line boxes —
    // FreeShow's "line spacing" row, separate from the line height above: the
    // height sizes a line box, this opens a gap before the next one. Neutral 0.
    readonly property real naturalLineSpacing: EngineBridge.templateLineSpacing(0)
    function lineSpacing(meta) {
        const v = meta ? meta.lineSpacing : undefined
        return EngineBridge.templateLineSpacing(Number(v))
    }

    // THE LINE STEP QML MUST DRAW WITH. QML's Text has no additive gap — only a
    // lineHeight MULTIPLIER of the font's own line box — so a block whose gap is
    // 4px is drawn at multiplier (natural × lineHeight + gap) / natural:
    // naturalHeightPx is that font's own line box at the size being drawn
    // (ascent + descent from a TextMetrics beside the Text). With no gap the
    // multiplier is exactly lineHeight, so this never perturbs an item that
    // does not use the control.
    function lineHeightWithGap(meta, naturalHeightPx) {
        const lh = root.lineHeight(meta)
        const gap = root.lineSpacing(meta)
        return EngineBridge.templateLineHeightWithGap(naturalHeightPx, lh, gap)
    }

    // The typed case, applied to a string — QML's font.capitalization does this
    // for a PLAIN Text, and the band layer below is rich text, where it does
    // nothing: an upper-cased label would otherwise band at the lower-case
    // widths of the same words.
    function applyTextCase(text, mode) {
        const s = String(text === undefined || text === null ? "" : text)
        if (mode === "upper")
            return s.toUpperCase()
        if (mode === "lower")
            return s.toLowerCase()
        if (mode === "capitalize")
            return s.replace(/(^|\s)(\S)/g, (m, pre, ch) => pre + ch.toUpperCase())
        return s
    }

    // THE PER-LINE BACKGROUND BAND, as rich text for a transparent Text drawn
    // UNDER the label. Qt paints a span's background per line FRAGMENT, so one
    // span around a paragraph bands every visual line it wraps onto, at exactly
    // that line's own width — the same wrapping and the same font as the label
    // beside it, which is why this is a second Text and not a hand-computed
    // list of rectangles (a rectangle list would be a second wrap engine, and
    // the two would disagree the first time a word did not fit). Empty when no
    // band colour is set, so an item that never used the control draws nothing.
    function lineBandText(text, color, textCase) {
        const hex = String(color === undefined || color === null ? "" : color).trim()
        if (hex === "" || hex === "transparent")
            return ""
        const escape = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        return root.applyTextCase(text, textCase)
            .split("\n")
            .map((line) => '<span style="background-color:' + hex + '">' + escape(line) + "</span>")
            .join("<br/>")
    }

    // ---- line boxes, for a ROUNDED per-line band -------------------------
    // FreeShow's `lineRadius` needs one rounded rectangle per line, and QML's
    // rich text can band a line but not round it. These boxes are those lines:
    // a greedy word wrap measured with the SAME font the label draws, so the
    // rectangles land on the words Qt wrapped. `maxWidth` is the label's own
    // content width; a line whose text already fits is its own line.
    //
    // Only the ROUNDED path uses this. The default (radius 0) band is painted by
    // Qt's own rich-text fragment backgrounds and never asks these questions.
    property var _lineMetrics: null
    function lineBoxes(text, maxWidth, font, caseMode) {
        if (!(maxWidth > 0))
            return []
        if (!root._lineMetrics) {
            // A measurer that cannot be created must not take the band with it:
            // returning no boxes makes the caller fall back to the rich-text
            // band (square corners) rather than to no band at all.
            try {
                root._lineMetrics = Qt.createQmlObject('import QtQuick; TextMetrics { }', root, "lineBoxMetrics")
            } catch (e) {
                console.warn("MetadataFields: no TextMetrics for rounded line bands — " + e)
                return []
            }
        }
        const m = root._lineMetrics
        m.font = font
        const advance = function (t) {
            m.text = t
            return m.advanceWidth
        }
        const s = root.applyTextCase(text === undefined || text === null ? "" : text, caseMode)
        const lines = []
        // Paragraphs are hard breaks; only the text inside one is wrapped.
        for (const para of s.split("\n")) {
            let current = ""
            const tokens = para.match(/\s+|\S+/g) || []
            for (const token of tokens) {
                const candidate = current + token
                if (current !== "" && advance(candidate) > maxWidth) {
                    lines.push(current.replace(/\s+$/, ""))
                    current = /^\s+$/.test(token) ? "" : token
                } else {
                    current = candidate
                }
            }
            lines.push(current.replace(/\s+$/, ""))
        }
        const out = []
        for (const line of lines)
            out.push({ text: line, width: advance(line) })
        return out
    }

    // The band's corner radius (FreeShow's lineRadius): 0 = square corners, the
    // rich-text path. Neutral 0, so an item that never named it is unchanged.
    readonly property real naturalLineRadius: 0
    function lineRadius(meta) {
        const v = meta ? meta.lineRadius : undefined
        const n = Number(v)
        return (v === undefined || v === null || isNaN(n) || n <= 0) ? root.naturalLineRadius : n
    }

    // "Text on one line": the canvas draws the item with Text.NoWrap, and the
    // engine folds the content's newlines to spaces before laying it out. One
    // reading of the flag, so no surface can honour it differently.
    function oneLine(meta) {
        return meta ? meta.oneLine === true : false
    }
}
