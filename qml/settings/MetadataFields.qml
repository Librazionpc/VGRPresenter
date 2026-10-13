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
}
