import QtQuick
import VGRPresenterUI
// The app's action-named keyboard binding (qml/shortcuts/Shortcut.qml), imported
// AFTER QtQuick so `Shortcut {}` below is this type rather than the QtQuick one:
// the chords themselves live in qml/settings/Hotkeys.qml.
import "../shortcuts"

// The REFERENCE-LIBRARY pane — the Scripture tab's UI, generalized so the
// Scripture tab and The Table tab are the SAME component driven by different
// adapters (FreeShow's Scripture.svelte layout):
//
//   Bibles/Collections | Books/Years | Chapters/Sermons | Verses/Paragraphs  | Preview
//                                                                              | Template (Scripture only)
//   [King James Version: Genesis 1:2]                    ( < > search )       | "Convert to show"
//
// IT OWNS NO DATA and names no service: everything (sources, books, chapters,
// verses, reference lookup, search, preview slides) comes from the `adapter`
// — a small object the thin per-tab wrappers (ScripturePane / TheTablePane)
// hand in, backed by their engine service. The adapter contract:
//   sources()                 -> [{ id, name }]                (Bibles / The Table)
//   loading()                 -> bool
//   books(sourceId)           -> [{ id, name, chapters: [...] }]
//   chapter(src, bookId, n)   -> [{ number, text }]
//   reference(book, ch, nums) -> "1953 12:3-5"
//   resolve(text, srcId)      -> { bookId, chapter, verseStart, verseEnd } | {}
//   search(text, srcId)       -> [{ reference, bookId, chapter, verse, snippet }]
//   preview(src, bookId, ch, nums) -> { blocks, background, hasValues, slideCount }
//   importNew()               -> bool (the sidebar's "+ New ..." action)
//   addFolderLabel / importFolder() / importing() / progress()  — the optional bulk folder import
//   addLabel / newEntryLabel / sidebarLabel / emptyText — the tab's wording
// Scripture-only extras (template panel, convert-to-show options) are gated by
// `supportsTemplates`: The Table passes false and gets the simpler chrome.
// same template placeholders, the same options (verse numbers, verses on individual lines, dividing long verses, smart
// split), the same reference wording. "Convert to show" hands those slides to the Edit screen.
Item {
    id: root

    // LAYOUT PROBE (VGR_LAYOUT_TEST): every LIST this pane owns reports how many delegates it
    // built and threw away, so a rebuild on a pane switch is visible as builds AND drops moving
    // together, and so a list that builds its whole model at once shows up as builds == rows.
    //   verse  — the passage rows (a ListView)
    //   hit    — the pill search's hits (a ListView)
    //   match  — The Table's citation matches (a ListView)
    //   peek   — the hovered sermon's paragraphs in the peek panel (a ListView)
    property int verseRowBuilds: 0
    property int verseRowDrops: 0
    property int hitRowBuilds: 0
    property int hitRowDrops: 0
    property int matchRowBuilds: 0
    property int matchRowDrops: 0
    property int peekRowBuilds: 0
    property int peekRowDrops: 0
    // HOW OFTEN A ROW'S OWN BINDINGS EVALUATE (the scroll-cost probe — the layout probe's
    // scroll phase, tools/selftest/run_layout_probe.sh). Builds and drops say a row was
    // created; these say how much binding work each row did, which is the other half of a
    // scroll's cost: a scroll that builds nothing but still drops frames is a list whose
    // IN-VIEW rows are re-evaluating, and one that builds a row per frame is paying the row's
    // own binding set on every step.
    //
    // A PLAIN JS OBJECT, deliberately, and not a QML `property int`: incrementing a QML
    // property from inside a binding is a property WRITE during evaluation, which QML reports
    // as a binding loop (the first cut of this did exactly that — "Binding loop detected for
    // property text/oneLine" at every row in the pane). A field on a JS object is not a QML
    // property: it cannot notify, so nothing re-evaluates because of it, and the probe reads
    // the totals on demand (it never binds to them). The increment sits in the VALUE
    // expression of the binding it counts — QML caches a binding's result, so READS of the
    // bound property do not count, only real evaluations do.
    //   verse   — the passage row's text (its `oneLine` flattening: a regex over the
    //             paragraph, run per row construction and per model change)
    //   book    — a book row's label text
    //   chapter — a chapter row's label text (a sermon title in The Table)
    //   source  — a source (Bible / sermon folder) row's label text
    readonly property var probeEvals: ({ verse: 0, book: 0, chapter: 0, source: 0 })

    // The lists a scenario scrolls BY NAME. The same pane class serves two tabs, so the
    // names are per-TAB ("selfTestVersesList-table" / "selfTestVersesList-scripture") —
    // an objectName lookup finds the first match in the tree, and a name shared by two
    // panes would resolve to whichever one happens to come first, hidden or not.
    readonly property string paneTag: (root.adapter && root.adapter.contentType) ? root.adapter.contentType : "pane"

    // The tab bar's search box: a reference ("John 3:16") jumps there, anything else narrows the book list.
    property string filter: ""

    // THE ADAPTER (see the header for the contract). The wrappers set this;
    // everything data-shaped below reads it, never a service directly.
    property var adapter: null
    // Scripture-only chrome: the template panel and its options button.
    property bool supportsTemplates: true
    // The Table's sermon templates have no scripture-style placeholders to fall back to, so "Use default
    // template" (which just clears the pick back to the built-in one) doesn't apply there - Scripture keeps it.
    property bool showUseDefaultTemplateButton: true

    signal templateEditRequested(string templateId)
    // "Convert to show": the show's name and its slides ([{ title, background, blocks }]), from the engine.
    signal convertToShowRequested(string name, var slides)
    // GO LIVE: the passage's slides go on air NOW (FreeShow's playScripture):
    // name + the same slide shape convertToShowRequested carries. Fired by a
    // verse double-click and the floating play button; the host (or the live
    // service directly) decides where the slides render.
    signal goLiveRequested(string name, var slides)
    // A suggestion row from the tab search box was accepted — carried up so the host can
    // route it back into the pane that offered it (see applySuggestion).
    signal suggestionChosen(var ref)

    // ---- what is open ----
    property string sourceId: ""
    property var books: []
    property int bookIndex: 0
    property int chapterNumber: 1
    property var chapterVerses: []       // [{ number, text }]
    property int chapterToken: 0
    property bool chapterLoading: false
    property bool chapterSelectFirst: false
    // A step BACK into the previous passage picks its LAST paragraph, not its first
    // (the row the reader walked in from). Applied when the chapter's rows land, so
    // the step never reads a list that has not been filled yet.
    property bool chapterSelectLast: false
    property double chapterStartedAt: 0
    property var pendingGoTo: null
    property var selected: []            // verse numbers, ascending
    property int anchorVerse: 0
    // THE NAME LiveOutputService WAS STAGED/GONE LIVE UNDER, captured at the
    // moment of the pick — NOT root.referenceText read live, which (for
    // Scripture) is itself DERIVED FROM root.selected ("Genesis 1:1" ->
    // "Genesis 1:2" as the pick changes) and would drift the instant
    // followLiveStep below updates root.selected to follow a step,
    // breaking its OWN ownership check on the very next change. The
    // Table's reference string happens to be the sermon's fixed citation
    // line regardless of which paragraph is selected, so it would not have
    // shown this — Scripture would have. See followLiveStep.
    property string lineStepOwnerRef: ""
    property bool optionsOpen: false
    property bool searching: false
    // EXACT WORDS (the padlock at the far end of the pill's search field — user
    // call: "add a pad lock icon at the extreme end of the search so once it lock
    // it means i want exact word search"). Locked, the typed words must appear
    // VERBATIM as whole words: no completion ("friend" no longer finds
    // "friends"), no inner substring, no typo correction. The spelling hint
    // (which exists to offer those corrections) stays quiet while locked.
    property bool exact: false
    // CHAPTER LOCK (user call: "add a chapter lock search ... if i turn the lock
    // for the book all my search goes against that book alone and not all the
    // books"): on, the search covers only the passage this tab has OPEN — the
    // chapter being read in a Bible, the sermon being read in The Table — so a
    // word is looked for where the user is actually reading instead of across the
    // whole library. Scripture and The Table are the only tabs with books, so
    // they are the only ones that show the pill (adapter.supportsChapterLock).
    property bool scopeLock: false
    // The scope the lock currently names, resolved from what is open: "" / 0
    // mean "not locked", which is what every adapter without the feature reads.
    readonly property bool supportsChapterLock: !!(root.adapter && root.adapter.supportsChapterLock)
    readonly property string lockedBookId: (root.scopeLock && root.book) ? String(root.book.id) : ""
    readonly property int lockedChapter: root.scopeLock ? root.chapterNumber : 0
    // What the lock names, for the field's own hint ("Search in Revelation 1"):
    // the pane's own two column labels — a Bible book plus its chapter number,
    // or The Table's sermon citation (its year books carry chapterTitles).
    readonly property string scopeName: {
        const b = root.book
        if (!b)
            return ""
        const titles = b.chapterTitles || []
        const idx = (b.chapters || []).indexOf(root.chapterNumber)
        if (idx >= 0 && titles[idx] !== undefined && String(titles[idx]) !== "")
            return String(titles[idx])
        return b.name + " " + root.chapterNumber
    }
    property var searchResults: []
    // The books / chapters / preview columns' widths - each one dragged with its own SplitHandle (double-click puts it back). The verses
    // column takes whatever is left, so it's the one that gives way as the others grow. Remembered across restarts (shared between
    // Scripture and The Table, since it is the same pane): the initial value is whatever was last saved, and every later plain
    // assignment (a drag, a reset, the titles auto-widen below) breaks that binding, same as any other controlled QML property - only
    // `saveColumnWidths` below writes back out, on a short idle after the value stops changing rather than on every drag frame.
    property real booksColWidth: Number(SettingsService.values["session.referencePaneBooksWidth"] ?? 150)
    property real chaptersColWidth: Number(SettingsService.values["session.referencePaneChaptersWidth"] ?? 52)
    property real previewColWidth: Number(SettingsService.values["session.referencePanePreviewWidth"] ?? Math.max(320, Math.min(420, root.width * 0.3)))
    // During The Table search, the year/sermon pickers and design preview are
    // unnecessary; the result list gets the full reading area.
    readonly property bool compactTableSearch: !!(root.adapter
        && root.adapter.contentType === "table" && root.searching)
    // Whether a width was already remembered before this pane ever auto-widened the chapters column for titles (below) - if so, that
    // remembered width IS the user's choice, and the auto-widen must not override it.
    readonly property bool chaptersWidthRemembered: SettingsService.values["session.referencePaneChaptersWidth"] !== undefined
    Timer {
        id: saveColumnWidthsTimer
        interval: 500
        onTriggered: {
            SettingsService.setValue("session.referencePaneBooksWidth", Math.round(root.booksColWidth))
            SettingsService.setValue("session.referencePaneChaptersWidth", Math.round(root.chaptersColWidth))
            SettingsService.setValue("session.referencePanePreviewWidth", Math.round(root.previewColWidth))
        }
    }
    onBooksColWidthChanged: saveColumnWidthsTimer.restart()
    onChaptersColWidthChanged: saveColumnWidthsTimer.restart()
    onPreviewColWidthChanged: saveColumnWidthsTimer.restart()
    // An inactive row's text: dimmer than the bright, active-row text but still a tint of it, not the muddier grey `Theme.textSecondary`
    // reads as against these dark columns - the same "tint the real colour" idiom AppButton/NavItem already use elsewhere.
    readonly property color textDim: Qt.rgba(Theme.textPrimary.r, Theme.textPrimary.g, Theme.textPrimary.b, 0.62)
    // Bumped whenever the engine's side changes (a Bible finished installing, the template or an option changed), so the bindings that
    // ask the engine again notice.
    property int engineRevision: 0

    // ---- User data (notes & highlights, docs/specs/24 §User Data) -----------
    // Bumped whenever a note or highlight landed in the engine, so the per-row
    // bindings re-ask the adapter. All user data is optional adapter surface
    // (an adapter without it simply shows no stars, no pencils, no drawer).
    property int userRevision: 0
    readonly property var userApi: (root.adapter && root.adapter.userData) ? root.adapter.userData : null
    readonly property bool hasUserData: !!(root.userApi && root.userApi.chapterUserData && root.userApi.notes)
    // THE batched read: one adapter call per open chapter, re-asked whenever a
    // note/highlight lands or the chapter changes — the rows consult this map
    // instead of each re-resolving a reference through the engine per repaint.
    readonly property var chapterMarks: {
        root.userRevision
        if (!root.hasUserData || !root.book)
            return ({}).valueOf()
        return root.userApi.chapterUserData(root.sourceId, root.book.id, root.chapterNumber) || ({}).valueOf()
    }
    function markFor(v) {
        const m = root.chapterMarks[String(v)]
        return m ? m : ({}).valueOf()
    }
    property bool notesOpen: false
    property var notesRows: []
    // The verse whose note editor is open (0 = none) — the editor is an inline
    // row under its verse; one at a time.
    property int noteVerse: 0
    // Where we are, as one string: leaving the chapter closes an open note editor.
    readonly property string chapterStamp: root.sourceId + "|" + (root.book ? root.book.id : "") + "|" + root.chapterNumber
    onChapterStampChanged: root.noteVerse = 0

    function refOfVerse(v) { return root.book ? root.adapter.reference(root.book.name, root.chapterNumber, [v]) : "" }

    function refreshNotesRows() {
        root.notesRows = root.hasUserData ? root.userApi.notes(root.sourceId) : []
    }

    function toggleHighlight(v) {
        if (!root.hasUserData || !root.book)
            return
        // The engine's SetHighlight is idempotent — write the inverted state
        // straight from the batched map, no read-then-write race window.
        root.userApi.setHighlighted(root.sourceId, root.refOfVerse(v), !root.markFor(v).highlight)
    }

    function openNoteEditor(v) {
        if (!root.hasUserData || !root.book)
            return
        root.noteVerse = v
        // The draft TEXT is seeded by the editor TextInput itself when it
        // becomes visible: it lives inside the verse-row delegate, and QML
        // ids don't cross delegate boundaries — reading `noteInput` here
        // threw "ReferenceError: noteInput is not defined" and aborted the
        // whole call (the reported notes bug).
        if (root.notesOpen)
            root.refreshNotesRows()
    }

    function saveNoteDraft(draft) {
        if (!root.hasUserData || root.noteVerse <= 0) {
            root.noteVerse = 0
            return
        }
        root.userApi.setNote(root.sourceId, root.refOfVerse(root.noteVerse), draft)
        if (root.notesOpen)
            root.refreshNotesRows()
        root.noteVerse = 0
    }

    // A drawer row was clicked: open the passage the note sits on and pick its paragraph.
    function jumpToNote(n) {
        if (!n)
            return
        root.goTo({ bookId: n.bookId, chapter: n.chapter, verseStart: n.verse, verseEnd: n.verse })
        root.notesOpen = false
    }

    // The wrapper bumps its adapter's userDataRevision when the service signals
    // a note/highlight landing; every per-row re-read hangs off userRevision.
    Connections {
        target: root.adapter
        function onUserDataRevisionChanged() {
            root.userRevision++
            if (root.notesOpen)
                root.refreshNotesRows()
        }
    }

    readonly property var book: root.books.length > root.bookIndex ? root.books[root.bookIndex] : null
    readonly property var sources: {
        root.engineRevision   // re-checked whenever the engine's side changes (e.g. a folder import just finished)
        return root.adapter ? root.adapter.sources() : []
    }
    readonly property var currentSource: {
        for (let i = 0; i < root.sources.length; ++i)
            if (root.sources[i].id === root.sourceId)
                return root.sources[i]
        return null
    }
    readonly property string referenceText: root.book ? root.adapter.reference(root.book.name, root.chapterNumber, root.selected) : ""
    readonly property var preview: {
        root.engineRevision
        return root.book ? root.adapter.preview(root.sourceId, root.book.id, root.chapterNumber, root.selected) : { blocks: [], background: "", reference: "", hasValues: true, slideCount: 0, slides: [] }
    }
    property int previewSlideIndex: 0
    readonly property var previewSlides: root.preview.slides || []
    // The pieces the preview is showing RIGHT NOW. The rows no longer carry a slide index
    // of their own (see mainRows), so "is this row the selected one" is answered in the
    // identity both sides have: the paragraph and the piece of it.
    readonly property var previewSlideParts: {
        const slides = root.previewSlides
        if (slides.length === 0)
            return []
        return slides[Math.max(0, Math.min(root.previewSlideIndex, slides.length - 1))].parts || []
    }
    // Is this row the one the pick is on? Undivided: as soon as its paragraph is picked.
    // Divided: while its own piece is the piece the preview shows — so walking a long
    // paragraph part by part with ‹ › (stepSlide) moves the highlight with it.
    function rowIsActive(number, partIndex, isSplit) {
        if (root.selected.indexOf(number) < 0)
            return false
        if (!isSplit)
            return true
        for (const part of root.previewSlideParts)
            if (Number(part.number) === number && Number(part.partIndex ?? 0) === partIndex)
                return true
        return false
    }

    // Put a paragraph ROW on screen, by its index in mainRows. The list is
    // VIRTUALIZED (see the ListView), so "scroll to it" and "build it" are one
    // operation, and positionViewAtIndex is the single call that does both — there is
    // no item to measure or move for a row that does not exist yet.
    //
    // Deferred a turn deliberately: a positioning asked for in the same tick as a
    // model change would be measured against a list that has not laid out yet, and the
    // chapter's own reveal asks for one immediately after assigning the rows. Passed as
    // a closure so the list settles first, and so a caller may reveal several rows in
    // one go without the first request winning.
    function revealRow(index) {
        if (index < 0 || index >= root.mainRows.length)
            return
        Qt.callLater(function () { versesFlick.positionViewAtIndex(index, ListView.Beginning) })
    }

    // Put a row in the MIDDLE of the list (user call: "sometimes the selected list
    // that is showing doesn't stay in the centre"). Used when a remembered place is
    // restored, where the point is to see the paragraph that was being read with the
    // passage around it — not to pin it to the top edge.
    function revealRowCentered(index) {
        if (index < 0 || index >= root.mainRows.length)
            return
        Qt.callLater(function () { versesFlick.positionViewAtIndex(index, ListView.Center) })
    }

    // Keep the row of (passage, piece) on screen, and re-centre it only when it
    // has to move. The ‹ › pair — and a pick's own slide walk — moves the
    // highlight one row at a time; scrolling to the middle on EVERY step slid the
    // whole list under the eye on each press, which reads as motion sickness ("let
    // it get to one end of the list before you move back to the centre"). So the
    // list stays put while the walking row is fully visible, and only when the row
    // reaches an edge does the view snap it back to the middle.
    //
    // Resolved inside the deferred call because the row INDEX is not stable yet
    // when a step lands right after a rows delivery — and positionViewAtIndex is
    // also what realizes a row the list has virtualized away (no item to measure
    // means it cannot be on screen).
    // THE ROW THIS FOLLOW LAST PUT ON SCREEN — the anchor for the stand-down
    // rule below. -1 means "nothing followed yet in this chapter".
    property int followedRowIndex: -1
    function rowFullyVisible(row) {
        if (!row)
            return false
        const pad = 6
        return row.y - pad >= versesFlick.contentY
            && row.y + row.height + pad <= versesFlick.contentY + versesFlick.height
    }
    // `force` is for navigation the OPERATOR asked for (opening a chapter,
    // clicking a continuation row, the preview's own arrows): those always move
    // the list. The follow (followLiveStep) is the one caller that defers to
    // where the operator has scrolled — see the stand-down rule below.
    function centerRow(number, partIndex, force) {
        Qt.callLater(function () {
            const index = root.rowIndexForPart(number, partIndex ?? 0)
            if (index < 0)
                return
            const row = versesFlick.itemAtIndex(index)
            if (row && root.rowFullyVisible(row)) {
                root.followedRowIndex = index
                return          // already fully on screen: do not move the list
            }
            // THE LIST BELONGS TO THE OPERATOR WHILE THEY READ AHEAD. This used
            // to re-centre on the active row no matter where the operator had
            // scrolled, so reading ahead and then advancing yanked the list back
            // to the row that was moving — reported as "it scrolls to two … then
            // it refreshes two and I lose my place; it moves from bottom to top".
            // Follow only while the list is TRACKING the reading: the row this
            // same follow showed last time is still on screen (or still gliding
            // to it). Once the active row is off screen because the operator's
            // own scroll put it there, stand down — and the moment they bring
            // the active row back into view themselves, the next step finds it
            // visible and tracking resumes on its own.
            if (force !== true) {
                const previous = root.followedRowIndex >= 0
                    ? versesFlick.itemAtIndex(root.followedRowIndex) : null
                const tracking = root.followedRowIndex < 0
                    || centerRowScroll.running || root.rowFullyVisible(previous)
                if (!tracking)
                    return
            }
            if (row) {
                // (The "already on screen" test was made above, before the
                // stand-down rule: one copy of that rule, not two.)
                // GLIDES to the row instead of jumping (user call, made twice:
                // "instead of jumping between slides, it could animate the
                // progress between slides... gradually move... easily track
                // things more easily") — the same ListView.Center target
                // positionViewAtIndex would land on, clamped the same way it
                // clamps internally, animated instead of set outright.
                const targetY = Math.max(0, Math.min(
                    versesFlick.contentHeight - versesFlick.height,
                    row.y + row.height / 2 - versesFlick.height / 2))
                centerRowScroll.to = targetY
                centerRowScroll.restart()
                root.followedRowIndex = index
                return
            }
            // No delegate yet (virtualized away, off the cache buffer) — nothing
            // to glide FROM, so land directly the way positionViewAtIndex always
            // has; there is no "from" position for an animation to start at.
            versesFlick.positionViewAtIndex(index, ListView.Center)
            root.followedRowIndex = index
        })
    }
    NumberAnimation {
        id: centerRowScroll
        target: versesFlick
        property: "contentY"
        duration: 220
        easing.type: Easing.InOutQuad
    }

    // The row a remembered place names. A PICK is centred (the paragraph being read,
    // with the passage around it, which is what "it doesn't stay in the centre" asked
    // for); a place with no pick is the top of the list, and it goes back to the top.
    function revealRemembered(at, picked) {
        if (picked && picked.length > 0) {
            let index = root.mainRows.findIndex((row) => Number(row.number) === Number(picked[0])
                                                        && row.continuation !== true)
            if (index < 0 && at)
                index = root.mainRows.findIndex((row) => Number(row.number) === Number(at.number)
                                                            && Number(row.partIndex ?? 0) === Number(at.partIndex ?? 0))
            root.revealRowCentered(index)
            return
        }
        let index = -1
        if (at)
            index = root.mainRows.findIndex((row) => Number(row.number) === Number(at.number)
                                                        && Number(row.partIndex ?? 0) === Number(at.partIndex ?? 0))
        root.revealRow(index)
    }

    // ---- the rows did not survive the tab (the reported "the split disappears …
    // I have to toggle Divide long paragraphs again") ------------------------------
    // The list shows the passage's own cut (mainRows carries `split`), and that cut is
    // made when the chapter is READ. A pane that comes back after a template edit, a
    // reload, or an option written behind it can therefore hold rows cut from what the
    // tab no longer says — or, when the cut landed while the background pass was being
    // rebuilt, no cut at all. The passage is read again with the options the tab says
    // now, and the reader's place is put back, so the divide is on screen without
    // toggling it off and on.
    function rowsHaveSplit() {
        for (const verse of root.chapterVerses)
            if (verse.split && verse.split.length > 0)
                return true
        return false
    }
    function refillIfStale() {
        if (!root.book || !root.adapter || root.chapterVerses.length === 0)
            return
        const p = String(root.adapter.optionsPrefix || "")
        const wantsSplit = root.options[p + ".splitLongVerses"] === true
        // A chapter with nothing long enough to divide legitimately has no split rows,
        // so "missing" is only evidence ONCE per (passage, options): the re-read is
        // tried, and a second empty answer is the truth. Only The Table is checked —
        // its paragraphs are whole sermon thoughts, while a Bible's verses are short
        // by nature and would be re-read for nothing.
        const stamp = String(root.book.id) + ":" + root.chapterNumber + "@" + root.fillSignature
        const signatureMoved = root.rowsFillSignature !== ""
                                && root.rowsFillSignature !== root.fillSignature
        const missing = wantsSplit && root.adapter.contentType === "table" && !root.rowsHaveSplit()
        if (!signatureMoved && !missing)
            return
        if (root.refillTriedFor === stamp)
            return
        root.refillTriedFor = stamp
        root.restorePicked = root.selected.slice()
        root.restoreRow = root.topRowIdentity()
        root.openChapter(root.chapterNumber, false)
    }

    // The same by paragraph NUMBER — the identity the rest of the app speaks in. A
    // number is not an index once the chapter is divided (the fortieth paragraph can
    // be row 78: a divided paragraph contributes one row per piece), so it is resolved
    // through the model, to the row that HEADS the paragraph (a continuation hangs off
    // its head, and the head is the row worth showing).
    function rowIndexFor(number) {
        return root.mainRows.findIndex((row) => Number(row.number) === Number(number)
                                                 && row.continuation !== true)
    }
    function revealVerse(number) {
        root.revealRow(root.rowIndexFor(number))
    }

    // The row that shows one PIECE of a paragraph — the (paragraph, part) pair a
    // divided pick walks with ‹ › (4 -> 4-2 -> 4-3 are separate rows).
    function rowIndexForPart(number, partIndex) {
        return root.mainRows.findIndex((row) => Number(row.number) === Number(number)
                                                 && Number(row.partIndex ?? 0) === Number(partIndex))
    }

    // ---- the list's place, across a cut that lands behind the operator ---------
    // The rows of a passage GROW where its cut divided it (see mainRows), so a second rows
    // delivery — the rest of a long passage's cut arriving after the head was shown, or an
    // option moving under an open passage — renumbers every row below the first divided
    // one. Restoring the pixel offset would leave the operator reading a different
    // paragraph; restoring the ROW's identity (paragraph + piece) leaves them where they
    // were, which is what "the list keeps its place" has to mean. Nothing is done at all
    // while the list is at the top, which is where every open starts.
    function topRowIdentity() {
        if (root.mainRows.length === 0 || versesFlick.contentY <= 0)
            return null
        const at = versesFlick.indexAt(4, Math.round(versesFlick.contentY) + 4)
        if (at < 0 || at >= root.mainRows.length)
            return null
        const row = root.mainRows[at]
        return { number: Number(row.number), partIndex: Number(row.partIndex ?? 0) }
    }
    function restoreRowAnchor(anchor) {
        Qt.callLater(function () {
            const at = root.mainRows.findIndex((row) => Number(row.number) === anchor.number
                                                        && Number(row.partIndex ?? 0) === anchor.partIndex)
            if (at >= 0)
                versesFlick.positionViewAtIndex(at, ListView.Beginning)
        })
    }
    readonly property var currentPreviewSlide: root.previewSlides.length > 0
        ? root.previewSlides[Math.max(0, Math.min(root.previewSlideIndex, root.previewSlides.length - 1))]
        : null

    // THE LIVE READING FLOW, when this pane's own pick is what the tile is
    // actually showing — as opposed to currentPreviewSlide/preview above,
    // which are a fresh, STATIC adapter.preview() render of whichever one
    // paragraph/part is selected, with no flowText/flowScroll on it at all.
    // The Main Output tile's own scroll (OutputMonitorTile's DesignPreview)
    // reads those two fields straight from LiveOutputService's staged/on-air
    // slide — this pane's own preview box was reading from the static query
    // instead, so a step there had nothing to animate from and just redrew a
    // new snapshot each press (user report: "it['s] not scrolling[,] it
    // act[s] as a sliding"). null when nothing of ours is staged/on air yet
    // (browsing before any pick), so the box falls back to the static query.
    // IMPERATIVE, not a reactive binding — a readonly property var computing
    // this from stagedIsOurs()/live/onAirTitle was observed to go stuck at
    // null (confirmed via the FollowLiveStep + StagedIsOurs diagnostics:
    // LiveOutputService.stagedIsOurs() genuinely returns true for this
    // pane's own title, logged at the right moment, yet the SAME-INSTANT
    // read of the property inside followLiveStep() still saw null) — a QML
    // binding-evaluation-ORDER issue between this binding's own dependency
    // invalidation and whatever read triggered its re-run, not a data bug.
    // Setting it explicitly, at the exact same moment followLiveStep() itself
    // runs (and at the moment a pick is first staged), removes the
    // dependence on QML's binding engine ever re-running this expression at
    // the right time at all.
    property var liveFlowSlide: null
    // ONE WALK, ONE SET OF BLOCKS.
    //
    // A walk is a RUN of pages (LiveOutputService's reading run) drawn by one
    // reading block, so the page turning underneath it must NOT replace the
    // blocks: the service commits a new page at every boundary, and each commit
    // swapped the longest-text block for the next page's paragraph — engine.log
    // showed nine drawsFlow flip-flops in 30 ms across the two surfaces, with the
    // old page painting its static text while the new one painted the passage.
    // That swap is the blink, and it is the scroller being told about slides it
    // does not need to know about.
    //
    // So the map is pinned to the PICK for the length of the walk: it is replaced
    // only when the pick changes (its owner/title), when live state changes under
    // it, or when the design's background does — never because a page turned. The
    // words still move, because DesignPreview draws the run at readingScroll; the
    // reference and slide number are the pane's own, drawn around this box.
    property string liveFlowSlideKey: ""
    function refreshLiveFlowSlide() {
        if (root.lineStepOwnerRef === "") {
            root.liveFlowSlideKey = ""
            root.liveFlowSlide = null
            return
        }
        if (LiveOutputService.live) {
            if (LiveOutputService.onAirTitle !== root.lineStepOwnerRef) {
                root.liveFlowSlideKey = ""
                root.liveFlowSlide = null
                return
            }
        } else if (!LiveOutputService.stagedIsOurs(root.lineStepOwnerRef)) {
            root.liveFlowSlideKey = ""
            root.liveFlowSlide = null
            return
        }
        const next = LiveOutputService.live ? LiveOutputService.onAirSlide
                                            : LiveOutputService.stagedSlide
        // The pick's identity, the live/staged state and the design's background —
        // everything the blocks were copied from except WHICH PAGE is current.
        const key = (LiveOutputService.live ? "live|" : "staged|")
            + root.lineStepOwnerRef + "|"
            + (next && next.background !== undefined ? String(next.background) : "")
        if (key === root.liveFlowSlideKey && root.liveFlowSlide !== null)
            return              // the same pick and design: keep the blocks painted
        root.liveFlowSlideKey = key
        root.liveFlowSlide = next
    }
    onPreviewChanged: {
        if (root.previewSlideIndex >= root.previewSlides.length)
            root.previewSlideIndex = 0
    }
    // ONE ROW PER OUTPUT SLIDE. The rows come from THE CHAPTER ITSELF: the services cut
    // the chapter in the background before it is shown and write each divided item's own
    // rows onto it (SlideBuilder::attachSplit), so a long paragraph's continuations are
    // on screen the moment the chapter is — the operator can see where a passage divides
    // while presenting, without clicking a row to find out, and no click has to fill
    // anything on the interface thread.
    //
    // The cut arrives in TWO pieces, and this list is what that is for: the head (what one
    // screenful of it can show) with the open, so the list is divided the moment it
    // appears, and the rest of the passage a beat LATER, as a second rows delivery that
    // divides every row below the fold (see SlideBuilder's head/remainder block). A row's
    // rows therefore grow under it exactly once, right after a passage opens — which is
    // why nothing below re-picks or re-scrolls for a delivery that is not an open.
    //
    // A row with no `split` is an item the cut did not divide: it shows the chapter's own
    // stored text, as it always did (the number is the row's own label, not part of the
    // wording). A chapter that was never cut — nothing warmed yet, a library too large to
    // hold — carries no `split` anywhere and reads whole, which is exactly the behaviour
    // before this existed. Search results stay unchanged.
    readonly property var mainRows: {
        // The user's marks are read HERE, once, and carried BY THE ROW. They come
        // from the one batched chapter read (chapterMarks) and land on every row's
        // delegate as a value — so a row never calls markFor() to paint its number
        // gold, and the work is bounded by the chapter rather than by how many rows
        // happen to be on screen. (This list therefore re-builds when a mark lands,
        // which is a real model change and a rare one — a note or a highlight write
        // — not something a scroll or a repaint can cause.)
        const marks = root.hasUserData ? (root.chapterMarks || ({}).valueOf()) : ({}).valueOf()
        const rows = []
        for (const verse of root.chapterVerses) {
            const mark = marks[String(verse.number)]
            const marked = !!(mark && mark.highlight === true)
            const hasNote = !!((mark && mark.note) || "")
            const split = verse.split
            if (!split || split.length === 0) {
                rows.push({ number: verse.number, text: verse.text, continuation: false, isSplit: false,
                            partIndex: 0, marked: marked, hasNote: hasNote })
                continue
            }
            for (const piece of split) {
                // A continuation is never itself marked: the verse's number is worn
                // by the piece that HEADS it, exactly as the delegate's own rule
                // spelled it (`!continuation && …`).
                const cont = piece.continuation === true
                rows.push({ number: verse.number, text: piece.text,
                            partIndex: Number(piece.partIndex ?? 0),
                            continuation: cont, isSplit: true,
                            marked: cont ? false : marked, hasNote: cont ? false : hasNote })
            }
        }
        return rows
    }
    readonly property string templateId: { root.engineRevision; return root.adapter.templateId() }
    // The template's display name. Deliberately a property gated on
    // engineRevision rather than a bare call in the row's own binding: the
    // adapter's templateName() is a plain function, so a binding that calls it
    // reads no notifyable property and NEVER re-evaluates. The template
    // library itself loads asynchronously, after boot — and the default id is
    // the same string before and after that load, so templateId never changes
    // and the name computed in the pre-load pass stayed empty forever (the
    // blank name space next to "Template"). engineRevision bumps on every
    // adapter/service change, including the library finishing its load.
    readonly property string templateName: { root.engineRevision; return root.adapter.templateName(root.templateId) }
    readonly property bool usingDefaultTemplate: root.templateId === root.adapter.defaultTemplateId()

    // ---- which template the OUTPUT really renders --------------------------------------------
    // "Edit template" has to open the template the on-air picture is made of. When
    // the active output's STYLE names a template for this tab's content family
    // (Settings · Styles), SceneBuilder renders that family template INSTEAD of the
    // blocks these slides carry (style > slide) — so the style's pick is what the user
    // sees on the output and on go-live, and this tab's own pick only governs when the
    // style names none. styleRevision is the change signal: activeStyleTemplateId() is
    // a plain function, so a binding that calls it reads no notifyable property and
    // would never re-evaluate (the same trap `templateName` above documents).
    property int styleRevision: 0
    Connections {
        target: OutputListModel
        // Fires when the active output's style changes, when a style is saved, and
        // when a template the style wears is edited.
        function onActiveStyleChanged() { root.styleRevision++ }
    }
    readonly property string outputStyleTemplateId: {
        root.styleRevision
        return (root.adapter && root.adapter.contentType)
            ? OutputListModel.activeStyleTemplateId(root.adapter.contentType) : ""
    }
    readonly property bool templateFromOutputStyle: root.outputStyleTemplateId !== ""
    // The template the pencil edits and the row names: the style's when it names
    // one, else this tab's own pick (which is what reaches the output then).
    readonly property string effectiveTemplateId: root.templateFromOutputStyle ? root.outputStyleTemplateId : root.templateId
    readonly property string effectiveTemplateName: root.templateFromOutputStyle
        ? root.adapter.templateName(root.outputStyleTemplateId)
        : root.templateName
    readonly property string templateRowText: root.effectiveTemplateId === ""
        ? qsTr("Default — no template")
        : ((root.effectiveTemplateName !== "" ? root.effectiveTemplateName : qsTr("Missing template"))
           + (root.templateFromOutputStyle ? qsTr(" (from style)") : ""))
    readonly property var options: SettingsService.values

    // ---- WHERE THE READER WAS (user call: "make the app remember its state and
    // position ... if i go to another page and come back it will go number one and
    // i was in para 35") -------------------------------------------------------
    // The adapter names ONE registered settings key (readingKey, like
    // sessionSourceKey) and the pane keeps its place there as a JSON value: the open
    // source, book and chapter, the rows that were picked, and the row that was at
    // the top of the list. A pane that is built again — a restart, a reload, a tab
    // rebuilt by the engine — comes back to all of it instead of to the first
    // chapter and paragraph one.
    readonly property string readingKey: (root.adapter && root.adapter.readingKey)
        ? String(root.adapter.readingKey) : ""
    // The place read back from the store, held until the open it belongs to has
    // applied it (then it is cleared, so a later open never jumps backwards).
    property var readingRestore: null
    // THE PLACE AS LAST CAPTURED IN MEMORY (user call: "keep the panes' open passage
    // in memory as well as in settings, so an engine reload can restore instantly
    // without waiting on the settings store"). It is captured the moment the reader
    // moves — before the debounced write, and whether or not the store is up — so a
    // reload inside the same session restores from here: no settings load, no write
    // debounce, no dependency on the engine's store having come back yet. The store
    // stays the cross-restart copy (see saveReading).
    property var readingPlace: null
    property string readingStamp: ""
    // What the NEXT open puts back: the picked numbers, and the row that was on
    // screen (paragraph + piece — see topRowIdentity).
    property var restorePicked: []
    property var restoreRow: null
    // This tab's own split choice as it was when the pane was last on screen — put
    // back on activation if something wrote the key while the tab was away (see the
    // re-apply in onVisibleChanged). Per pane, so each tab remembers its own.
    property bool rememberedSplit: false
    property bool hasRememberedSplit: false
    // The fill options the rows ON SCREEN were cut with, captured when their chapter
    // was opened; and the one stale state a re-read was already tried for.
    property string openFillSignature: ""
    property string rowsFillSignature: ""
    property string refillTriedFor: ""
    // Everything the DIVISION of this tab's rows depends on (see SlideBuilder's
    // attachSplit): the template and this tab's own fill options. A change here
    // means the rows on screen were cut from something the tab no longer says.
    readonly property string fillSignature: {
        const p = root.adapter ? String(root.adapter.optionsPrefix || "") : ""
        const v = root.options || ({})
        return [root.templateId, p, v[p + ".verseNumbers"],
                v[p + ".splitLongVerses"], v[p + ".wordsPerSlide"]].join("|")
    }
    // Every move the reader makes is remembered on a short idle (see saveReading):
    // a scroll fires these hundreds of times a second, and the settings store is not
    // a place to write per frame.
    onSelectedChanged: root.touchReading()
    onChapterNumberChanged: root.touchReading()
    onBookIndexChanged: root.touchReading()
    Timer {
        id: saveReadingTimer
        interval: 350
        onTriggered: root.saveReading()
    }

    // ---- "RELOADING", NOT "EMPTY" (user call: give the library panes a quiet
    // reloading state, so a source that is absent for a moment reads as loading
    // rather than as blank columns) --------------------------------------------
    // `sources` reads empty in several honest situations: the engine has not booted
    // yet, a library is being (re)read, or a service publishes its corpus in a second
    // pass. None of them means "nothing is installed", but the empty wording said so
    // — and next to blank columns that reads as lost data. A list that has JUST gone
    // empty is therefore reported as reloading for a short grace window; one still
    // empty after it is genuinely empty. A pane holding an open passage is ALWAYS
    // reloading while its list is empty: it has a chapter on screen, so the library
    // exists and is coming back (see syncSources, which keeps that passage).
    property bool libraryReloading: false
    function noteSources() {
        if (root.sources.length > 0) {
            root.libraryReloading = false
            reloadGrace.stop()
            return
        }
        if (!root.libraryReloading) {
            root.libraryReloading = true
            reloadGrace.restart()
        }
    }
    readonly property bool showReloading: root.sources.length === 0
        && (root.libraryReloading || !EngineBridge.booted || root.book !== null
            || (root.adapter && root.adapter.loading && root.adapter.loading()))
    // What the empty columns say. The tab's own loading wording is the quiet one: the
    // empty wording ("No sermons yet — add one") is reserved for a library that is
    // really empty, because that is the one that is news.
    function libraryHint() {
        if (root.sources.length > 0)
            return ""
        if (root.showReloading) {
            // With a passage open this is a RELOAD — the library was read once and its
            // list is coming back — so it says so. Without one it is the tab's first
            // read, and the tab's own wording ("Opening the library…", "Reading the
            // Bibles…") is the right quiet line for it.
            if (root.book !== null)
                return qsTr("Reloading the library…")
            return (root.adapter && root.adapter.loadingText)
                ? String(root.adapter.loadingText) : qsTr("Reloading the library…")
        }
        return (root.adapter && root.adapter.emptyText)
            ? String(root.adapter.emptyText) : qsTr("Nothing here yet.")
    }
    onSourcesChanged: root.noteSources()
    Timer {
        id: reloadGrace
        interval: 4000
        // The grace is only for a list that was never seen non-empty; an open passage
        // keeps the loading wording for as long as the library takes.
        onTriggered: if (!root.book) root.libraryReloading = false
    }

    // The adapter object is a plain QtObject with `changed` emitted by its
    // owner (the wrapper binds it to its service's signal) — no direct
    // service reference anywhere in this file.
    Connections {
        target: root.adapter ? root.adapter : null
        function onChanged() {
            root.engineRevision++
            root.syncSources()
            // A template edit, a reload or an option written behind the pane leaves
            // the list holding a cut the tab no longer asks for (see refillIfStale).
            root.refillIfStale()
        }
        // A SYNC-fallback query's worker answer landed (a tab whose adapter has no
        // searchAsync). The published search answers [] for a query nobody has run
        // yet — "not searched yet" and "no rows" are different facts, the same split
        // peekShow's `pending` draws for a show nobody has read — so the fallback
        // re-asks here, by which time the answer is a cache read. Without this the
        // first empty answer was the last word: the search list stayed empty with
        // nothing on screen to say the query was still being run. Both tabs shipped
        // today have an async search, so this is the shape for one that has none.
        function onSearchCached(text) {
            if (root.searching && searchInput.text.trim() === text)
                root.runSearch(text)
        }
    }
    // The engine boots (and reads its settings store) AFTER this UI is up, so
    // at construction the remembered source key is not there yet — syncSources
    // would fall through to the FIRST source and, through openSource, write it
    // back as the remembered one (clobbering the user's choice). It runs once
    // now (a no-op until the store lands; syncSources keeps its ready guard for
    // the adapter's own pre-boot changed) and again via whenReady the moment the
    // store is open — the one idiom for a pre-boot read.
    Component.onCompleted: {
        root.noteSources()
        root.loadReading()
        root.syncSources()
        SettingsService.whenReady(function () {
            root.loadReading()
            root.syncSources()
        })
    }

    // ---- the reader's place (see readingKey) -------------------------------------

    // The place as one JSON value, or null while nothing is open.
    //
    // The ROW saved is the one to come back to, not necessarily the top one: the
    // picked paragraph when there is a pick (that is what the operator was reading),
    // else the row at the top of the list. Saving the top row unconditionally made the
    // place DRIFT — the restore centres what it saved, so the next save would record a
    // row half a screen higher, and every launch would walk up the chapter.
    function readingState() {
        if (!root.book || root.sourceId === "")
            return null
        const picked = root.selected.slice()
        const at = picked.length > 0
            ? { number: Number(picked[0]), partIndex: 0 }
            : root.topRowIdentity()
        return {
            source: String(root.sourceId),
            book: String(root.book.id),
            chapter: Number(root.chapterNumber),
            picked: picked,
            row: at
        }
    }
    // The session's own copy. Cheap enough to take on every move (a scroll's idle, a
    // pick, a chapter change) and independent of the engine, which is the point.
    function captureReading() {
        if (root.readingKey === "")
            return
        const state = root.readingState()
        if (!state)
            return
        const stamp = JSON.stringify(state)
        if (stamp === root.readingStamp)
            return
        root.readingStamp = stamp
        root.readingPlace = state
    }
    function saveReading() {
        root.captureReading()
        if (root.readingKey === "" || !SettingsService.ready || root.readingPlace === null)
            return
        const text = JSON.stringify(root.readingPlace)
        if (String(SettingsService.values[root.readingKey] ?? "") !== text)
            SettingsService.setValue(root.readingKey, text)
    }
    function touchReading() {
        if (root.readingKey === "")
            return
        root.captureReading()
        saveReadingTimer.restart()
    }
    // Read once per pane, and again when the engine's store lands: a pre-boot read
    // sees nothing, and acting on that is what sticks the wrong place.
    function loadReading() {
        if (root.readingKey === "" || root.readingRestore !== null)
            return
        // ONLY WHEN THERE IS NOTHING OPEN. A book on screen (or opening) is already
        // where the reader left it — and a read in flight carries its own place in
        // `restorePicked`/`restoreRow` — so a place loaded under one would just sit
        // armed for the next repair open, possibly older than the reader's last move.
        // The one case that needs it is a pane that has no book: a rebuild, or a source
        // whose outline is (re)arriving.
        if (root.book !== null)
            return
        // THE SESSION'S OWN PLACE FIRST: a reload inside this session restores from
        // memory at once, even while the engine's store is still coming up. ONE line
        // per restore, naming the book and chapter it came back to, so which copy was
        // used (memory or store) is visible in the log alongside the open trace.
        if (root.readingPlace !== null) {
            root.readingRestore = root.readingPlace
            EngineBridge.log("info", "OpenTrace", "reading place from MEMORY (this session): "
                             + root.readingPlace.book + " " + root.readingPlace.chapter)
            return
        }
        const raw = String(SettingsService.values[root.readingKey] ?? "")
        if (raw === "")
            return
        try {
            const state = JSON.parse(raw)
            if (state && typeof state === "object" && state.book !== undefined) {
                root.readingRestore = state
                EngineBridge.log("info", "OpenTrace", "reading place from SETTINGS: "
                                 + state.book + " " + state.chapter)
            }
        } catch (e) {
            // A value this build cannot read is simply no remembered place.
        }
    }

    // ---- opening things ----

    // Keeps a valid Bible open: the one used last, else the first installed.
    function syncSources() {
        // Wait for the engine's store: before it is up the remembered key reads
        // as merely absent, and acting on that would stick the wrong source (see
        // the SettingsService connection above, which calls back once it loads).
        //
        // EXCEPT WHEN THIS SESSION ALREADY KNOWS ITS PLACE: `readingPlace` is captured
        // from what is on screen, not from the store, so a pane that has been reading
        // can reopen that passage while the engine's store is still coming up — the
        // store is not consulted at all on that path (see loadReading). This is the
        // "restore instantly without waiting on the store" half.
        if (!SettingsService.ready && root.readingPlace === null)
            return
        // Every open this pane makes on its own (the first, the one after a corpus
        // reload, the one after a source list that momentarily read empty) asks the
        // store where the reader was first: without this the place is only restored
        // while `readingRestore` happens to be held, and a rebuild — the reported "it
        // goes back to number one and I was in para 35" — would open the first book
        // again. loadReading() is a no-op while a place is already waiting to apply.
        root.loadReading()
        const list = root.sources
        if (list.length === 0) {
            // A SOURCE LIST THAT READS EMPTY IS NOT AN EMPTY LIBRARY (user report: "if
            // i go to another page and come back it will go number one"). Both services
            // PUBLISH their corpus reads and answer `sources()` with [] while a reload,
            // an import, or a library rebuild is in flight — and this branch used to
            // clear the book, the chapter, the rows and the pick for that instant. When
            // the list came back the pane opened the FIRST chapter again, which is what
            // read as "it forgot where I was".
            //
            // The open passage is therefore KEPT: the rows stay on screen through the
            // gap (nothing blinks back to nothing), and when the list returns the
            // `currentSource` branch below finds the same book and does nothing at all —
            // no reopen, no reset. Only a pane with nothing open has nothing to keep, so
            // that is the one case that still clears.
            if (!root.book && root.chapterVerses.length === 0) {
                root.sourceId = ""
                root.books = []
            }
            return
        }
        if (root.currentSource) {
            // THE SOURCE IS OPEN — but its OUTLINE may not be in yet, and that
            // used to be the end of it. The service's corpus reads are PUBLISHED
            // (a miss queues the read on a worker and answers EMPTY, see
            // ScriptureService's header), so the FIRST openSource can land in that
            // gap: it set the source, took an empty outline, and left no book — and
            // every later notify then returned HERE, with the Bibles column listing
            // the Bible and the books / chapters / verses columns blank forever
            // (user report, with the tab's screenshot: "scripture is not showing").
            // An empty outline is therefore asked for again; openSource is the
            // whole repair (it reads once more and opens the first book — and with
            // it the chapter — when the rows finally arrive), and repeating it is
            // cheap: a read already in flight is not queued twice.
            if (root.books.length === 0)
                root.openSource(root.sourceId)
            return
        }
        // The adapter opts into "remember the last source" by naming a
        // REGISTERED settings key (Scripture: session.scriptureBible). No key
        // — single-source tabs like The Table — just opens the first.
        const lastKey = (root.adapter && root.adapter.sessionSourceKey) ? String(root.adapter.sessionSourceKey) : ""
        if (lastKey !== "") {
            const last = String(SettingsService.values[lastKey] ?? "")
            const known = list.find((b) => b.id === last)
            if (known) {
                root.openSource(known.id)
                return
            }
        }
        root.openSource(list[0].id)
    }

    function openSource(id) {
        if (id === root.sourceId && root.books.length > 0)
            return
        const previousBook = root.book ? root.book.id : ""
        root.sourceId = id
        root.books = root.adapter.books(id)
        // WHERE THE READER WAS: the remembered book and chapter of THIS source, not
        // the first book of it. Applied once — the state is taken as it is applied,
        // so every later open (a click, a jump) goes exactly where it was told.
        if (root.readingRestore !== null) {
            // THE OUTLINE IS NOT IN YET (a service's corpus read is PUBLISHED and answers
            // empty first — see this file's own header): the place is KEPT for the open
            // that can use it, because the outline's arrival re-enters openSource. It
            // used to be consumed against an empty book list, and the next open then fell
            // back to the first chapter — the restored place was silently dropped.
            if (root.books.length === 0)
                return
            const state = root.readingRestore
            const saved = String(state.source ?? "") === String(id)
                ? root.books.findIndex((b) => String(b.id) === String(state.book)) : -1
            root.readingRestore = null
            if (saved >= 0) {
                root.restorePicked = Array.isArray(state.picked) ? state.picked.slice() : []
                root.restoreRow = state.row || null
                root.openBook(saved, Number(state.chapter) || 0)
                return
            }
        }
        const same = root.books.findIndex((b) => b.id === previousBook)
        root.openBook(same >= 0 ? same : 0, same >= 0 ? root.chapterNumber : 0)
        const lastKey = (root.adapter && root.adapter.sessionSourceKey) ? String(root.adapter.sessionSourceKey) : ""
        // Only once the store is up: a write before it loads is refused, and the
        // remember-last-source value is not what an in-memory restore depends on.
        if (lastKey !== "" && SettingsService.ready
                && String(SettingsService.values[lastKey] ?? "") !== id)
            SettingsService.setValue(lastKey, id)
    }

    function openBook(index, chapter) {
        const __b0 = Date.now()
        root.bookIndex = Math.max(0, Math.min(index, root.books.length - 1))
        const b = root.book
        if (!b)
            return
        revealBook(root.bookIndex)
        const wanted = chapter > 0 && b.chapters.indexOf(chapter) >= 0 ? chapter : b.chapters[0]
        const __b1 = Date.now()
        root.openChapter(wanted, true)
        EngineBridge.log("info", "OpenTrace",
                         "openBook reveal=" + (__b1 - __b0) + "ms chapter=" + (Date.now() - __b1) + "ms")
    }

    // Scrolls the books / chapters columns so the active row is in view — a resolved
    // reference ("psalms 119") highlights a row that can sit far below the fold, and
    // a highlight you cannot see reads as "the chapters didn't move".
    function revealBook(index) {
        const y = 4 + index * 28
        if (y < booksFlick.contentY)
            booksFlick.contentY = Math.max(0, y - 4)
        else if (y + 28 > booksFlick.contentY + booksFlick.height)
            booksFlick.contentY = Math.max(0, Math.min(y + 28 - booksFlick.height + 4, booksFlick.contentHeight - booksFlick.height))
    }
    function revealChapter(chapter) {
        const idx = root.book ? root.book.chapters.indexOf(chapter) : -1
        if (idx < 0)
            return
        const y = 4 + idx * 28
        if (y < chaptersFlick.contentY)
            chaptersFlick.contentY = Math.max(0, y - 4)
        else if (y + 28 > chaptersFlick.contentY + chaptersFlick.height)
            chaptersFlick.contentY = Math.max(0, Math.min(y + 28 - chaptersFlick.height + 4, chaptersFlick.contentHeight - chaptersFlick.height))
    }

    function openChapter(number, selectFirst) {
        const __c0 = Date.now()
        root.peekIndex = -1                   // (a real open ends any hover peek)
        root.peekVerses = []
        root.previewSlideIndex = 0
        root.chapterNumber = number
        root.searching = false
        root.chapterSelectFirst = selectFirst
        root.chapterSelectLast = false
        // The options this read is cut with, held so the list can tell later whether
        // the rows it shows still match what the tab says (see refillIfStale).
        root.openFillSignature = root.fillSignature
        root.chapterVerses = []
        revealChapter(number)
        root.selected = []
        root.chapterToken++
        const token = root.chapterToken
        const bookId = root.book ? root.book.id : ""
        root.chapterStartedAt = Date.now()
        if (root.pendingGoTo && (root.pendingGoTo.bookId !== bookId || root.pendingGoTo.chapter !== number))
            root.pendingGoTo = null
        if (!bookId) {
            root.chapterVerses = []
            root.chapterLoading = false
            return
        }
        root.chapterLoading = true
        versesFlick.contentY = 0
        // A new chapter starts a new follow: the row the last one was tracking
        // belongs to the list that just left, and would otherwise read as
        // "off screen" and stand the follow down on its very first step.
        root.followedRowIndex = -1
        if (root.adapter && root.adapter.chapterAsync) {
            root.adapter.chapterAsync(root.sourceId, bookId, number, token)
        } else {
            root.applyChapter(token, root.sourceId, bookId, number,
                              root.adapter ? root.adapter.chapter(root.sourceId, bookId, number) : [])
        }
    }

    function applyChapter(token, sourceId, bookId, number, rows) {
        if (token !== root.chapterToken || sourceId !== root.sourceId
                || !root.book || bookId !== root.book.id || number !== root.chapterNumber)
            return
        const startedAt = root.chapterStartedAt
        // Was this a read the pane ASKED for? A chapter the SERVICE cuts again on its
        // own (an option moved under an open passage — see ScriptureService::changed)
        // arrives with chapterLoading already false, and it must not be logged as an
        // open: its timing would be measured from the original open, and the line would
        // claim the operator had navigated somewhere they had not.
        const opened = root.chapterLoading
        // A chapter that arrives while one is ALREADY on screen is the service cutting it
        // again — the rest of a long passage's cut landing after the head was shown, or an
        // option moving under an open passage (ScriptureService::changed). The list is the
        // operator's: it keeps reading the row it was reading (see topRowIdentity), and
        // nothing below re-picks for them.
        const anchor = opened ? null : root.topRowIdentity()
        root.chapterVerses = rows
        root.chapterLoading = false
        if (anchor)
            root.restoreRowAnchor(anchor)
        const pending = root.pendingGoTo
        if (opened && pending && pending.bookId === bookId && pending.chapter === number) {
            const end = pending.verseEnd > 0 ? pending.verseEnd : pending.verseStart
            const picked = rows.map((v) => v.number).filter((n) => n >= pending.verseStart && n <= end)
            root.pendingGoTo = null
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
                root.previewSlideIndex = 0
                root.revealVerse(picked[0])
                if (root.adapter && root.adapter.contentType === "table" && pending.matchText)
                    root.selectMatchingSubpart(picked[0], pending.matchText)
            }
        } else if (opened && (root.restorePicked.length > 0 || root.restoreRow !== null)) {
            // THE PLACE COMES BACK (see readingKey): the paragraphs that were picked in
            // this chapter, and the row that was under the eye. Only numbers this
            // chapter still has are taken — a replaced source is not the old place.
            const numbers = rows.map((v) => v.number)
            const picked = root.restorePicked.filter((n) => numbers.indexOf(Number(n)) >= 0)
            root.restorePicked = []
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
                root.previewSlideIndex = 0
            }
            const at = root.restoreRow
            root.restoreRow = null
            root.revealRemembered(at, picked)
        } else if (opened && root.chapterSelectLast && rows.length > 0) {
            const v = rows[rows.length - 1]
            root.selected = [v.number]
            root.anchorVerse = v.number
            root.centerRow(v.number, v.partIndex ?? 0, true)
        } else if (opened && root.chapterSelectFirst && rows.length > 0) {
            root.selected = [rows[0].number]
            root.anchorVerse = rows[0].number
            // A chapter stepped INTO puts its pick at the top of a list whose own
            // scroll was just reset — but the ‹ › may also have stepped INTO the
            // last row while the list was showing something else, so say it.
            root.centerRow(rows[0].number, 0, true)
        }
        if (opened) {
            root.rowsFillSignature = root.openFillSignature
            root.touchReading()
            EngineBridge.log("info", "OpenTrace", "openChapter n=" + number
                             + " async=" + (Date.now() - startedAt) + "ms rows=" + rows.length)
        }
    }

    Connections {
        target: TheTableService
        function onPreviewChapterReady(token, bookId, chapter, rows) {
            if (root.adapter && root.adapter.contentType === "table")
                root.applyPeekChapter(token, bookId, chapter, rows)
        }
        function onChapterReady(token, bookId, number, rows) {
            if (root.adapter && root.adapter.contentType === "table")
                root.applyChapter(token, root.sourceId, bookId, number, rows)
        }
    }
    Connections {
        target: ScriptureService
        function onChapterReady(token, bibleId, bookId, number, rows) {
            if (root.adapter && root.adapter.contentType !== "table")
                root.applyChapter(token, bibleId, bookId, number, rows)
        }
    }

    // A verse was clicked: alone, with Ctrl added or removed, with Shift as a range from the last one.
    function selectVerse(number, ctrl, shift) {
        root.previewSlideIndex = 0
        let next
        if (shift && root.anchorVerse > 0) {
            const lo = Math.min(root.anchorVerse, number), hi = Math.max(root.anchorVerse, number)
            next = root.chapterVerses.map((v) => v.number).filter((n) => n >= lo && n <= hi)
        } else if (ctrl) {
            next = root.selected.indexOf(number) >= 0 ? root.selected.filter((n) => n !== number) : root.selected.concat([number])
            root.anchorVerse = number
        } else {
            next = [number]
            root.anchorVerse = number
        }
        root.selected = next.sort((a, b) => a - b)
    }

    // Pick one PIECE of a divided paragraph (the row "4-2"). A CLICK only picks it —
    // the pane's preview walks to that piece and the MONITOR PREVIEW is left alone
    // (user call: "double click is what should push it to monitor preview, single
    // click is preview pane"). The staging that used to live here made every plain
    // click on a continuation row land on the tile, which is the only row shape
    // where a single click did what a double click is for; the double click now
    // stages it through stageSubpart below.
    function selectSubpart(number, partIndex) {
        root.selected = [number]
        root.anchorVerse = number
        root.previewSlideIndex = 0
        const slides = root.adapter.preview(root.sourceId, root.book.id, root.chapterNumber, [number]).slides || []
        for (let i = 0; i < slides.length; ++i) {
            const parts = slides[i].parts || []
            if (parts.some((part) => part.number === number && part.partIndex === partIndex)) {
                root.previewSlideIndex = i
                return
            }
        }
    }

    // The double click's half for a continuation row: the pick is already made (the
    // click ahead of the double click made it), so this stages exactly the piece the
    // pane's preview is showing — the same "pane preview and tile agree" selectSubpart
    // used to do on its own.
    function stageSubpart(number, partIndex) {
        root.selectSubpart(number, partIndex)
        root.stageOutputPart(number, partIndex)
    }

    // FOLLOW THE LINE STEPPER. Alt+Down/Alt+Up (LiveOutputService.lineStep)
    // walks the on-air/staged picture on its own, entirely inside that
    // service — nothing calls back into this pane as it crosses from one
    // paragraph's slide into the next one's, so the list kept showing
    // whatever was ORIGINALLY picked as "active" no matter how far the
    // reader had actually stepped (user report: "the scroll is working but
    // it's stuck at paragraph one" — the WORDS were moving; the list's own
    // highlight never was). Whenever the tile's content changes and it is
    // still THIS pick (stagedIsOurs / onAirTitle), re-point root.selected
    // and previewSlideIndex at whatever is actually showing now — the same
    // lookup selectSubpart already does for a direct click — so the
    // highlight and the auto-scroll (centerRow) follow the step.
    // TEMPORARY A/B SWITCH (user's own suggested test: "let's stop that
    // selector from moving, and let's see what happens"). false = the list's
    // own highlight/selection/scroll-into-view stay put while the content
    // keeps line-stepping normally — isolates whether MOVING THE SELECTOR
    // itself is what disturbs the monitor, separate from the content step
    // that triggers it. liveFlowSlide still refreshes either way, so the
    // preview box's own scroll is unaffected by this switch.
    // A/B TEST RESULT (user confirmed): disabling this did NOT stop the
    // monitor glitch — it's in the shared flow-drawing mechanism itself
    // (DesignPreview.qml / LiveOutputService), not in this pane's own
    // selector-tracking. Back on; see DrawsFlowChanged in DesignPreview.qml
    // for where the hunt continues.
    property bool followSelectorMoves: true
    // THE PIECE THE OPERATOR JUST PICKED (see followLiveStep): a click and the ‹ ›
    // set it before staging, so the follow can tell an operator's page from a step's.
    property var operatorPiece: null
    // ONE FOLLOW PER ADVANCE, NOT PER SIGNAL. An advance lands as several
    // notifications a few tens of ms apart (a staged commit, then the poll tick
    // reading the same state), and engine.log showed the whole body running
    // twice per press — two preview-index resolves and two scroll requests for
    // the SAME row. The signature (this pick's owner + live/staged + the
    // paragraph and piece actually showing) makes the repeat a no-op, while a
    // step that really moved still follows at once: nothing is debounced, so the
    // first notification of a fresh position always acts immediately.
    //
    // The owner is IN the signature on purpose — a new pick starts with the same
    // first paragraph and piece as the old one could end on, and that is a
    // follow that must happen (it re-points the list at a new passage).
    property string followedStepSignature: ""
    function followLiveStep() {
        if (!root.book)
            return
        root.refreshLiveFlowSlide()
        if (!root.followSelectorMoves)
            return
        const raw = LiveOutputService.live ? LiveOutputService.onAirSlideRaw : LiveOutputService.stagedSlideRaw
        const parts = (raw && raw.parts) || []
        if (parts.length === 0)
            return
        // THE OPERATOR'S OWN PICK IS ALREADY IN PLACE — DO NOT FOLLOW IT BACK.
        //
        // A click (and a ‹ › step) sets the selection, the preview index and the
        // list's scroll itself, then stages. This follow then re-derived the piece
        // from parts[0], which for a paragraph the fill split is the paragraph's
        // HEAD (1), not the continuation that was picked (1-7): the selection went
        // back to paragraph 1, the preview to its first slide, and centerRow put the
        // list at its first row — the reported "double click and the pane list goes
        // back to the top", with the outputs (correctly) still on the clicked piece.
        //
        // So when the slide that was just staged CONTAINS the piece the operator
        // picked, there is nothing to follow: the pane is already showing it. A
        // stepper's own stage carries no such piece and follows exactly as before.
        const op = root.operatorPiece
        const opMatches = (part) => op && Number(part.number) === Number(op.number)
                                         && Number(part.partIndex ?? 0) === Number(op.part)
        if (op && parts.some(opMatches)) {
            root.operatorPiece = null
            // LAND ON THE OPERATOR'S PIECE, DO NOT RE-DERIVE IT FROM parts[0].
            //
            // Returning early here was not enough: the preview index is whatever the
            // LAST pick set it to, so "click 1-6, then double-click 1" kept drawing
            // 1-5's page beside a list on row 1 (reported exactly that way). The
            // pick, the preview index and the row all follow the piece the operator
            // chose — and only when the staged slide does not carry it (a stepper's
            // own stage) does the old parts[0] follow run.
            root.selected = [Number(op.number)]
            root.anchorVerse = Number(op.number)
            const mine = root.previewSlides
            for (let i = 0; i < mine.length; ++i) {
                if ((mine[i].parts || []).some(opMatches)) {
                    root.previewSlideIndex = i
                    break
                }
            }
            root.followedStepSignature = root.lineStepOwnerRef
                + (LiveOutputService.live ? "|live|" : "|staged|")
                + Number(op.number) + "|" + Number(op.part)
            root.centerRow(Number(op.number), Number(op.part))
            EngineBridge.log("info", "FollowLiveStep",
                "keptOperatorPick number=" + Number(op.number)
                + " partIndex=" + Number(op.part)
                + " previewIndex=" + root.previewSlideIndex
                + " (the pane's own pick, not the paragraph's head)")
            return
        }
        const number = parts[0].number
        const partIndex = parts[0].partIndex ?? 0
        const signature = root.lineStepOwnerRef + (LiveOutputService.live ? "|live|" : "|staged|")
            + number + "|" + partIndex
        if (signature === root.followedStepSignature)
            return          // the same step, announced again: already followed
        root.followedStepSignature = signature
        const selectionChanging = root.selected.length !== 1 || root.selected[0] !== number
        // ONE-SHOT DIAGNOSTIC: everything that could matter about THIS exact
        // moment — is this a real paragraph change or a no-op re-announce,
        // what liveFlowSlide (what the preview box actually draws) is doing
        // right now, before this function touches root.selected at all. The
        // same function's own writes below (root.selected, previewSlideIndex)
        // are the ones that ripple into root.preview/currentPreviewSlide and
        // every binding that reads them (user call: "check and trace what
        // happens when the selector moves... track and see what could cause
        // that [glitch]").
        EngineBridge.log("info", "FollowLiveStep",
            "number=" + number + " partIndex=" + partIndex
            + " selectionChanging=" + selectionChanging
            + " prevSelected=" + JSON.stringify(root.selected)
            + " live=" + LiveOutputService.live
            + " liveFlowSlideNull=" + (root.liveFlowSlide === null)
            + " lineStepOwnerRef=" + root.lineStepOwnerRef
            + " referenceTextBefore=" + root.referenceText)
        if (selectionChanging) {
            root.selected = [number]
            root.anchorVerse = number
        }
        const slides = root.adapter.preview(root.sourceId, root.book.id, root.chapterNumber, [number]).slides || []
        for (let i = 0; i < slides.length; ++i) {
            const p = slides[i].parts || []
            if (p.some((part) => part.number === number && (part.partIndex ?? 0) === partIndex)) {
                root.previewSlideIndex = i
                root.centerRow(number, partIndex)
                EngineBridge.log("info", "FollowLiveStep",
                    "done previewSlideIndex=" + i
                    + " referenceTextAfter=" + root.referenceText
                    + " liveFlowSlideNullAfter=" + (root.liveFlowSlide === null))
                return
            }
        }
    }
    Connections {
        target: LiveOutputService
        function onStagedChanged() {
            if (root.lineStepOwnerRef !== "" && !LiveOutputService.live
                    && LiveOutputService.stagedIsOurs(root.lineStepOwnerRef))
                root.followLiveStep()
        }
        function onOnAirChanged() {
            if (root.lineStepOwnerRef !== "" && LiveOutputService.live
                    && LiveOutputService.onAirTitle === root.lineStepOwnerRef)
                root.followLiveStep()
        }
    }

    // Search hits identify the stored paragraph, while The Table can split that
    // paragraph into continuation rows (for example 150 and 150-3). Select the
    // continuation that actually contains the query so a hit opens at its text.
    function selectMatchingSubpart(number, query) {
        if (!root.book || !root.adapter || root.adapter.contentType !== "table")
            return
        const words = String(query || "").toLowerCase().match(/[a-z0-9]+/g) || []
        if (words.length === 0)
            return
        // `words` contains only alphanumerics, so it is safe to insert directly
        // into the regex. Reuse the pane's already-built preview slides.
        const ordered = new RegExp("\\b" + words.join("\\b\\W+(?:\\w+\\W+){0,2}") + "\\b", "i")
        const slides = root.previewSlides
        let best = null
        for (let slideIndex = 0; slideIndex < slides.length; ++slideIndex) {
            for (const part of slides[slideIndex].parts || []) {
                if (Number(part.number) !== number)
                    continue
                const text = String(part.text || "")
                const lower = text.toLowerCase()
                let count = 0
                for (const word of words)
                    if (new RegExp("\\b" + word + "\\b", "i").test(text))
                        ++count
                const phrase = ordered.test(lower)
                if (!best || (phrase && !best.phrase) || (phrase === best.phrase && count > best.count))
                    best = { slideIndex: slideIndex, partIndex: Number(part.partIndex), count: count, phrase: phrase }
            }
        }
        if (!best || best.count === 0)
            return
        root.selected = [number]
        root.anchorVerse = number
        root.previewSlideIndex = best.slideIndex
        // The row to reveal is found by the PIECE the hit sits on, not by a slide index:
        // the rows carry the chapter's cut and the pick its own, and the paragraph plus
        // its piece is the identity the two share (see mainRows).
        const rowIndex = root.mainRows.findIndex((row) => Number(row.number) === number
                                                     && Number(row.partIndex ?? 0) === best.partIndex)
        root.revealRow(rowIndex)
    }

    // The slide of the pick's preview that shows a paragraph's PIECE (the pair a
    // continuation row carries), or -1 when the pick has no such slide.
    function slideIndexForPart(number, partIndex) {
        const slides = root.previewSlides
        for (let i = 0; i < slides.length; ++i) {
            const parts = slides[i].parts || []
            if (parts.some((p) => Number(p.number) === Number(number)
                                   && Number(p.partIndex ?? 0) === Number(partIndex)))
                return i
        }
        return -1
    }

    function chapterOutputSlides() {
        const output = []
        for (const verse of root.chapterVerses) {
            const slides = root.adapter.slides(root.sourceId, root.book.id, root.chapterNumber, [verse.number])
            for (const slide of slides)
                output.push(slide)
        }
        return output
    }

    // Where the pick sits in the ROW list: the piece its preview shows (a divided
    // paragraph's continuation is a row of its own), else the head of the far end
    // of a range pick. -1 when nothing usable is picked.
    function currentRowIndex(direction) {
        const parts = root.previewSlideParts
        if (parts.length > 0) {
            const at = root.rowIndexForPart(parts[0].number, parts[0].partIndex ?? 0)
            if (at >= 0)
                return at
        }
        if (root.selected.length === 0)
            return -1
        return root.rowIndexFor(direction > 0 ? root.selected[root.selected.length - 1]
                                              : root.selected[0])
    }

    // Put the pick on one ROW, and the preview on that row's own piece.
    function focusRow(row) {
        root.selected = [row.number]
        root.anchorVerse = row.number
        root.previewSlideIndex = 0
        // An operator's own pick always moves the list (force) — the stand-down
        // rule exists for the automatic follow, not for a click.
        root.centerRow(row.number, row.partIndex ?? 0, true)
        // The pick's preview is rebuilt for the new selection, so the slide that
        // shows THIS piece is resolved after that — a continuation row (20-1) must
        // open on its own slide, not on the paragraph's first one.
        Qt.callLater(function () {
            const i = root.slideIndexForPart(row.number, row.partIndex ?? 0)
            if (i >= 0)
                root.previewSlideIndex = i
        })
    }

    // The previous / next ROW (crossing into the next chapter or book when it runs
    // out). The LIST is the operator's sequence, continuations included:
    // 20 -> 20-1 -> 21. Stepping by paragraph NUMBER skipped every continuation
    // row — 20-1 is a row of paragraph 20, not a number of its own (the reported
    // "the down jump 20-1 to 21"). Returns true when it moved within this chapter.
    function step(direction) {
        if (!root.book || root.mainRows.length === 0)
            return false
        const at = root.currentRowIndex(direction)
        const to = at + direction
        if (at >= 0 && to >= 0 && to < root.mainRows.length) {
            root.focusRow(root.mainRows[to])
            return true
        }
        if (root.chapterVerses.length > 0 && at < 0 && direction > 0) {
            // Nothing picked yet: forward starts at the top of the list.
            root.focusRow(root.mainRows[0])
            return true
        }
        root.stepChapter(direction)
        return false
    }

    // The neighbouring SERMON (a chapter of The Table's year book, a chapter of a
    // Bible): forward opens the next one on its first paragraph, back opens it on its
    // LAST one. Both select when the reader's rows arrive — never off `chapterVerses`
    // here, which is EMPTY the moment the chapter is asked for (that read was the
    // reported "ReferencePane.qml: Cannot read property 'number' of undefined",
    // thrown when a step crossed a chapter boundary with the async read still out).
    function stepChapter(direction) {
        if (!root.book)
            return false
        const chapters = root.book.chapters || []
        const ci = chapters.indexOf(root.chapterNumber) + direction
        if (ci >= 0 && ci < chapters.length) {
            root.openChapter(chapters[ci], direction > 0)
            root.chapterSelectLast = direction < 0
            return true
        }
        const bi = root.bookIndex + direction
        if (bi >= 0 && bi < root.books.length) {
            const b = root.books[bi]
            const chs = b && b.chapters ? b.chapters : []
            if (chs.length === 0) {
                root.openBook(bi, 0)
                return true
            }
            // openBook opens the chapter it is given (and announces "select first"),
            // so the step's own half is just which END of the new book to land on.
            root.openBook(bi, direction > 0 ? chs[0] : chs[chs.length - 1])
            root.chapterSelectFirst = direction > 0
            root.chapterSelectLast = direction < 0
            return true
        }
        return false
    }

    // Jumps to a passage the engine resolved from typed text: { bookId, chapter, verseStart, verseEnd }.
    function goTo(ref) {
        // Any jump ends a hover peek — including the pick-a-row kind. (A pick that
        // lands on the ALREADY-open chapter skips openBook/openChapter below, so
        // without this the peeked content would stick after the selection.)
        root.peekIndex = -1
        root.peekVerses = []
        const index = root.books.findIndex((b) => b.id === ref.bookId)
        if (index < 0)
            return
        // Re-opening the chapter resets the selection and scroll, and the search box fires a
        // resolve on EVERY keystroke — so only reload when the spot actually changed (typing
        // "genesis 1:1" then "genesis 1:16" must move the highlight, not blank the chapter).
        const targetBook = root.books[index]
        const chapters = targetBook ? targetBook.chapters : []
        const target = ref.chapter > 0 && chapters.indexOf(ref.chapter) >= 0
                     ? ref.chapter : (chapters.length > 0 ? chapters[0] : 0)
        const locationChanged = root.bookIndex !== index || root.chapterNumber !== target
        if (locationChanged || root.chapterLoading) {
            root.pendingGoTo = ref.verseStart > 0
                ? { bookId: ref.bookId, chapter: target, verseStart: ref.verseStart, verseEnd: ref.verseEnd,
                    matchText: ref.matchText || "" }
                : null
            if (locationChanged)
                root.openBook(index, target)
            return
        }
        if (ref.verseStart > 0) {
            const to = ref.verseEnd > 0 ? ref.verseEnd : ref.verseStart
            const picked = root.chapterVerses.map((v) => v.number).filter((n) => n >= ref.verseStart && n <= to)
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
                root.previewSlideIndex = 0
                // Bring the first picked verse into view — by ROW, not by the arithmetic
                // this used to do (a verse number minus one times 38): a divided chapter
                // makes those two different, and the row it names is the one the operator
                // needs to see (see revealVerse).
                root.revealVerse(picked[0])
                if (root.adapter && root.adapter.contentType === "table" && ref.matchText)
                    root.selectMatchingSubpart(picked[0], ref.matchText)
            }
        }
    }

    onFilterChanged: {
        if (root.filter.trim() === "")
            root.clearCitationMatches()   // (the box never calls the provider with "")
        const ref = root.adapter.resolve(root.filter, root.sourceId)
        if (ref.bookId !== undefined)
            root.goTo(ref)
    }

    // THE PUBLISHED CORPUS READ LANDED. The service reads its corpus off the
    // interface thread now (books, chapter, resolve), so the answer to a question
    // the operator has just asked arrives a beat later than the call that asked
    // it — `resolve` says "not a reference yet" while the read is in flight, and
    // the hover peek has no rows until the chapter is published. The adapter that
    // owns the service calls this when one lands; both re-asks below are then
    // pure cache reads, and `corpusRevision` is what re-evaluates visibleBooks
    // (whose answer to "is the box a reference" changed).
    // A tab whose adapter has no worker-backed reads simply never calls it.
    property int corpusRevision: 0
    function corpusCacheLanded() {
        root.corpusRevision++
        if (root.peekIndex >= 0)
            root.peekSuggestion(root.peekIndex)
        // The read the TYPED text was waiting on (see jumpToTyped): so a verse typed
        // into the seeded box moves the pane as soon as its reference resolves, not
        // only when the debounced filter happens to re-ask late enough.
        if (root.pendingJump !== "" && root.adapter) {
            const typed = root.pendingJump
            const jumpRef = root.adapter.resolve(typed, root.sourceId)
            if (jumpRef.bookId !== undefined) {
                root.pendingJump = ""
                root.goTo(jumpRef)
            }
        }
        if (root.filter.trim() !== "") {
            const ref = root.adapter ? root.adapter.resolve(root.filter, root.sourceId) : ({
                bookId: undefined
            })
            if (ref.bookId !== undefined)
                root.goTo(ref)
        }
    }

    readonly property var visibleBooks: {
        root.corpusRevision   // a reference resolution just landed: re-ask
        const needle = root.filter.trim().toLowerCase()
        if (needle === "")
            return root.books
        // A complete reference resolves — jump done, whole list stays (the popup does the moving).
        if (root.adapter.resolve(root.filter, root.sourceId).bookId !== undefined)
            return root.books
        const named = root.books.filter((b) => b.name.toLowerCase().indexOf(needle) >= 0)
        // NEVER empty while typing: a half-typed reference ("gene", "genesis 1:") matches no
        // book NAME, and blanking the books/chapters column mid-type looked like the pane lost
        // its data. Fall back to the full list — the suggestion popup handles the jumping.
        return named.length > 0 ? named : root.books
    }

    // Yellow highlight of the typed words in displayed text — the SAME idiom
    // Quick search uses (see QuickSearchDialog.highlight): escape first, then wrap
    // each query word (2+ chars) in a bright-yellow chip, rendered via StyledText.
    // The query to highlight with: the PILL's text while it is searching, else the
    // tab search box. (Keying only on root.filter left pill-searched text plain —
    // the "no highlight at all" case.)
    readonly property string activeQuery: root.searching ? searchInput.text : root.filter

    // The compiled highlight for the CURRENT query, built ONCE per query instead of
    // once per row per evaluation. The row delegate called highlight() for every
    // visible row and each call compiled its own RegExp out of the query words —
    // work that scaled with the row count on every repaint, which is part of what
    // made scrolling these passage lists stutter. A single `g` regex is safe to
    // share: String.replace always starts at 0 and leaves lastIndex at 0.
    readonly property var highlightRegex: {
        const words = root.highlightQuery.trim().toLowerCase().split(/\s+/).filter((w) => w.length >= 2)
        if (words.length === 0)
            return null   // nothing to highlight (an empty or one-letter query)
        const safeWords = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        const boundary = root.exact ? "\\b" : ""
        return new RegExp(boundary + "(" + safeWords.join("|") + ")" + boundary, "gi")
    }

    function highlight(text) {
        const source = String(text ?? "")
        if (source === "")
            return ""
        const re = root.highlightRegex
        if (!re)
            return source
        const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        // Qt StyledText ignores background-color on spans (bold survived, the yellow
        // didn't render — user screenshot). A colored <font> is honored.
        const gold = String(Theme.highlightInk)
        return esc(source).replace(re, '<b><font color="' + gold + '">$1</font></b>')
    }
    readonly property bool canHighlight: root.activeQuery.trim().length >= 2

    // One dropdown row for a sermon citation line ("47-0412 - Faith Is The
    // Substance"): the title as the label, the citation code ("47-0412") in the
    // right-hand trailing slot (user call — the two shapes they named, both
    // visible). The stored chapter title IS the citation line; split it. Letters
    // after the code ("53-0217A") ride the code, not the title.
    // Two stored shapes (spaces around dashes tolerated, code space-stripped):
    //   "47-0412 - Faith Is The Substance"  code - title (the separator dash)
    //   "47 - 1100X Fellowship"             code title  (no separator dash)
    // `paragraph` (optional, 1-based): a citation query that NAMES one paragraph
    // ("64-0617 54") lands its row on that paragraph — the ref carries the verse
    // so opening the row goes straight there, and the label says so ("· ¶54").
    function citeRow(book, index, cite, paragraph) {
        let code = "", title = cite
        let m = /^(\d{2}\s*-\s*\d{4}[A-Za-z]?)\s*-\s*(.+)$/.exec(cite)
        if (!m)
            m = /^(\d{2}\s*-\s*\d{4}[A-Za-z]?)\s+(\S.*)$/.exec(cite)
        if (m) {
            code = m[1].replace(/\s+/g, "")
            title = m[2].trim()
        }
        const para = Number(paragraph) > 0 ? Number(paragraph) : 0
        return { label: para > 0 ? title + " \u00b7 \u00b6" + para : title,
                 trailing: code,
                 cite: cite,
                 para: para,
                 ref: { bookId: book.id, chapter: book.chapters[index], verseStart: para, verseEnd: para } }
    }

    // The typed "<citation code> <paragraph>" tail ("64-0617 54", "47 - 0410 3"):
    // { code, para } when the text really is a citation code followed by a bare
    // paragraph number, else null. The code part must itself be code-shaped (a
    // dash or 3+ digits) so "genesis 1 12" (a Bible reference with a number
    // beside it) is never read as citation "genesis 1" + paragraph 12.
    function paragraphPick(text) {
        const needle = String(text == undefined ? "" : text).trim().toLowerCase()
        const toks = needle.split(/\s+/)
        if (toks.length < 2 || !/^\d{1,4}$/.test(toks[toks.length - 1]))
            return null
        const code = toks.slice(0, -1).join("").replace(/\s+/g, "")
        if (code === "" || !/^[0-9][0-9a-z\-]*$/.test(code))
            return null
        const digits = code.replace(/[^0-9]/g, "").length
        if (code.indexOf("-") < 0 && digits < 3)
            return null
        return { code: code, para: parseInt(toks[toks.length - 1], 10) }
    }

    // Live autocomplete for the tab search box: what the typed text still means, as plain data
    // What is left of a citation line after the handle the user typed, so the box can
    // complete "47-1020" -> " - The Angel Of God" — and "47 - 1020" -> the same,
    // because the handle is walked in the service's form (spaces beside a dash
    // skipped, and only those: a space anywhere else must match the cite literally).
    // "" means the handle does not address this citation's code (nothing to complete).
    function citationRest(text, cite) {
        const needle = TheTableService.citationNeedle(text)
        let i = 0
        let j = 0
        while (j < cite.length && i < needle.length) {
            const c = cite[j]
            // The run of spaces beside a dash is the part the two forms differ on.
            if (c === " " && (cite[j + 1] === "-" || (j > 0 && cite[j - 1] === "-"))) {
                ++j
                continue
            }
            if (c.toLowerCase() !== needle[i])
                return ""
            ++i
            ++j
        }
        return i === needle.length ? cite.slice(j) : ""
    }

    // rows for TabSearchBox's popup. Books whose name contains the text, then — when the text
    // IS a book ("genesis", "genesis 1") — that book's chapters with their sizes (verse counts
    // for a Bible, sermon titles for The Table). Each row carries a full `ref` so picking one
    // jumps straight there; clicking elsewhere in the box (or focusing it with text already
    // in) re-opens the list. Empty when there is nothing useful to offer.
    function suggest(text, sourceId) {
        // { rows: [popup rows], complete: string|null } — the inline completion rides
        // BESIDE the rows, never ON the array: the first version attached it as `out.complete`
        // and every return path went through out.slice(...), which returns a fresh plain array
        // and silently dropped the property (the autocomplete looked dead).
        const rows = []
        let completion = null
        if (!root.adapter || !text)
            return { rows: rows, complete: completion }
        const needle = String(text).trim().toLowerCase()
        if (needle === "")
            return { rows: rows, complete: completion }
        // THE TABLE (its books carry chapterTitles; Bibles never do). The citation
        // lines ("47-0412 - Faith Is The Substance") are the search targets, and
        // the user call sets a SPLIT threshold:
        //   - from TWO chars the in-pane list FILTERS live ("47-" lists the year's
        //     sermons, "fa" lists every title containing it) — the list is the
        //     filter, the user scrolls/picks; nothing is ever chosen for them;
        //   - the AUTO-FILL/JUMP stays strict: FIVE chars minimum (resolveCitation's
        //     threshold, TheTableService.cpp) AND exactly one match — short needles
        //     like "47-" match dozens of sermons and must never complete or jump.
        // So a unique match below 5 chars still shows as a one-row list to click;
        // from 5 chars up, uniqueness completes the rest of the line inline.
        if (root.books.length > 0 && root.books[0].chapterTitles !== undefined) {
            // A two-digit year is the Table's shorthand for a year prefix.
            // When that year exists, commit the citation separator so `61`
            // immediately becomes `61-` and the in-pane list is narrowed to
            // sermons from 1961 instead of matching those digits anywhere in
            // other years' titles.
            if (/^\d{2}$/.test(needle)) {
                const yearName = "19" + needle
                const yearBook = root.books.find((b) => String(b.name).toLowerCase() === yearName)
                if (yearBook) {
                    const yearMatches = []
                    const titles = yearBook.chapterTitles || []
                    for (let i = 0; i < yearBook.chapters.length; i++) {
                        const cite = String(titles[i] || "")
                        if (cite.toLowerCase().indexOf(needle + "-") === 0)
                            yearMatches.push(root.citeRow(yearBook, i, cite))
                    }
                    return { rows: yearMatches, complete: "-" }
                }
            }
            // A CITATION + PARAGRAPH query ("64-0617 54") names one sermon AND one
            // paragraph, so the paragraph gets its OWN row. The number is in no
            // citation line, so the plain indexOf match below could never see it
            // and the query listed nothing at all (user call: "let the quick
            // accept verse for the table"). The paragraph is dropped back to the
            // sermon-level row when the sermon does not have that paragraph.
            const paraPick = root.paragraphPick(needle)
            if (paraPick) {
                for (const b of root.books) {
                    const titles = b.chapterTitles || []
                    for (let i = 0; i < b.chapters.length; i++) {
                        const cite = String(titles[i] || "")
                        if (cite === "")
                            continue
                        const row = root.citeRow(b, i, cite)
                        if (row.trailing.toLowerCase() !== paraPick.code)
                            continue
                        const have = b.verseCounts && b.verseCounts.length === b.chapters.length
                                     ? Number(b.verseCounts[i]) : 0
                        const para = (have === 0 || paraPick.para <= have) ? paraPick.para : 0
                        return { rows: [root.citeRow(b, i, cite, para)], complete: "" }
                    }
                }
            }
            if (needle.length >= 2) {
                // The handle is compared in the service's OWN form (spaces beside
                // a dash dropped), so "47 - 1020" filters and completes exactly
                // like "47-1020" — the box and the engine must agree on what the
                // user typed, and TheTableService.citationNeedle is the one place
                // that decides.
                const citeNeedle = TheTableService.citationNeedle(text)
                const matches = []
                for (const b of root.books) {
                    const titles = b.chapterTitles || []
                    for (let i = 0; i < b.chapters.length; i++) {
                        const cite = String(titles[i] || "")
                        if (cite === "")
                            continue
                        if (cite.toLowerCase().indexOf(needle) >= 0
                                || TheTableService.citationNeedle(cite).indexOf(citeNeedle) >= 0)
                            matches.push({ book: b, index: i, cite: cite })
                    }
                }
                // Unique at 5+ chars: complete inline, no list (the resolve jumps).
                // Unique below 5: one-row list — click it, the fill must not fire.
                if (matches.length === 1 && needle.length >= 5) {
                    return { rows: [], complete: root.citationRest(text, matches[0].cite) }
                }
                if (matches.length > 0)
                    // EVERY match, uncapped: the pane's list is a ListView now, so
                    // the cost is the viewport's rows and not the result count
                    // ("th" matches 641 sermons of 1206 in a real library — the
                    // 150-row cap here existed only to bound that build, and it
                    // hid the rest of the matches from a list that scrolls).
                    return { rows: matches.map((m) => root.citeRow(m.book, m.index, m.cite)), complete: "" }
            }
            return { rows: [], complete: "" }
        }
        // Inline book-name completion ("gene" -> "sis "). Do not insert the
        // chapter/verse colon automatically: a valid one-digit chapter such as 1
        // may be the start of 10, 11, or 12. The operator types the full chapter,
        // then `:` and the verse; this keeps the separator easy to edit and avoids
        // turning the next chapter digit into a verse digit.
        const starts = root.books.filter((b) => b.name.toLowerCase().indexOf(needle) === 0 ||
                                                needle.indexOf(b.name.toLowerCase()) === 0)
        if (starts.length === 1) {
            const bk = starts[0], bkLower = bk.name.toLowerCase()
            if (bkLower.indexOf(needle) === 0)
                completion = bk.name.slice(needle.length) + " "
        }
        // The Table (a book WITH chapterTitles): once the year is typed in full, the rows
        // ARE that year's sermons — each row carries bookId + chapter so picking one jumps
        // straight into the sermon (user call: "use chapter not book"). Runs BEFORE the
        // resolved check (a bare year resolves, but the sermon list is exactly what should
        // show). Bibles (no chapterTitles) never enter this and keep the rows below.
        if (starts.length === 1 && starts[0].chapterTitles && starts[0].chapterTitles.length > 0 &&
                needle === starts[0].name.toLowerCase()) {
            const yr = starts[0]
            for (let i = 0; i < yr.chapters.length; i++) {
                const cite = String(yr.chapterTitles[i] || "")
                if (cite !== "")
                    rows.push(root.citeRow(yr, i, cite))
            }
            // No 8-row cap here (books keep theirs): a year holds a hundred+ sermons and
            // the popup list is a Flickable — it scrolls under its own maxHeight cap.
            return { rows: rows, complete: completion }
        }
        // RESOLVED -> NO dropdown (user call): the pane has already jumped and the
        // chapters column has moved, so a chapter list in a popup is redundant — the
        // the operator types the full chapter and then its verse separator.
        const resolved = root.adapter.resolve(String(text), sourceId)
        if (resolved.bookId !== undefined)
            return { rows: [], complete: completion }
        // Not resolved yet (an ambiguous prefix like "sa", or no book at all): the books
        // whose NAME contains the text, as rows to pick from.
        for (const b of root.books) {
            if (b.name.toLowerCase().indexOf(needle) >= 0) {
                const size = b.verseCounts && b.verseCounts.length === b.chapters.length
                             ? b.verseCounts.reduce((a, c) => a + c, 0) : b.chapters.length
                rows.push({ label: b.name, detail: b.verseCounts ? size + " verses" : size + " sermons",
                            ref: { bookId: b.id, chapter: 0, verseStart: 0, verseEnd: 0 } })
            }
        }
        return { rows: rows.slice(0, 8), complete: completion }
    }

    // The engine's resolved words for the LIVE query ("thn freind" -> "then
    // friend"): the pill's hint row AND the highlight both read this — the
    // highlight must wrap the CORRECTED words, or a typo query lights nothing
    // (the text says "friend", the raw query said "freind").
    property var resolvedWords: []
    // ASYNC pill search (the Quick-search pattern): the adapter's searchAsync
    // runs the query on a worker thread and answers on searchResultsReady; the
    // newest request's token wins, so a fast typist's older keystrokes answer
    // into the void and the GUI never blocks on a big query ("the" in the
    // sermons scanned every paragraph synchronously here — visible lag).
    // Adapters without searchAsync keep the synchronous call.
    // Word resolution is ALSO async now (wordsResolved): every misspelled
    // token scans the whole vocabulary, and a garbled multi-token query ran
    // that scan on the GUI thread per keystroke — the freeze behind typing
    // "danile 1; b4 icoud and 1".
    property int searchToken: 0
    property int resolveToken: 0
    // The keyboard/hover cursor into root.searchResults (full-text hits, NOT
    // citationMatches — see matchCursor/stepMatch for that list's own
    // version of this same pattern). -1 = nothing walked to or hovered yet.
    // Reset on every new query: a stale row position from the last search
    // must not survive onto a different result list.
    property int hitCursor: -1
    function stepHit(delta) {
        const rows = root.searchResults
        if (rows.length === 0)
            return false
        const next = root.hitCursor < 0
                   ? (delta < 0 ? rows.length - 1 : 0)
                   : Math.max(0, Math.min(rows.length - 1, root.hitCursor + delta))
        root.hitCursor = next
        if (hitsList && hitsList.visible)
            hitsList.positionViewAtIndex(next, ListView.Contain)
        return true
    }
    function runSearch(text) {
        root.hitCursor = -1
        const t = text.trim()
        if (t === "") {
            root.resolvedWords = []
            root.searchResults = []
            root.resolveToken++
            root.searchToken++
            return
        }
        const tableSearch = root.adapter && root.adapter.contentType === "table"
        if (tableSearch) {
            // (The Table's own search resolves the terms while it scans; the
            // spelling hint comes back only when it found nothing — see
            // applySearchResults, which runs it for a LOCKED empty answer too,
            // so the user can Tab the correction in and find the passage.)
            root.resolveToken++
            root.resolvedWords = []
        } else {
            root.resolveToken++
            SearchService.resolveWordsAsync(text, root.resolveToken)
        }
        // The chapter lock rides along as the scope (empty = the whole tab, as
        // it always was); adapters without the feature ignore the extra pair.
        if (root.adapter && root.adapter.searchAsync) {
            root.searchToken++
            root.adapter.searchAsync(text, root.sourceId, root.searchToken, root.exact,
                                     root.lockedBookId, root.lockedChapter)
        } else {
            root.searchResults = root.adapter.search(text, root.sourceId, root.exact,
                                                     root.lockedBookId, root.lockedChapter)
        }
    }
    // The chapter lock's click: flip it, and the SCOPE's own change re-runs what
    // is already typed (rescopeSearch below), so the list changes under the
    // user's eyes exactly like the padlock's does.
    function toggleScopeLock() {
        if (!root.book) return   // nothing open to lock to
        root.scopeLock = !root.scopeLock
    }
    // While the lock is on, the scope IS what is open: moving to another chapter
    // (or another book) re-runs the typed query against the passage now on
    // screen, so the rows always describe the page the user is looking at.
    function rescopeSearch() {
        if (!root.searching || searchInput.text.trim() === "")
            return
        root.runSearch(searchInput.text)
    }
    onLockedBookIdChanged: root.rescopeSearch()
    onLockedChapterChanged: root.rescopeSearch()
    // The padlock's click: flip the mode and re-run what is already typed, so the
    // list changes under the user's eyes instead of waiting for the next keystroke.
    function toggleExact() {
        root.exact = !root.exact
        if (root.searching && searchInput.text.trim() !== "")
            root.runSearch(searchInput.text)
    }
    // The worker's answer — stale tokens dropped (a fast typist's older
    // keystrokes resolve into the void, same as the search's).
    function applyResolvedWords(token, rows) {
        if (token !== root.resolveToken) return
        root.resolvedWords = rows
    }
    function applySearchResults(token, rows) {
        if (token !== root.searchToken) return   // a stale keystroke's answer
        root.searchResults = rows
        // The spelling hint and the half-typed word's candidate ladder show in
        // THIS pane too — the same words the app-wide dialog offers (user call:
        // "the typo fixer should be there" in this box as well). It used to run
        // only when the search found nothing, so with results on screen the
        // corrections and the readings were never shown. Resolved off the GUI
        // thread; the newest request's token wins.
        if (root.adapter && root.adapter.contentType === "table") {
            root.resolveToken++
            if (searchInput.text.trim() === "")
                root.resolvedWords = []
            else
                SearchService.resolveWordsAsync(searchInput.text, root.resolveToken)
        }
    }
    // Word-resolution worker answers (applyResolvedWords above).
    Connections {
        target: SearchService
        function onWordsResolved(token, rows) { root.applyResolvedWords(token, rows) }
    }

    // Words to highlight: EVERY word the user typed, plus whatever the engine
    // searched for instead of it (a corrected spelling). The typed words cannot be
    // missing from this list — the rows were matched on them, and a row showing
    // "compelling" plain beside a highlighted "that" reads as a broken highlighter
    // (user report: "the highlighter is not highlighting compelling").
    readonly property string highlightQuery: {
        const typed = root.activeQuery.trim().toLowerCase().match(/[a-z0-9]+/g) || []
        // Locked: the rows were matched on the TYPED words — highlight those, not
        // the engine's corrections (the correction is offered as the gold hint,
        // and taking it re-runs the locked search on the fixed word).
        if (root.exact)
            return typed.join(" ")
        // (No Array.flat in the QML JS engine — plain loops keep it portable.)
        for (const w of root.resolvedWords) {
            const parts = String(w.resolved).toLowerCase().match(/[a-z0-9]+/g) || []
            for (const p of parts)
                if (typed.indexOf(p) < 0) typed.push(p)
        }
        return typed.join(" ")
    }

    // The row's snippet, WINDOWED so the match reads in the middle of the row
    // rather than at its edge. The engine anchors its 220-character snippet near
    // the match, but a pane row shows ONE line (~90 characters), so a hit anywhere
    // past that was simply not on screen (user call: "i expect the highlighted text
    // to show well like in the centre not at the extreme so user can see the hits
    // well"). The width is measured with the row's own font, and the window opens
    // a third of the way before the hit CLUSTER.
    //
    // This one is measured IMPERATIVELY (no binding reads it), so it is safe to
    // assign here: the delegate passes its own bound measurement instead — see
    // the row's hitMetrics. Measuring inside the row's binding (writing a shared
    // TextMetrics that the same binding then read back) was a binding loop, and
    // Qt re-evaluated it for every row on every frame ("Binding loop detected
    // for property \"shown\"").
    TextMetrics {
        id: probeMetrics
        font.family: Theme.fontFamily
        font.pixelSize: 15
    }
    function collapseSnippet(text) {
        return String(text ?? "").replace(/\s*\n+\s*/g, " ").replace(/\s+/g, " ").trim()
    }
    // Keep The Table's result title to three words after its citation code so
    // the highlighted paragraph excerpt stays readable beside it.
    function compactTableResultTitle(row) {
        const citation = String((row && row.citation) || (row && row.reference) || "")
        const match = /^(\d{2}\s*-\s*\d{4}[A-Za-z]?)\s*-\s*(.*)$/.exec(citation)
        if (!match)
            return citation
        const words = match[2].trim().split(/\s+/).filter((word) => word !== "")
        const title = words.length > 3 ? words.slice(0, 3).join(" ") + "…" : words.join(" ")
        return match[1].replace(/\s+/g, "") + " - " + title
    }
    // `measuredAdvance` (optional) = the advance width the CALLER measured for
    // `snippet` with the row's own font. The delegate always passes its own (a
    // bound TextMetrics, so this stays a pure function of its arguments); a
    // caller that does not — the pane-highlight scenario — makes this measure
    // with probeMetrics above, which is only ever written imperatively.
    function centeredSnippet(snippet, availableWidth, measuredAdvance) {
        const text = root.collapseSnippet(snippet)
        if (text === "" || availableWidth <= 0)
            return text
        // How much text fits the row: measure the whole snippet once and scale.
        let advance = Number(measuredAdvance)
        if (!(advance > 0)) {
            probeMetrics.text = text
            advance = probeMetrics.advanceWidth
        }
        const perChar = advance / Math.max(1, text.length)
        const fit = perChar > 0 ? Math.max(24, Math.floor(availableWidth / perChar)) : 90
        const span = Math.ceil(fit * 1.25)
        const words = root.highlightQuery.split(/\s+/).filter((w) => w.length >= 2)
        // WHERE to open the window: the opening that carries the MOST of the
        // typed words, not simply the first one of them. Anchoring on the first
        // occurrence let an incidental early "that" hold the window at the
        // snippet's start while the words the user actually typed sat past the
        // visible edge — the row showed "compelling, that" with compelling half
        // off the end (user screenshot). The cluster goes in the middle instead.
        const low = text.toLowerCase()
        let hit = -1
        let bestCovered = 0
        for (const w of words) {
            let at = low.indexOf(w)
            let tried = 0
            while (at >= 0 && tried < 24) {   // a 220-char snippet cannot hold many
                let covered = 0
                const until = at + span
                for (const other of words) {
                    const o = low.indexOf(other, at)
                    if (o >= 0 && o < until) ++covered
                }
                if (covered > bestCovered || (covered === bestCovered && hit < 0)) {
                    bestCovered = covered
                    hit = at
                }
                at = low.indexOf(w, at + 1)
                ++tried
            }
        }
        if (hit < 0)
            return text
        let start = Math.max(0, Math.round(hit - fit * 0.35))
        // Never begin mid-word: step back to the space before the window.
        if (start > 0) {
            const before = text.lastIndexOf(" ", start)
            start = before > 0 ? before + 1 : 0
        }
        const window = text.substring(start, start + span)
        return start > 0 ? "… " + window : window
    }

    // ---- tab-search autocomplete ------------------------------------------------
    // Called per keystroke by the tab bar (which registered this pane as its tab's
    // provider): rows for the popup (label + right-hand detail + payload.ref), straight
    // from suggest() above. (The tab bar looks this method up by name — paneSuggestions
    // — so the name is the contract; a rename here silently kills the popup AND the
    // inline autofill while the resolve-jump keeps working, which is exactly the
    // "jumped but nothing completed" bug this name once caused.)
    function paneSuggestions(text) {
        const res = root.suggest(text, root.sourceId)
        // THE TABLE: the matches live IN THE PANE (FreeShow's scripture search —
        // results render in the drawer, nothing floats over the content; the old
        // floating popup covered the search box AND the hover preview). Stored for
        // the in-pane matches list (citationMatches); NO rows returned, so the
        // popup never opens here. Scripture keeps the popup path below.
        if (root.books.length > 0 && root.books[0].chapterTitles !== undefined) {
            root.allMatches = res.rows || []
            root.citationMatches = root.allMatches
            root.lastSuggestRows = root.allMatches
            // A NEW LIST STARTS ITS OWN WALK: the old cursor indexes rows that are
            // gone, and the SEARCH owns the selector now (the arrows are the search's
            // while results are up — see TabSearchBox.listMode).
            root.matchCursor = -1
            root.bookCursor = -1
            root.chapterCursor = -1
            if (root.citationMatches.length > 0)
                root.column = 2
            return { rows: [], complete: res.complete || "" }
        }
        root.lastSuggestRows = res.rows || []
        // `trailing` is what DropdownPanel actually draws on the right (its rows read
        // label + trailing; `detail` was silently ignored). Sermon rows carry their own
        // trailing (the citation code); book rows fall back to detail ("1189 verses"),
        // which never rendered before either.
        // `cite` rides the payload too (sermon rows): on a pick the search box replaces
        // the typed needle with the FULL citation line, so the box ends up naming the
        // sermon the user actually chose (not the one the fill guessed).
        return { rows: (res.rows || []).map((row) => ({ label: row.label, trailing: row.trailing || row.detail || "",
                                                        payload: row.cite !== undefined ? { ref: row.ref, cite: row.cite } : { ref: row.ref } })),
                 complete: res.complete || "" }
    }
    // WHAT THE SEARCH BOX STARTS WITH (paneSearchSeed, below): the passage the pane is
    // ON, written the way this tab's own resolve reads it, with the part still to name
    // left open — so the operator types the paragraph/verse instead of the whole line.
    //
    //   The Table: "55-0117 - How The Angel Came To Me And His Commission " — the
    //   citation line plus a trailing space, so digits typed next become the PARAGRAPH
    //   ("…Commission 5" is ¶5; resolveCitation's trailing-number rule reads it).
    //   Seeding it WITH a paragraph would make the typed digits append to the old one
    //   ("… 2" + "3" = ¶23), which is why the number is deliberately not in the seed.
    //
    //   Scripture: "Genesis 1: " — the open book and chapter with the VERSE slot left
    //   open, so the next digits are the verse ("Genesis 1:" + "16" = "Genesis 1: 16",
    //   which the reference resolver reads as verse 16 of that chapter; its ParseBody
    //   explicitly allows the space after the colon). The book name is the loaded
    //   Bible's own ("1 Samuel", not an abbreviation), because that is what the
    //   resolver and the suggestion rows speak.
    //
    // THE TWO SEARCHABLE REFERENCE TABS (user calls: "it is for only the table not
    // all the tabs", then "scripture too needs that feature"). Shows / Media /
    // Templates / Overlays keep their plain "Search <tab>" hint and an empty box — a
    // filter over cards has no passage to name.
    //
    // "" when the pane has nothing open yet (or the adapter cannot build one): the
    // box then shows its plain hint instead.
    readonly property bool isTablePane: root.books.length > 0 && root.books[0].chapterTitles !== undefined
    function paneSearchSeed() {
        if (!root.book || !root.adapter)
            return ""
        const chapter = root.chapterNumber
        if (!(chapter > 0))
            return ""
        if (!root.isTablePane) {
            // Scripture (the only other tab that registers a suggestion provider and
            // opens one passage at a time): book + chapter, verse still to be typed.
            const name = String(root.book.name || "")
            return name === "" ? "" : name + " " + chapter + ": "
        }
        const text = String(root.adapter.reference(root.book.name, chapter, []) || "")
        if (text === "")
            return ""
        // The citation line is the sermon's whole name; the trailing space opens the
        // paragraph slot, so digits typed next name a paragraph ("…Commission 56").
        return text + " "
    }

    // The typed text, resolved and jumped — the SAME two calls the pane's own
    // onFilterChanged makes (resolve, then goTo). The tab bar calls this per keystroke
    // for The Table, so a seeded citation line plus a paragraph number
    // ("…Commission 56") moves the pane to ¶56 as it is typed, instead of waiting on
    // the filter's reading beat. Anything that does not resolve is simply ignored —
    // a half-typed citation changes nothing.
    function jumpToTyped(text) {
        if (!root.adapter)
            return
        const typed = String(text)
        if (typed.trim() === "") {
            root.pendingJump = ""
            return
        }
        const ref = root.adapter.resolve(typed, root.sourceId)
        if (ref && ref.bookId !== undefined) {
            root.pendingJump = ""
            root.goTo(ref)
            return
        }
        // NOT RESOLVED *YET*: Scripture answers a reference from its published cache
        // and starts the read on a miss, so the first time a verse is typed the
        // answer is "nothing" and arrives a beat later. Remembering the text here is
        // what makes that landing land: corpusCacheLanded re-asks THIS text once the
        // read is in (the pane's own filter-based re-ask is the debounced one, and it
        // can fire before the read is back — the reported "scripture is not moving to
        // verse 8": the line was in the box, the pane stayed on verse 1).
        root.pendingJump = typed
    }
    // The typed text still waiting on a reference read ("" = nothing pending).
    property string pendingJump: ""
    // A walked result was OPENED (Enter, or a double-click on its row): the search is
    // finished, and the host clears its query so the tab is back to plain browsing.
    signal matchTaken()

    // A picked row jumps to its passage: book only = open the book; chapter = open that
    // chapter (vessels through goTo, so selection/scroll behave exactly like a resolve).
    function applySuggestion(ref) {
        if (ref && ref.bookId !== undefined)
            root.goTo(ref)
    }

    // ---- search matches (The Table, IN THE PANE) + hover peek --------------------
    // The tab search's citation matches render as a list INSIDE the verses column
    // (matchesRow below) — FreeShow's scripture-search model: the results are pane
    // content, so nothing floats over the search box or the text. Hovering a match
    // shows that sermon's FULL paragraphs in the right half (the peek) — no visibility
    // swaps, so moving between rows can't oscillate. Cleared when the text empties
    // (onFilterChanged) or a match is clicked (the sermon opens for real).
    property var citationMatches: []    // [{ label, trailing, cite, ref }] (citeRow shape)
    property var lastSuggestRows: []    // (the popup's rows — Scripture only now)
    property int peekIndex: -1
    property int peekRequestToken: 0
    // THE KEYBOARD'S PLACE IN THE RESULT LIST (user call: "if press the down or up
    // button it should search result list and show their paragraph preview"). The
    // tab search box's Up/Down walk this: each row highlights exactly like a hover
    // and the SAME peek panel shows that sermon's paragraphs, so the list is
    // navigable without the pointer. -1 = nothing walked to yet.
    property int matchCursor: -1
    function stepMatch(delta) {
        const rows = root.citationMatches
        if (rows.length === 0)
            return false
        const next = root.matchCursor < 0
                   ? (delta < 0 ? rows.length - 1 : 0)
                   : Math.max(0, Math.min(rows.length - 1, root.matchCursor + delta))
        if (next === root.matchCursor)
            return true            // at an end: the key is still ours, the caret stays put
        root.matchCursor = next
        root.peekRow = 0           // a new result opens on its first paragraph
        root.hoveredMatch = -2     // the arrows own the preview (a stray hover-exit must not clear it)
        peekClear.stop()
        root.peekSuggestion(next)
        if (matchesList && matchesList.visible)
            matchesList.positionViewAtIndex(next, ListView.Contain)
        return true
    }
    // ENTER on a walked-to row: that row IS the answer. Returns the row's citation
    // line ("56-0101 - Why Are People So Tossed About") so the host can leave it in
    // the box — the same full line a picked suggestion leaves, which is what the
    // user asked to be consistent with ("the return search should be 56-0101 - Why
    // are people ... so it consistent with others").
    // ---- ESC: the search is abandoned, not just emptied --------------------------
    // The first keystroke of a search jots down the place the reader was in (the
    // passage, the paragraphs picked, the preview slide and the row under the eye), and
    // Esc puts all of it back — user call: "when i press esc i expect to take back where
    // i was before entering search mode". Only the FIRST keystroke captures: the jump
    // the typing itself causes must not become the place to return to.
    property var searchReturn: null
    function beginSearch() {
        if (root.searchReturn !== null || !root.book)
            return
        root.bookCursor = -1
        root.chapterCursor = -1
        root.searchReturn = { bookId: root.book.id, chapter: root.chapterNumber,
                              picked: root.selected.slice(),
                              slide: root.previewSlideIndex,
                              row: root.topRowIdentity() }
    }
    function endSearch() {
        const back = root.searchReturn
        root.searchReturn = null
        root.allMatches = []
        root.clearCitationMatches()
        root.matchCursor = -1
        root.bookCursor = -1
        root.chapterCursor = -1
        root.pendingJump = ""
        root.column = 2
        if (!back || !root.book || root.books.length === 0)
            return false
        if (root.book.id === back.bookId && root.chapterNumber === back.chapter) {
            root.selected = back.picked.slice()
            root.previewSlideIndex = back.slide
            if (back.row)
                root.revealRemembered(back.row, back.picked)
            return true
        }
        const index = root.books.findIndex((b) => b.id === back.bookId)
        if (index < 0)
            return false
        root.restorePicked = back.picked.slice()
        root.restoreRow = back.row
        root.openBook(index, back.chapter)
        return true
    }

    function takeMatch() {
        const row = root.matchCursor >= 0 ? root.citationMatches[root.matchCursor] : null
        if (!row || !row.ref)
            return ""
        const cite = String(row.cite !== undefined ? row.cite : row.label)
        // ENTER on the preview's paragraph column must open the paragraph the
        // selector is sitting on, rather than only opening the sermon at its
        // default first paragraph.
        let target = row.ref
        if (root.column === 3 && root.peekRow >= 0 && root.peekRow < root.peekVerses.length) {
            const paragraph = Number(root.peekVerses[root.peekRow].number)
            if (paragraph > 0)
                target = Object.assign({}, row.ref, {
                    verseStart: paragraph,
                    verseEnd: paragraph,
                    matchText: searchInput.text
                })
        }
        root.applySuggestion(target)
        root.allMatches = []
        root.clearCitationMatches()
        root.bookCursor = -1
        root.chapterCursor = -1
        root.column = 2
        // Taking a result ends both the tab's query (matchTaken below) and
        // this pane's local search view. Otherwise the picked paragraph opens
        // underneath the results and the user has to dismiss the search by hand.
        root.searching = false
        searchInput.text = ""
        // TAKEN IS TAKEN: the search is committed, so there is nothing left for Esc to
        // take the reader back from.
        root.searchReturn = null
        // THE SEARCH IS OVER: the host empties its box back to the opened sermon's own
        // seed (user call: "when press enter or double click it should open it up and
        // leave the search mode").
        root.matchTaken()
        return cite
    }
    property var peekVerses: []          // [{ number, text }] while hovering a match
    // THE KEYBOARD'S PLACE IN THOSE VERSES: which paragraph the selector is reading
    // while it is on the preview pane (search mode, column 3 — see stepPeek). Reset
    // whenever the highlighted result changes.
    property int peekRow: 0
    function peekSuggestion(index) {
        if (index < 0 || !root.adapter) {
            root.peekRequestToken++
            root.peekIndex = -1
            root.matchCursor = -1
            root.peekVerses = []
            root.peekRow = 0
            return
        }
        const row = root.citationMatches[index]
        const ref = row ? row.ref : null
        const ch = ref ? ref.chapter : 0
        if (!(ch > 0)) {
            root.peekRequestToken++
            root.peekIndex = -1
            root.peekVerses = []
            return
        }
        root.peekRequestToken++
        const token = root.peekRequestToken
        root.peekIndex = index
        root.peekRow = 0
        if (root.adapter.chapterPeekAsync) {
            root.peekVerses = []
            root.adapter.chapterPeekAsync(root.sourceId, ref.bookId, ch, token)
        } else {
            root.peekVerses = root.adapter.chapter(root.sourceId, ref.bookId, ch)
        }
    }
    function applyPeekChapter(token, bookId, chapter, rows) {
        if (token !== root.peekRequestToken || root.peekIndex < 0)
            return
        const current = root.citationMatches[root.peekIndex]
        if (!current || !current.ref || current.ref.bookId !== bookId || current.ref.chapter !== chapter)
            return
        root.peekVerses = rows
    }
    function clearCitationMatches() {
        root.peekRequestToken++
        root.citationMatches = []
        root.lastSuggestRows = []
        root.peekIndex = -1
        root.matchCursor = -1
        root.peekVerses = []
        root.peekRow = 0
        root.hoveredMatch = -1
    }
    // WHICH ROW THE POINTER IS ON, as ONE value on the pane — the hand-off between
    // rows could not be read from a per-row hover flag: moving down the list delivers
    // enter(B) and exit(A) in either order, and when the exit arrived last its clear
    // wiped the preview that the same move had just opened (the reported "I hover down
    // and the results stop showing their verses").
    //
    //   >= 0  the pointer is on that row
    //     -1  nobody owns the preview; a deferred clear may close it
    //     -2  the CLICK or the keyboard owns it (a pointer leaving a row must not
    //         throw away what the operator just chose)
    property int hoveredMatch: -1
    // The deferred clear behind the row-to-row hover hand-off (see matchHover).
    // Non-zero so it runs AFTER both halves of a move have been delivered.
    Timer {
        id: peekClear
        interval: 16
        onTriggered: if (root.hoveredMatch === -1) root.peekSuggestion(-1)
    }

    // What a dragged verse carries into a project: the passage (the selected verses when this one is among them, else just this one).
    function dragPayload(numbers) {
        if (!root.book)
            return null
        const nums = numbers.slice().sort((a, b) => a - b)
        const ref = root.adapter.reference(root.book.name, root.chapterNumber, nums)
        return { kind: "reference", items: [{ ref: ref, name: ref, meta: { source: root.sourceId, book: root.book.id, chapter: root.chapterNumber, verses: nums } }] }
    }

    // "Convert to show": the whole passage becomes a show, one slide per screenful of verses.
    // WHAT A FULL SCREENFUL IS FOR THIS TAB: the pane's own "Max words per slide"
    // setting, handed to the service so the reading column can be fitted against the
    // BUDGET instead of against whatever words a page carries (see
    // LiveOutputService.readingWordBudget — one size per template box, nothing able
    // to overflow it). Read at each stage because the setting is per tab and the
    // operator can change it between picks; slides themselves are not touched.
    // ONLY THE SCROLL ASKS FOR THIS. A double-click and the ‹ › stage a SLIDE —
    // its own template box, its own split, nothing of the reading walk — so they
    // must not push a budget into the service and let it reach the fit: these two
    // features are separate, and mixing them is what made a slide's size depend on
    // a scroll setting. Called from ensureScrollSource, the one place the walk
    // starts.
    function syncReadingWordBudget() {
        if (!root.adapter || !root.adapter.optionsPrefix)
            return
        const v = Number(SettingsService.value(root.adapter.optionsPrefix + ".wordsPerSlide"))
        // A WRITE accessor is a property assignment in QML — calling it threw
        // "Property 'setReadingWordBudget' … is not a function" on every stage.
        LiveOutputService.readingWordBudget = (v > 0 ? v : 40)
    }

    function buildSlides() {
        if (root.selected.length === 0 || !root.book)
            return []
        return root.adapter.slides(root.sourceId, root.book.id, root.chapterNumber, root.selected)
    }

    function convertToShow() {
        const slides = root.buildSlides()
        if (slides.length > 0)
            root.convertToShowRequested(root.referenceText, slides)
    }

    // Build the SCROLL stream from the chapter JSON, independently of the visual
    // slides and their template splitting. Works for any reference tab (The Table's
    // paragraphs, Scripture's verses) because it reads the pane's own
    // chapterVerses/mainRows, which both adapters fill — the walk is a reading of
    // the passage, not a re-cut of the slides.
    function readingPagesFromChapter(number, partIndex) {
        const verseIndex = root.chapterVerses.findIndex((v) => Number(v.number) === Number(number))
        if (verseIndex < 0)
            return { pages: [], index: 0 }
        const row = root.mainRows.find((r) => Number(r.number) === Number(number)
                                              && Number(r.partIndex ?? 0) === Number(partIndex ?? 0))
        // ONE CONTINUOUS PASSAGE, the way a book prints it: the items run on into
        // each other with a space, NOT a newline. A newline here is a hard line
        // break inside the page, so a page that crosses a paragraph boundary
        // started the next paragraph on its own line and left the rest of the box
        // empty — the reported "I pick paragraph one and it is not like a book".
        // The page break (the word budget below) is the only division left, and a
        // page fills from wherever it starts with as many words as the budget
        // allows, crossing into the next paragraph and the one after it.
        let text = ""
        let selectedOffset = 0
        for (let i = 0; i < root.chapterVerses.length; ++i) {
            const verse = root.chapterVerses[i]
            if (i > 0)
                text += " "
            const verseText = String(verse.text ?? "")
            if (i === verseIndex) {
                let partOffset = 0
                if (row) {
                    const found = verseText.indexOf(String(row.text ?? ""))
                    if (found >= 0)
                        partOffset = found
                }
                selectedOffset = text.length + partOffset
            }
            text += verseText
        }

        const setting = Number(root.options[root.adapter.optionsPrefix + ".wordsPerSlide"] ?? 40)
        const wordsPerScreen = Math.max(3, Math.floor(setting))
        const word = /\S+/g
        const matches = []
        let match
        while ((match = word.exec(text)) !== null)
            matches.push({ start: match.index, end: word.lastIndex })
        const pages = []
        const selectedWord = matches.findIndex((entry) => entry.start >= selectedOffset)
        const pivot = selectedWord < 0 ? matches.length : selectedWord
        const appendRange = (from, to) => {
            if (from >= to)
                return
            const start = from === 0 ? 0 : matches[from].start
            const end = to < matches.length ? matches[to].start : text.length
            pages.push({ body: text.slice(start, end), start: start })
        }
        let cursor = 0
        while (cursor < pivot) {
            const next = Math.min(cursor + wordsPerScreen, pivot)
            appendRange(cursor, next)
            cursor = next
        }
        let selectedPage = pages.length
        while (cursor < matches.length) {
            const next = Math.min(cursor + wordsPerScreen, matches.length)
            appendRange(cursor, next)
            cursor = next
        }
        if (pages.length > 0)
            selectedPage = Math.min(selectedPage, pages.length - 1)
        return { pages: pages, index: Math.max(0, selectedPage) }
    }

    // THE ON-AIR PICK (FreeShow's verse dblclick -> playScripture): the picked
    // verses' slides go straight to the live output. The adapter (or the host)
    // decides how — ScripturePane/TheTablePane hand it to LiveOutputService,
    // which checks the output style's content pills first.
    function playPicked() {
        if (!root.book || root.selected.length === 0)
            return
        // THE OPERATOR JUMPED (a double-click's body, the play button): the outputs'
        // reading column may solve its own size for the page this stages. See
        // LiveOutputService.readingJumpSerial — a page turn inside a walk must not.
        LiveOutputService.noteOperatorJump()
        // The style gate: the active output's style must admit this tab's
        // content type (the Edit dialog's pills). Greyed pill = not meant for
        // that output — say so instead of silently showing nothing.
        const kind = root.adapter.contentType
        if (kind !== "" && !OutputListModel.activeStyleAllows(kind)) {
            EventBus.notify(qsTr("%1 is switched off in this output's style.").arg(root.adapter.tabLabel),
                            "warning", qsTr("On air"), "live.style.gate")
            return
        }
        // Use the same page identity the pane preview uses. It maps the visible
        // preview slide to the whole output set so line stepping still has neighbors.
        //
        // ...BUT ONLY IF THAT SLIDE IS PART OF THE PICK. The preview index is
        // whatever the LAST pick set it to, and a double-click on a plain row does
        // not always re-seat it: click row 1-6, then double-click row 1, and this
        // staged 1-6's page while the list said 1 (reported: "when I click 1 it won't
        // show it, it will show 1-5"). A slide carrying a paragraph the pick does not
        // name is not this pick's page — fall through and build the pick itself.
        const previewed = root.previewSlides[root.previewSlideIndex]
        const shownParts = (previewed && previewed.parts) || []
        const inPick = shownParts.length > 0
            && shownParts.every((p) => root.selected.indexOf(Number(p.number)) >= 0)
        if (inPick && root.playSlideAt(root.previewSlideIndex))
            return
        const slides = root.buildSlides()
        if (slides.length === 0)
            return
        // The pick's own first piece: the pane's preview and list land on it after
        // this stage (see followLiveStep's operatorPiece), instead of on whatever
        // page the previous pick had left in previewSlideIndex.
        const first = ((slides[0] && slides[0].parts) || [])[0]
        if (first)
            root.operatorPiece = { number: Number(first.number),
                                   part: Number(first.partIndex ?? 0) }
        root.lineStepOwnerRef = root.referenceText
        root.goLiveRequested(root.referenceText, slides)
        if (first)
            root.rememberPick(slides, 0, first.number, first.partIndex ?? 0)
        else
            root.scrollPiece = null
        // WITH SCROLL ON THE PASSAGE GOES OUT NOW, not on the first arrow: the
        // tile draws the budget-sized column and fills its box with the following
        // paragraphs. Scroll off leaves the pick a slide, fitted to its own words
        // (the auto-size above).
        if (LiveOutputService.scrollMode)
            root.ensureScrollSource()
        root.refreshLiveFlowSlide()
    }

    // Floating pill buttons: previous / next picked passage (FreeShow's
    // _moveSelection), play what's picked. Wired by the wrappers to the live
    // service (`goLive`, which STAGES the Main Output tile) — the step follows
    // through to that tile on every press; the wall's own ‹ › relays here too.
    // NOTE for anyone reintroducing an ownership test: it has to be taken
    // BEFORE root.step(), because after it the reference text is already the NEW
    // passage while the tile's title is still the old one, so a post-step
    // liveIsOurs(newRef) never matches.
    function stepAndPlay(direction) {
        // SCROLL MODE: the arrows walk this pane's passage line by line. A boundary
        // press stays in scroll mode instead of turning into a slide or passage jump.
        const owned = LiveOutputService.live
            ? LiveOutputService.onAirTitle === root.lineStepOwnerRef
            : LiveOutputService.stagedIsOurs(root.lineStepOwnerRef)
        if (LiveOutputService.scrollMode && root.scrollPiece && owned) {
            LiveOutputService.lineStep(direction)
            return
        }
        // THE ‹ › DELIBERATELY STAGE (per the pane's design): a jump, so each output
        // fits the page it lands on instead of reusing the previous page's size.
        LiveOutputService.noteOperatorJump()
        // THE MONITOR PREVIEW FOLLOWS THE ARROWS (user call: "the ‹ › should
        // move the slide in the monitor preview — the ones there do"). A step is
        // a presentation action, so it STAGES the slide it lands on whether or
        // not this pick was already what the tile showed: before this, a pick
        // that was neither on air nor staged left the tile frozen on whatever
        // was there, which read as dead arrows. Staging is still only the tile
        // preview — GO LIVE remains the one thing that puts it on air.
        // A RESULT THE ARROWS WALKED TO IS THE PICK (user call: "the <> should move
        // to their paragraph preview"): the highlighted search row is opened before
        // the step, so the ‹ › walk THAT sermon's paragraph preview instead of
        // stepping whatever happened to be open before the search.
        if (root.matchCursor >= 0 && root.matchCursor < root.citationMatches.length) {
            const row = root.citationMatches[root.matchCursor]
            if (row && row.ref) {
                root.applySuggestion(row.ref)
                root.clearCitationMatches()
            }
        }
        // THE PICK'S OWN SLIDES FIRST: a paragraph the engine split into several
        // slides (4, 4-2, 4-3 — the same continuations the verse list shows as
        // its own rows) is walked part by part, and only the step past its last
        // part moves on to the neighbouring passage.
        if (root.stepSlide(direction, true))
            return
        const before = root.referenceText
        const moved = root.step(direction)
        if (!moved && root.referenceText === before)
            return          // nothing that way (the first/last passage of them all)
        if (!moved) {
            // INTO THE NEIGHBOURING PASSAGE at the end the step came FROM: forward
            // lands on its first slide, back on its last (walking out of 4-3 goes
            // to 5; walking back out of 4 goes to 3's final part). The cursor is
            // set rather than left wherever the previous passage left it, which
            // used to show a part of the new passage that neither arrow had
            // walked to.
            if (root.previewSlides.length > 0)
                root.previewSlideIndex = direction < 0 ? root.previewSlides.length - 1 : 0
        }
        // Deferred so a same-chapter step publishes the piece focusRow pointed
        // the preview at (a continuation row's own slide), not the paragraph's
        // first one.
        Qt.callLater(function () { root.playSlideAt(root.previewSlideIndex) })
    }

    // ‹ › INSIDE the current pick: move the pane's preview to the neighbouring
    // slide of the same passage, and — when the pick IS what the tile is showing
    // (`publish`) — put that one slide on it. Returns false when there is no
    // slide that way (a single-slide pick, or already at the end), which is what
    // lets the caller fall through to the passage step in step().
    //
    // A step taken while this tab's content is NOT the one shown only moves the
    // pane's own preview: browsing must never hijack the output.
    function stepSlide(direction, publish) {
        const slides = root.previewSlides
        if (slides.length < 2)
            return false                          // nothing to walk: one slide
        const next = root.previewSlideIndex + direction
        if (next < 0 || next >= slides.length)
            return false                          // at this end of the pick
        root.previewSlideIndex = next             // the pane's preview follows at once
        // The highlight walks the CONTINUATION rows now (4 -> 4-2 -> 4-3), so the
        // list has to follow it — the row of the piece the preview landed on, not
        // of the paragraph's head (see rowIndexForPart).
        const piece = (slides[next].parts || [])[0]
        if (piece) {
            // The ‹ › has just put this piece on the list and in the preview, so the
            // follow must not walk them back to the paragraph's head (see
            // followLiveStep's operatorPiece).
            root.operatorPiece = { number: Number(piece.number), part: Number(piece.partIndex ?? 0) }
            root.centerRow(piece.number, piece.partIndex ?? 0, true)
        }
        if (publish)
            root.playSlideAt(next)
        return true
    }

    // The OUTPUT slide a preview slide stands for. Both lists name a
    // continuation by the paragraph it belongs to plus its part, so that pair is
    // the identity to match on (the same mapping selectSubpart makes).
    // Returns { slide, output, index } — the matching slide, the WHOLE output
    // array it came from, and its position in that array — or null. The
    // array+position (not just the one slide) is what playSlideAt below needs.
    function outputSlideAt(index) {
        const slides = root.previewSlides
        if (index < 0 || index >= slides.length)
            return null
        const parts = slides[index].parts || []
        if (parts.length === 0)
            return null
        const number = parts[0].number
        const partIndex = parts[0].partIndex ?? 0
        const output = root.chapterOutputSlides()
        for (let i = 0; i < output.length; ++i) {
            const slide = output[i]
            if ((slide.parts || []).some((part) => part.number === number
                                                   && (part.partIndex ?? 0) === partIndex))
                return { slide: slide, output: output, index: i }
        }
        return null
    }

    // ONE slide on the tile/air — exactly the pick a continuation row's click
    // makes (see selectSubpart), so the pane's preview and the tile/air agree
    // on which part of the passage is being shown rather than the tile
    // sitting on part one.
    //
    // Stages the WHOLE output set via stageSlideSet, not stageSlides(name,
    // [slide]): the line stepper (Alt+Down/Alt+Up, LiveOutputService.
    // lineStep) reads its neighbour slide's lines from that whole set, same
    // as ShowCenter.qml's takeSlideLive — a single-slide stage left it with
    // no neighbour to step into (user report: "when I press Alt[+]down it
    // doesn't step in lines").
    // Scroll picks retain the clicked JSON paragraph/verse and rebuild the reading
    // stream from it; slide picks continue to use the staged slide set.
    property var scrollPiece: null
    property var scrollOutput: null
    property int scrollIndex: 0
    property bool scrollSourced: false
    // THE PAGES THE WALK WAS BUILT FROM ({ body, start } each, see
    // readingPagesFromChapter), kept because Scroll-off has to map the page the
    // service is ON back to the slide holding that character. It was read without
    // ever being declared — "Cannot read property 'length' of undefined" on every
    // Scroll toggle (applyScrollMode).
    property var scrollPages: []
    function ensureScrollSource() {
        // The active reading tab supplies its own JSON passage and word budget.
        // Other content, such as a show's slides, has no chapter passage to read.
        const reading = root.adapter ? String(root.adapter.contentType || "") : ""
        if (root.scrollSourced || !root.scrollPiece || !root.book || !root.adapter
                || (reading !== "table" && reading !== "scripture"))
            return
        const ours = LiveOutputService.live
            ? LiveOutputService.onAirTitle === root.lineStepOwnerRef
            : LiveOutputService.stagedIsOurs(root.lineStepOwnerRef)
        if (!ours)
            return
        root.scrollSourced = true
        root.syncReadingWordBudget()   // the walk's budget, read where the walk begins
        const source = root.readingPagesFromChapter(root.scrollPiece.number,
                                                   root.scrollPiece.part)
        root.scrollPages = source.pages
        LiveOutputService.setReadingTextSource(root.lineStepOwnerRef, source.pages, source.index)
    }
    // SCROLL ON REBUILDS THE OUTPUT: the pick's passage goes out at once, so the
    // monitor tiles and this pane's preview draw the budget-sized scroll text
    // instead of waiting for the first arrow press.
    // THE SET THIS PANE LAST STAGED, so Scroll off can put the slides back exactly as
    // they were staged (scrollOutput at scrollIndex) and Scroll on can rebuild the
    // passage from the same piece.
    function rememberPick(output, index, number, part) {
        root.scrollOutput = output
        root.scrollIndex = index
        root.scrollPiece = { number: Number(number), part: Number(part) }
        root.scrollSourced = false
        // In Scroll, a click shows the chapter text from the clicked paragraph on, in
        // the template's box at the budget size, not the slide's own split.
        if (LiveOutputService.scrollMode)
            root.ensureScrollSource()
    }
    function applyScrollMode() {
        if (!root.scrollPiece || !root.scrollOutput)
            return
        const ours = LiveOutputService.live
            ? LiveOutputService.onAirTitle === root.lineStepOwnerRef
            : LiveOutputService.stagedIsOurs(root.lineStepOwnerRef)
        if (!ours)
            return
        if (LiveOutputService.scrollMode) {
            root.scrollSourced = false
            root.ensureScrollSource()
        } else {
            // WHERE THE WALK IS: the scroll page the service is on starts at a character
            // of the chapter, and the slide holding that character is the one restaged.
            const set = root.scrollOutput
            let index = root.scrollIndex
            if (root.scrollSourced && root.scrollPages.length > 0) {
                const page = root.scrollPages[Math.min(LiveOutputService.readingPage,
                                                       root.scrollPages.length - 1)]
                const piece = root.pieceAtChapterOffset(Number(page.start ?? 0))
                const found = piece ? set.findIndex((slide) => (slide.parts || []).some(
                    (p) => Number(p.number) === piece.number && Number(p.partIndex ?? 0) === piece.partIndex))
                                    : -1
                if (found >= 0)
                    index = found
            }
            root.scrollSourced = false
            root.scrollIndex = index
            LiveOutputService.stageSlideSet(root.lineStepOwnerRef, set, index)
        }
        root.refreshLiveFlowSlide()
    }
    Connections {
        target: LiveOutputService
        function onLineStepPending(direction) { root.ensureScrollSource() }
        function onScrollModeChanged() { root.applyScrollMode() }
    }

    function playSlideAt(index) {
        const hit = root.outputSlideAt(index)
        if (!hit)
            return false
        root.lineStepOwnerRef = root.referenceText
        LiveOutputService.stageSlideSet(root.referenceText, hit.output, hit.index)
        const piece = (root.previewSlides[index].parts || [])[0]
        root.rememberPick(hit.output, hit.index, piece.number, piece.partIndex ?? 0)
        root.refreshLiveFlowSlide()
        return true
    }

    // Stage the identity carried by a pane row directly. A selection change and
    // its preview binding can settle in separate QML updates, so a double-click
    // must not use previewSlideIndex to decide which row was actually clicked.
    function stageOutputPart(number, partIndex) {
        if (!root.book || !root.adapter)
            return false
        const kind = root.adapter.contentType
        if (kind !== "" && !OutputListModel.activeStyleAllows(kind)) {
            EventBus.notify(qsTr("%1 is switched off in this output's style.").arg(root.adapter.tabLabel),
                            "warning", qsTr("On air"), "live.style.gate")
            return false
        }
        // A CLICK ON A PARAGRAPH IS A JUMP: fit this page's own auto-size.
        LiveOutputService.noteOperatorJump()
        const part = Number(partIndex ?? 0)
        // ...and the pane already shows this piece: the follow must not take the
        // list back to the paragraph's head row (see followLiveStep).
        root.operatorPiece = { number: Number(number), part: part }
        // Preserve the exact row slide produced by the old pick path
        // (buildSlides -> goLiveRequested). The chapter set supplies line-step
        // neighbours; it should not replace the picked page with a second,
        // independently rebuilt copy.
        const picked = root.buildSlides()
        let pickedSlide = null
        for (const slide of picked) {
            if ((slide.parts || []).some((p) => Number(p.number) === Number(number)
                                               && Number(p.partIndex ?? 0) === part)) {
                pickedSlide = slide
                break
            }
        }
        root.lineStepOwnerRef = root.adapter.reference(root.book.name,
                                                       root.chapterNumber,
                                                       [Number(number)])
        if (kind === "table") {
            if (!pickedSlide)
                return false
            LiveOutputService.stageSlideSet(root.lineStepOwnerRef, [pickedSlide], 0)
            // The whole chapter set is what Scroll off restores, so its neighbours exist.
            const set = root.chapterOutputSlides()
            const at = set.findIndex((slide) => (slide.parts || []).some(
                (p) => Number(p.number) === Number(number) && Number(p.partIndex ?? 0) === part))
            root.rememberPick(at >= 0 ? set : [pickedSlide], at >= 0 ? at : 0, number, part)
        } else {
            const output = root.chapterOutputSlides()
            let staged = false
            for (let i = 0; i < output.length; ++i) {
                if (!(output[i].parts || []).some((p) => Number(p.number) === Number(number)
                                                         && Number(p.partIndex ?? 0) === part))
                    continue
                if (pickedSlide)
                    output[i] = pickedSlide
                LiveOutputService.stageSlideSet(root.lineStepOwnerRef, output, i)
                root.rememberPick(output, i, number, part)
                staged = true
                break
            }
            if (!staged)
                return false
        }
        // A CLICK WITH SCROLL ON IS A READING PICK: stage the passage itself, at the
        // size the text area and the max-words budget solve, instead of fitting this
        // one page's own words. That is the difference the mode makes — the click
        // still picks, it just stops auto-fitting when the tab is scrolling.
        if (LiveOutputService.scrollMode)
            root.ensureScrollSource()
        root.refreshLiveFlowSlide()
        return true
    }

    // ---- The wall's ‹ ›: is THIS tab the one that can step the tile? --------
    // The wall's transport answers the same question stepAndPlay does — is my
    // content the one on the tile (on air or staged)? — and its arrows are lit
    // only while some tab says yes (LiveOutputService.setWallStepper), so an
    // arrow can never be enabled with nothing behind it. The pane ANNOUNCES on
    // every change that can move the answer: its own reference text, and the
    // tile's live/staged state.
    property string wallStepperId: ""
    function refreshWallStepper() {
        const id = root.adapter && root.adapter.contentType ? root.adapter.contentType : ""
        if (id === "" || !root.adapter.stagedIsOurs) {
            if (root.wallStepperId !== "")
                LiveOutputService.setWallStepper(root.wallStepperId, false)
            return
        }
        root.wallStepperId = id
        // lineStepOwnerRef when an active pick has one (see its own comment on
        // root.selected) — NOT the live root.referenceText, which for The Table
        // includes the current paragraph number and DRIFTS as followLiveStep
        // tracks the stepper into a new one ("47-1100X - Fellowship 1" ->
        // "...Fellowship 6"). LiveOutputService's own stagedName_/onAirTitle
        // stay fixed at the ORIGINAL pick, so a drifted reference read here as
        // "this isn't my content any more" and called setWallStepper(id,
        // false) — withdrawing stepper status mid-step, every single time the
        // selector moved (user report: "it[s] update is moving to the next,
        // that cause[s] an update on the monitor[s]" — this handler fires on
        // EVERY onReferenceTextChanged, which followLiveStep's own
        // root.selected write triggers).
        const ref = root.lineStepOwnerRef !== "" ? root.lineStepOwnerRef : root.referenceText
        const ours = (root.adapter.liveIsOurs && root.adapter.liveIsOurs(ref))
                  || (root.adapter.stagedIsOurs && root.adapter.stagedIsOurs(ref))
        LiveOutputService.setWallStepper(id, ours)
    }
    onReferenceTextChanged: root.refreshWallStepper()
    Connections {
        target: LiveOutputService
        function onLiveChanged() { root.refreshWallStepper() }
        function onStagedChanged() { root.refreshWallStepper() }
        function onOnAirChanged() { root.refreshWallStepper() }
    }

    // ---- Bibles ----------------------------------------------------------------------------------------------------
    LibrarySidebar {
        id: sidebar
        height: parent.height
        defaultWidth: 150
        minWidth: 120
        maxWidth: 320

        Rectangle {
            id: sourcesHeader
            x: 8; y: 8; width: parent.width - 16; height: 27
            color: Theme.panelBg; radius: 6
            Text {
                x: 10; y: 7
                text: (root.adapter ? root.adapter.sidebarLabel : qsTr("Sources"))
                color: Theme.textSecondary
                font.family: Theme.fontFamily; font.pixelSize: 13; font.bold: true
            }
        }

        Flickable {
            id: sourcesFlick
            objectName: "selfTestSourcesList-" + root.paneTag   // (the scroll probe scrolls it by name)
            x: 8; y: 42; width: parent.width - 16; height: parent.height - 42 - 48
            clip: true
            contentHeight: sourceList.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: sourceList
                width: parent.width
                spacing: 4

                Repeater {
                    model: root.sources
                    delegate: Rectangle {
                        id: sourceRow
                        required property var modelData
                        readonly property bool active: modelData.id === root.sourceId
                        width: sourceList.width; height: 29; radius: 6
                        color: active ? Theme.accentSoft : "transparent"

                        // The pointer's wash is the app's shared hover layer
                        // (HoverGlow) — a row here lights exactly like a row in
                        // the Shows list, instead of with its own private grey.
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: sourceHoverPA.hovered
                            glowColor: Theme.hoverBg
                        }

                        Rectangle { visible: sourceRow.active; x: 0; y: 4; width: 3; height: 21; color: Theme.accent; radius: 1.5 }
                        IconGlyph {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            name: "bible"
                            color: sourceRow.active ? Theme.accent : root.textDim
                            width: 12; height: 12
                        }
                        Text {
                            x: 28; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 34
                            text: { root.probeEvals.source++; return sourceRow.modelData.name }   // (scroll-cost probe: the row's label binding)
                            color: sourceRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        // Position-truth hover (the verse rows' pattern) —
                        // a HoverHandler's hovered suffers this build's
                        // broken hover delivery, same as MouseArea.
                        PositionHoverArea { id: sourceHoverPA; anchors.fill: parent; checkAncestors: false; showCursor: false }
                        TapHandler { onTapped: { root.column = 0; root.openSource(sourceRow.modelData.id) } }
                    }
                }

                // The empty/reloading hint. While the library is really being read a
                // small spinner turns beside the words (the app's BusySpinner — it only
                // animates while visible), so a long read looks alive instead of
                // stalled. Laid out by hand rather than in a Row: a positioner would
                // not let the spinner take the text's line height for its centre.
                Item {
                    visible: root.sources.length === 0
                    x: 4; width: parent.width - 8
                    height: hintText.implicitHeight

                    BusySpinner {
                        id: sidebarSpinner
                        visible: root.showReloading
                        width: 13; height: 13
                        thickness: 2
                        color: Theme.textMuted
                        y: Math.max(0, (hintText.implicitHeight - height) / 2)
                    }
                    Text {
                        id: hintText
                        x: sidebarSpinner.visible ? 19 : 0
                        width: parent.width - x
                        text: root.libraryHint()
                        color: Theme.textMuted
                        wrapMode: Text.Wrap
                        font.family: Theme.fontFamily; font.pixelSize: 13
                    }
                }
            }
        }

        // Many installed Bibles overflow the sidebar — a real (draggable,
        // click-to-jump) scrollbar, not wheel-only.
        AppScrollBar {
            flickable: sourcesFlick
            anchors.top: sourcesFlick.top
            anchors.bottom: sourcesFlick.bottom
            anchors.right: parent.right
            anchors.rightMargin: 2
        }

        // The add actions: the tab's main "add one" plus an optional bulk
        // "add a folder" (an adapter with addFolderLabel + importFolder gets
        // the second button and, while its importing() runs, a progress bar
        // fed by progress() { done, total }).
        Column {
            id: addActions
            x: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            width: parent.width - 16
            spacing: 6

            // The bulk import's progress bar (this tab only shows it while a
            // folder import runs; adapters without importing() never do).
            Rectangle {
                id: importProgress
                readonly property bool bulk: !!(root.adapter && root.adapter.importing && root.adapter.importing())
                readonly property var stats: (root.adapter && root.adapter.progress) ? root.adapter.progress() : ({}).valueOf()
                readonly property int done: stats.done !== undefined ? stats.done : 0
                readonly property int total: stats.total !== undefined ? stats.total : 0
                readonly property real fraction: total > 0 ? Math.min(1, done / total) : 0

                visible: bulk
                width: parent.width
                height: bulk ? 22 : 0
                radius: 6
                color: Theme.windowBg
                border.color: Theme.border

                Rectangle {
                    x: 2; y: 2
                    width: Math.max(0, parent.width - 4) * importProgress.fraction
                    height: parent.height - 4
                    radius: 4
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
                Text {
                    anchors.centerIn: parent
                    text: importProgress.total > 0
                          ? qsTr("%1 / %2").arg(importProgress.done).arg(importProgress.total)
                          : ""
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }

            SidebarAddButton {
                readonly property bool hasFolder: !!(root.adapter && root.adapter.addFolderLabel && root.adapter.importFolder)
                visible: hasFolder
                enabled: !(hasFolder && root.adapter.importing && root.adapter.importing())
                width: parent.width
                text: {
                    if (!hasFolder)
                        return ""
                    if (root.adapter.importing && root.adapter.importing())
                        return qsTr("Importing…")
                    return root.adapter.addFolderLabel
                }
                onClicked: root.adapter.importFolder()
            }
            SidebarAddButton {
                width: parent.width
                // One install at a time: the button waits out a running import
                // (the service rejects a second one anyway).
                enabled: !(root.adapter && root.adapter.importing && root.adapter.importing())
                text: (root.adapter ? root.adapter.addLabel : qsTr("Add"))
                onClicked: root.adapter.importNew()
            }
        }
    }

    // ---- Books ---------------------------------------------------------------------------------------------------------
    Rectangle {
        id: booksCol
        x: sidebar.width; y: 0
        visible: !root.compactTableSearch
        width: root.compactTableSearch ? 0 : root.booksColWidth; height: parent.height
        color: Theme.panelBg

        Flickable {
            id: booksFlick
            objectName: "selfTestBooksList-" + root.paneTag   // (the scroll probe scrolls it by name)
            anchors.fill: parent
            clip: true
            contentHeight: booksList.height + 8
            boundsBehavior: Flickable.StopAtBounds
            // Wheel-only before: no scrollbar on this column at all.

            Column {
                id: booksList
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: root.visibleBooks
                    delegate: Item {
                        id: bookRow
                        required property var modelData
                        // The SEARCH's own cursor wins while one is up (see bookCursor): the
                        // selector sits on a year without opening it.
                        readonly property bool active: root.bookCursor >= 0 && root.books[root.bookCursor]
                                                       ? modelData.id === root.books[root.bookCursor].id
                                                       : (root.book !== null && modelData.id === root.book.id)
                        width: booksList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: bookRow.active ? Theme.activeBg : "transparent"
                            HoverGlow {
                                anchors.fill: parent
                                radius: parent.radius
                                hovered: bookHoverPA.hovered
                                glowColor: Theme.hoverBg
                            }
                            // WHICH COLUMN OWNS THE SELECTOR: the accent bar is red on the
                            // column the ‹ › are on and dim elsewhere, so moving the selector
                            // with ‹ › is something you can SEE (user call: "when i <> it did
                            // not move across the panes").
                            Rectangle { visible: bookRow.active; x: 0; y: 4; width: 2; height: parent.height - 8
                                        color: root.column === 0 ? Theme.accent : Theme.border }
                        }
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 20
                            text: { root.probeEvals.book++; return bookRow.modelData.name }   // (scroll-cost probe: the row's label binding)
                            color: bookRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 16
                        }
                        PositionHoverArea {
                            id: bookHoverPA
                            anchors.fill: parent
                            checkAncestors: false
                            showCursor: false
                            onClicked: {
                                const bi = root.books.findIndex((b) => b.id === bookRow.modelData.id)
                                // IN A SEARCH the click updates EVERY pane that follows the
                                // year: the sermons column opens on this year (as it always
                                // did) AND the answer beside it is narrowed to it. Before
                                // this, only the sermons moved — the results pane kept the
                                // previous year's rows (the reported "it updated the next
                                // pane but the 3rd pane did not update").
                                if (root.searchMode) {
                                    root.openBook(bi, 0)
                                    root.bookCursor = bi
                                    root.chapterCursor = 0
                                    root.narrowMatches()
                                    return
                                }
                                root.column = 0
                                root.openBook(bi, 0)
                            }
                        }
                    }
                }
            }
        }
        AppScrollBar {
            flickable: booksFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 2; anchors.bottomMargin: 2
            anchors.right: parent.right; anchors.rightMargin: 3
        }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
        SplitHandle {
            // A Bible's book names need real room, but The Table's "books"
            // column holds only a 4-digit year — 100px was a floor sized for
            // the wrong content and left it wasting space the sermon-titles
            // column beside it needed.
            value: booksCol.width; minValue: 50; maxValue: 340
            onDragged: (v) => root.booksColWidth = v
            onResetRequested: root.booksColWidth = 150
        }
    }

    // ---- Chapters --------------------------------------------------------------------------------------------------------
    // A Bible's chapters are just numbers (1, 2, 3…), so the column stays a narrow number picker. The Table's "chapters" are sermons,
    // each with its own name (adapter's `book.chapterTitles`, parallel to `book.chapters`) - when present, that name is what is shown,
    // and the column widens to fit it, same idea as the books column beside it.
    Rectangle {
        id: chaptersCol
        // !!(...) rather than the bare && chain: when root.book is undefined (not null), "undefined && x" is
        // undefined, not false, and QML logs "Unable to assign [undefined] to bool" trying to store that in
        // this readonly bool property - coerce it explicitly instead of relying on JS's falsy short-circuit.
        readonly property bool hasTitles: !!(root.book && root.book.chapterTitles && root.book.chapterTitles.length > 0)
        // Widened once, the first time there turn out to be names to show (a bare Bible chapter number needs far less room than a
        // sermon's title) - after that the user's own drag is what decides, same as every other column here.
        property bool widenedForTitles: false
        visible: !root.compactTableSearch
        onHasTitlesChanged: if (hasTitles && !widenedForTitles && !root.chaptersWidthRemembered) { widenedForTitles = true; root.chaptersColWidth = 190 }
        x: booksCol.x + booksCol.width; y: 0
        width: root.compactTableSearch ? 0 : root.chaptersColWidth; height: parent.height
        color: Theme.windowBg

        Flickable {
            id: chaptersFlick
            objectName: "selfTestChaptersList-" + root.paneTag   // (the scroll probe scrolls it by name)
            anchors.fill: parent
            clip: true
            contentHeight: chapterList.height + 8
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: chapterList
                x: 4; y: 4; width: parent.width - 8
                Repeater {
                    model: root.book ? root.book.chapters : []
                    delegate: Item {
                        id: chapterRow
                        required property int modelData
                        required property int index
                        readonly property string title: chaptersCol.hasTitles ? root.book.chapterTitles[chapterRow.index] : ""
                        readonly property bool active: root.chapterCursor >= 0
                                                       ? modelData === root.chapterCursor
                                                       : modelData === root.chapterNumber
                        width: chapterList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: chapterRow.active ? Theme.activeBg : "transparent"
                            HoverGlow {
                                anchors.fill: parent
                                radius: parent.radius
                                hovered: chapterHoverPA.hovered
                                glowColor: Theme.hoverBg
                            }
                            Rectangle { visible: chapterRow.active; x: 0; y: 4; width: 2; height: parent.height - 8
                                        color: root.column === 1 ? Theme.accent : Theme.border }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: chaptersCol.hasTitles ? parent.left : undefined
                            anchors.leftMargin: chaptersCol.hasTitles ? 10 : 0
                            anchors.horizontalCenter: chaptersCol.hasTitles ? undefined : parent.horizontalCenter
                            width: chaptersCol.hasTitles ? parent.width - 18 : undefined
                            text: { root.probeEvals.chapter++; return chapterRow.title !== "" ? chapterRow.title : chapterRow.modelData }   // (scroll-cost probe: the row's label binding)
                            elide: Text.ElideRight
                            color: chapterRow.active ? Theme.textPrimary : root.textDim
                            font.family: Theme.fontFamily; font.pixelSize: 16
                        }
                        PositionHoverArea {
                            id: chapterHoverPA
                            anchors.fill: parent
                            checkAncestors: false
                            showCursor: false
                            onClicked: {
                                // IN A SEARCH: the same cascade one level down — the
                                // sermon opens and the answer narrows to it.
                                if (root.searchMode) {
                                    root.bookCursor = root.bookCursor >= 0 ? root.bookCursor : root.bookIndex
                                    root.chapterCursor = chapterRow.modelData
                                    root.openChapter(chapterRow.modelData, true)
                                    root.narrowMatches()
                                    return
                                }
                                root.column = 1
                                root.openChapter(chapterRow.modelData, true)
                            }
                        }
                    }
                }
            }
        }
        AppScrollBar {
            flickable: chaptersFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 2; anchors.bottomMargin: 2
            anchors.right: parent.right; anchors.rightMargin: 3
        }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
        SplitHandle {
            value: chaptersCol.width; minValue: chaptersCol.hasTitles ? 100 : 40; maxValue: 420
            onDragged: (v) => root.chaptersColWidth = v
            onResetRequested: root.chaptersColWidth = chaptersCol.hasTitles ? 190 : 52
        }
    }

    // ---- Verses (or the search results) ---------------------------------------------------------------------------------------
    Rectangle {
        id: versesCol
        x: chaptersCol.x + chaptersCol.width; y: 0
        width: parent.width - x - previewCol.width; height: parent.height
        color: Theme.referenceContentBg

        // THE PARAGRAPH ROWS. A LISTVIEW, not a Column-and-Repeater: only the rows
        // in view are built. A sermon is one row per output slide once its chapter
        // is divided — the shipped library's longest is 546 rows — and the old
        // Column built every one of them the moment the chapter arrived, for a
        // viewport that shows nine (measured: 546 rows realized, 19732 px of
        // content, 317 px of view). The list now builds what fits on screen, plus
        // ListView's own small cache, and builds more as it is scrolled.
        //
        // The geometry the rest of the pane assumes is unchanged: a 4 px header
        // (the rows' old y: 4), 38 px rows (34 for a continuation, 122 while the
        // note editor is open), 60 px of room under the last row. What IS different
        // is that a row which is off screen does not EXIST — so anything that needs
        // a row that is not built (a reveal, a self-test click) asks for it by
        // index through revealRow(), which both scrolls and materializes it.
        ListView {
            id: versesFlick
            objectName: "selfTestVersesList-" + root.paneTag   // (the scroll probe scrolls it by name)
            anchors.fill: parent
            visible: !root.searching && root.citationMatches.length === 0
            clip: true
            // The paragraph list is already the one list in the app that does NOT
            // rebuild on a filter or a tab switch: the probe measured 28 rows built
            // once and never dropped, for a 945-row chapter (this ListView, and the
            // panes keeping their width — see the dock's own note). Its rows are
            // rebuilt only when the CHAPTER itself changes, which is a real model
            // change and not a churn to remove.
            //
            // SCROLLING was the other half of that, and the scroll probe measured it:
            // for a 945-row chapter, one 32 831 px scroll built 288 rows and threw 296
            // away (~29 fps, ~11 frames over 33 ms) — every row the viewport passed
            // was re-created and re-shaped, and the chapter was on screen the whole
            // time, so nothing about the model had changed. The shows table in this
            // app already scrolls the same distance building 22 rows and dropping
            // none, and the one difference is `reuseItems`: ListView keeps the rows
            // it has finished with in a pool and RE-BINDS them to the next index
            // instead of destroying and re-creating them. Every binding in this
            // delegate already reads `modelData` (including the objectName the
            // self-tests address rows by, and the `oneLine` text), so a rebound row
            // is a correct row — which is the same posture the shows rows and this
            // list's own recycled-row notes (SelfTestDriver's reuse-pool comment)
            // already assume.
            reuseItems: true
            model: root.mainRows
            boundsBehavior: Flickable.StopAtBounds
            // The row at the top of the list is part of the remembered place.
            onMovementEnded: root.touchReading()
            header: Item { height: 4 }
            footer: Item { height: 60 }
            delegate: Item {
                id: verseRow
                // LAYOUT PROBE (VGR_LAYOUT_TEST).
                Component.onCompleted: root.verseRowBuilds++
                Component.onDestruction: root.verseRowDrops++
                // Self-test handle: the verse double-click scenario
                // (double-click goes on air) addresses rows by name. A divided
                // paragraph shows SEVERAL rows for one number, so the name carries
                // the piece as well, spelled exactly as the row's own label spells
                // it ("selfTestVerseRow_12", "selfTestVerseRow_12-1") — one row per
                // number was not enough to address a continuation row at all.
                objectName: "selfTestVerseRow_" + Number(modelData.number)
                            + (continuation ? "-" + partIndex : "")
                required property var modelData
                required property int index
                readonly property bool continuation: modelData.continuation === true
                readonly property int verseNumber: Number(modelData.number)
                readonly property int partIndex: Number(modelData.partIndex ?? 0)
                readonly property bool isSplit: modelData.isSplit === true || continuation
                readonly property bool active: root.rowIsActive(verseNumber, partIndex, isSplit)
                // One line per row, like FreeShow's own verse list (the full paragraph is what the preview pane on the right is
                // for). A verse spanning several of the sermon's own blank-line paragraphs still carries their break as a literal
                // "\n" in its text - elide alone does not collapse a real line break, so it is flattened to a space here too.
                readonly property string oneLine: { root.probeEvals.verse++; return verseRow.modelData.text.replace(/\s*\n+\s*/g, " ") }   // (scroll-cost probe: the row's text binding)
                // The user's marks, from the ONE batched chapter read
                // (root.chapterMarks) — no per-row engine calls and no per-row JS:
                // mainRows owns that dependency and put the two flags on the row,
                // so the verse number wears gold from a value rather than from two
                // markFor() calls on every evaluation of this delegate.
                readonly property bool marked: modelData.marked === true
                readonly property bool hasNote: modelData.hasNote === true
                // The open note editor stretches this one row to make room for itself.
                width: versesFlick.width - 8
                height: continuation ? 34 : (root.noteVerse === verseNumber ? 122 : 38)
                Rectangle {
                    anchors.fill: parent; anchors.margins: 1; radius: 4
                    // Position-truth hover (PositionHoverArea), not DragSource's
                    // containsMouse — the latter LATCHES in this build (hover-enter
                    // delivers, hover-exit never does; see PositionHoverArea's
                    // header), which is exactly the "verse row stays highlighted
                    // after I click it" report.
                    // An inactive row carries the app's shared list stripe on odd
                    // rows (ListRows); the open verse keeps its own solid ground.
                    color: verseRow.active ? Qt.alpha(Theme.accent, 0.24)
                                           : ListRows.stripeOverlay(verseRow.index)
                    border.width: verseRow.active ? 1.5 : 0
                    border.color: verseRow.active ? Qt.alpha(Theme.accentLight, 0.95) : "transparent"
                    // GLIDES, not a hard cut — the line stepper (followLiveStep)
                    // can flip `active` off this row and onto the next one in the
                    // same tick, and the two Behaviors crossfade them: this row's
                    // highlight fades out while the next row's fades in, instead
                    // of the selection snapping straight from one to the other
                    // (user call: "make the selector gliding between to[the]
                    // list[s] so it shows that it['s] in between two").
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    Behavior on border.width { NumberAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    HoverGlow {
                        anchors.fill: parent
                        radius: parent.radius
                        hovered: verseHover.hovered
                        glowColor: Theme.hoverBg
                    }
                }
                PositionHoverArea {
                    id: verseHover
                    anchors.fill: parent
                    checkAncestors: false   // (rows inside a Flickable; no hover-gated visibility in the chain)
                    clickEnabled: false     // the DragSource below owns verse clicks and drags
                }
                // The row itself stays one line (the preview pane is the full
                // read — see the row's own comment above), but a narrow pane
                // can hide that preview; hover gives the full text either way.
                HoverDetail {
                    anchors.fill: parent
                    // A scrolling list of verse rows, not a toolbar sweep —
                    // each row earns its own showDelay (see
                    // HoverDetail.alwaysDelay) instead of popping up the
                    // instant the pointer crosses from the row above it.
                    alwaysDelay: true
                    title: verseRow.continuation
                           ? qsTr("%1-%2").arg(verseRow.verseNumber).arg(verseRow.partIndex)
                           : String(verseRow.verseNumber)
                    detail: verseRow.oneLine
                }
                // The verse number: the usual red, gold while the user has
                // marked this verse with a highlight or a note.
                Text {
                    x: verseRow.continuation ? 18 : 10; anchors.verticalCenter: parent.verticalCenter
                    width: 40; horizontalAlignment: Text.AlignRight
                    text: verseRow.continuation
                        ? qsTr("%1-%2").arg(verseRow.verseNumber).arg(verseRow.partIndex)
                        : verseRow.verseNumber
                    color: verseRow.marked ? Theme.markedInk : Theme.accent
                    font.family: Theme.fontFamily; font.pixelSize: 16; font.bold: true
                }
                Text {
                    x: verseRow.continuation ? 68 : 60
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (verseRow.continuation ? 114 : 106)
                    // While a search (or citation filter) is showing, the typed
                    // words light up yellow in the visible text — the same obvious
                    // chip Quick search uses.
                    text: root.canHighlight ? root.highlight(verseRow.oneLine) : verseRow.oneLine
                    color: Theme.textPrimary
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                    textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                    font.family: Theme.fontFamily; font.pixelSize: 16
                }
                // Click selects (Ctrl / Shift add); drag carries the selection into a project; DOUBLE-CLICK GOES ON AIR
                // (FreeShow's verse dblclick -> playScripture — the original report: "double click on the scripture
                // doesn't display on the monitor").
                DragSource {
                    id: verseSource
                    anchors.fill: parent
                    owner: root
                    // LAZY (not `payload:`): dragPayload asks the engine for the
                    // reference, and this delegate is created for EVERY paragraph of
                    // the sermon at once — ~200 engine calls at open cost ~2s of the
                    // click-to-open delay (measured). Evaluated once, at drag start.
                    payloadProvider: () => {
                        const p = root.dragPayload(verseRow.active ? root.selected : [verseRow.verseNumber])
                        return p ? { payload: p, label: p.items[0].name } : null
                    }
                    // `owner` is this pane. A click or double-click already
                    // in flight when the pane is rebuilt (a tab switch, a
                    // settings toggle the tab renders — table.splitLongVerses
                    // was the reported one) arrives with a DESTROYED pane,
                    // and a property read off a destroyed object is
                    // `undefined` rather than an error: that is the reported
                    // "Cannot read property 'playPicked' of undefined". The
                    // `typeof` half mirrors DropdownPanel's stale-row
                    // guards. A vanished pane simply has nothing to select
                    // into, so selection work is skipped, not attempted.
                    //
                    // The guard is INLINED — a property read on the
                    // DragSource, never a helper FUNCTION declared on it.
                    // selectVerse can rebuild the verse list synchronously,
                    // destroying this DragSource while its own handler is
                    // still running; calling a function member of a
                    // destroyed object throws "Property 'ownerPane' ... is
                    // not a function" and "attempted to evaluate a function
                    // in an invalid context" (the reported crash), whereas
                    // re-reading the plain `verseSource.owner` property is
                    // exactly the read that safely yields undefined.
                    // The modifiers come FROM THE CLICK (`activated`
                    // carries them). This used to read
                    // `Qt.application.keyboardModifiers`, which is
                    // `undefined` on Qt 6 — so the Ctrl/Shift halves of
                    // "Click selects (Ctrl / Shift add)" above were
                    // inert and every click was a plain single select.
                    onActivated: (mods) => {
                        if (typeof verseSource === "undefined" || !verseSource)
                            return
                        const pane = verseSource.owner
                        if (!pane)
                            return
                        // Row clicks are handled by DragSource/TapHandler and do not
                        // take keyboard focus. The search field can therefore keep
                        // activeFocus after a passage is clicked, making keysMine
                        // mistake ordinary arrow navigation for text editing.
                        pane.forceActiveFocus()
                        pane.column = 2      // (the paragraph column)
                        if (verseRow.isSplit) {
                            pane.selectSubpart(verseRow.verseNumber, verseRow.partIndex)
                        } else {
                            pane.selectVerse(verseRow.verseNumber, (mods & Qt.ControlModifier) !== 0, (mods & Qt.ShiftModifier) !== 0)
                        }
                    }
                    onOpened: {
                        if (typeof verseSource === "undefined" || !verseSource)
                            return
                        const pane = verseSource.owner
                        if (!pane)
                            return
                        if (pane.adapter && pane.adapter.contentType === "table")
                            EngineBridge.log("info", "TableRowDoubleClick",
                                "number=" + verseRow.verseNumber
                                + " partIndex=" + verseRow.partIndex
                                + " split=" + verseRow.isSplit)
                        // DOUBLE CLICK IS THE STAGING GESTURE (user call: "double click
                        // is what should push it to monitor preview, single click is
                        // preview pane"): the pane's preview followed the click, and
                        // this puts what it is showing on the MONITOR PREVIEW. A
                        // continuation row stages its own piece, not the whole
                        // paragraph.
                        if (verseRow.isSplit) {
                            pane.stageSubpart(verseRow.verseNumber, verseRow.partIndex)
                            return
                        }
                        pane.selectVerse(verseRow.verseNumber, false, false)
                        pane.stageOutputPart(verseRow.verseNumber, verseRow.partIndex)
                    }
                }
                // The note affordance: a pencil on hover, persistent while a note
                // exists or the editor is open (gold then). Click opens the inline
                // editor; Enter saves, Esc discards.
                Item {
                    id: noteBtn
                    readonly property bool editing: !verseRow.continuation && root.noteVerse === verseRow.verseNumber
                    visible: root.hasUserData && (verseHover.hovered || verseRow.hasNote || editing)
                    x: parent.width - 34; y: 7
                    width: 24; height: 24
                    Rectangle {
                        anchors.fill: parent; radius: 5
                        color: noteBtn.editing ? Theme.hoverBg : "transparent"
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: noteHover.hovered
                            glowColor: Theme.hoverBg
                        }
                    }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "pencil"
                        color: noteBtn.editing || verseRow.hasNote ? Theme.markedInk : Theme.textSecondary
                        width: 11; height: 11
                    }
                    HoverHandler { id: noteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.openNoteEditor(verseRow.verseNumber) }
                }
                // The note editor, open under its verse (one at a time).
                Rectangle {
                    visible: !verseRow.continuation && root.noteVerse === verseRow.verseNumber
                    x: 4; y: 40
                    width: parent.width - 8; height: 76
                    radius: 6
                    color: Theme.surface
                    border.color: Theme.border
                    TextInput {
                        id: noteInput
                        anchors.fill: parent; anchors.margins: 8
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily; font.pixelSize: 13
                        wrapMode: TextInput.Wrap
                        clip: true
                        // Seed on open + take focus HERE (delegate
                        // scope can see its own id; the root's
                        // functions can't).
                        onVisibleChanged: if (visible) {
                            text = root.markFor(verseRow.modelData.number).note || ""
                            forceActiveFocus()
                        }
                        Keys.onEscapePressed: root.noteVerse = 0
                        Keys.onReturnPressed: root.saveNoteDraft(text)
                    }
                    Text {
                        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6
                        text: qsTr("Enter to save · Esc to close")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 10
                    }
                }
            }
        }

        // The column's other two contents — the search hits and The Table's citation
        // matches (with the hover peek beside them) — each a LISTVIEW, like the
        // paragraph list above, and for the same reason: the row count is the QUERY's,
        // not the viewport's. Both used to be a Column-and-Repeater, which built every
        // row the moment the model landed: a two-character query matches hundreds of
        // sermons in a real library ("th" matches 641 of 1206 here), and the citation
        // list was capped at 150 only to bound that build — a cap that also hid the
        // rest of the matches from a list the operator can scroll. The modes are
        // mutually exclusive, so the two never share a viewport, and the scrollbar
        // follows whichever is showing. The container is a plain Item (not a Flickable):
        // each list scrolls itself now, and the peek beside them has its own.
        Item {
            id: resultsArea
            anchors.fill: parent
            clip: true

            ListView {
                id: hitsList
                objectName: "selfTestPaneHitList"   // (the pane-rows scenario reads it by name)
                visible: root.searching
                x: 0; y: 4; width: parent.width - 8; height: parent.height - 8
                clip: true
                model: root.searchResults
                boundsBehavior: Flickable.StopAtBounds
                // The engine's resolved-words hint rides ABOVE the rows (the old
                // Column's first child): a header, so it scrolls away with them
                // instead of eating the top of the viewport forever.
                header: Item {
                    width: hitsList.width
                    height: hitsHint.visible ? hitsHint.implicitHeight : 0
                    Text {
                        id: hitsHint
                    // The engine's resolved-words hint ("thn → then   freind →
                    // friend"), gold like the match highlight. Tab in the pill box
                    // accepts it — the same behavior Quick search has.
                    //
                    // Named for the pane-highlight scenario, which asserts this line
                    // is really ON SCREEN for a misspelled query (the user's ask:
                    // "the typo fixer should be there" in this box as well).
                    // Per-TAB, like the lists above: the same pane class serves two tabs and
                    // both hints are alive at once, so a bare name resolves to whichever pane
                    // the tree walk reaches first (that is how a scenario came to read the
                    // OTHER tab's empty hint and call the correction missing).
                    objectName: "selfTestPaneSearchHint-" + root.paneTag
                    visible: root.resolvedWords.some((w) => w.changed === true
                                                     || (w.candidates && w.candidates.length > 0))
                    width: parent.width
                    leftPadding: 10
                    color: Theme.highlightInk
                    font.family: Theme.fontFamily; font.pixelSize: 13
                    elide: Text.ElideRight
                    // Corrections ("thn → then") AND the half-typed last word's
                    // candidate ladder, likeliest first ("rec → receive · received
                    // · record") — the pane and the app-wide dialog read the same
                    // list from the same engine call.
                    text: root.resolvedWords.filter((w) => w.changed === true
                                                        || (w.candidates && w.candidates.length > 0))
                              .map((w) => (w.candidates && w.candidates.length > 0)
                                       ? w.typed + " \u2192 " + w.candidates.slice(0, 3).join(" \u00b7 ")
                                       : w.typed + " \u2192 " + w.resolved).join("   ")
                              + "   (Tab to accept)"
                    }
                }
                delegate: Item {
                    id: hitRow
                    required property var modelData
                    required property int index
                    objectName: "selfTestHitRow_" + index   // (the pane-rows scenario scrolls to it by name)
                    readonly property real labelWidth: Math.min(260, Math.max(160, width * 0.28))
                    // The keyboard/hover cursor's row — same pattern as
                    // citationMatches' matchRow.active, just against hitCursor.
                    readonly property bool active: root.hitCursor === index
                    // LAYOUT PROBE (VGR_LAYOUT_TEST).
                    Component.onCompleted: root.hitRowBuilds++
                    Component.onDestruction: root.hitRowDrops++
                    width: hitsList.width
                    height: 32
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 1; radius: 4
                        // Same treatment as verseRow's own active state: an
                        // accent-tinted ground and a full outline, not a faint
                        // neutral chip — that read as no highlight at all.
                        color: hitRow.active ? Qt.alpha(Theme.accent, 0.24) : "transparent"
                        border.width: hitRow.active ? 1.5 : 0
                        border.color: hitRow.active ? Qt.alpha(Theme.accentLight, 0.95) : "transparent"
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: hitHover.hovered
                            glowColor: Theme.hoverBg
                        }
                    }
                    Text {
                        x: 10; y: 6; width: hitRow.labelWidth
                        text: (root.adapter && root.adapter.contentType === "table"
                               ? root.compactTableResultTitle(hitRow.modelData)
                               : hitRow.modelData.reference)
                              + (Number(hitRow.modelData.verse) > 0
                                 ? "  ·  ¶" + Number(hitRow.modelData.verse) : "")
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 15
                    }
                    // The row's own measurement of its snippet — BOUND, so the
                    // windowing below stays a pure function of its arguments.
                    // (Measuring inside `shown` instead made that binding write
                    // the shared TextMetrics it was reading: a binding loop Qt
                    // re-evaluated for every row, every frame.)
                    TextMetrics {
                        id: hitMetrics
                        font.family: Theme.fontFamily; font.pixelSize: 15
                        text: hitSnippet.collapsed
                    }
                    Text {
                        id: hitSnippet
                        objectName: "selfTestPaneHitSnippet"   // (the pane-highlight scenario reads the rendered row by name)
                        x: hitRow.labelWidth + 20; anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - hitRow.labelWidth - 28
                        color: root.textDim
                        elide: Text.ElideRight
                        textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                        font.family: Theme.fontFamily; font.pixelSize: 15
                        readonly property string collapsed: root.collapseSnippet(hitRow.modelData.snippet)
                        // Centre the short excerpt around the matching words, then
                        // elide at the row edge; the result stays a preview, not a
                        // full paragraph dump.
                        readonly property string shown: root.centeredSnippet(
                            hitSnippet.collapsed, width, hitMetrics.advanceWidth)
                        text: root.canHighlight ? root.highlight(shown) : shown
                    }
                    // Position truth (PositionHoverArea), the codebase
                    // convention — a bare HoverHandler here was the actual
                    // reason the hover-follows-selection never caught: this
                    // build does not reliably deliver per-row hover-enter/exit
                    // (see PositionHoverArea's own header), and matchRow's
                    // identical "hover selects, like the arrows" logic already
                    // relies on this component for exactly that reason.
                    PositionHoverArea {
                        id: hitHover
                        anchors.fill: parent
                        checkAncestors: false
                        clickEnabled: false   // the TapHandler below owns clicks
                        onHoveredChanged: if (hitHover.hovered) root.hitCursor = hitRow.index
                    }
                    TapHandler {
                        onTapped: {
                            root.hitCursor = hitRow.index
                            root.goTo({ bookId: hitRow.modelData.bookId, chapter: hitRow.modelData.chapter,
                                        verseStart: hitRow.modelData.verse, verseEnd: hitRow.modelData.verse,
                                        matchText: searchInput.text })
                        }
                        onDoubleTapped: {
                            root.hitCursor = hitRow.index
                            root.goTo({ bookId: hitRow.modelData.bookId, chapter: hitRow.modelData.chapter,
                                        verseStart: hitRow.modelData.verse, verseEnd: hitRow.modelData.verse,
                                        matchText: searchInput.text })
                            root.searching = false
                            searchInput.text = ""
                        }
                    }
                    // Both the title and the snippet elide on this row — hover
                    // gives the full title and the full (uncentred, untruncated)
                    // snippet. A scrolling results list, not a toolbar sweep, so
                    // each row earns its own delay (see HoverDetail.alwaysDelay).
                    HoverDetail {
                        anchors.fill: parent
                        alwaysDelay: true
                        title: root.adapter && root.adapter.contentType === "table"
                               ? root.compactTableResultTitle(hitRow.modelData)
                               : hitRow.modelData.reference
                                 + (Number(hitRow.modelData.verse) > 0 ? "  ·  ¶" + Number(hitRow.modelData.verse) : "")
                        detail: hitSnippet.collapsed
                    }
                }
                // Nothing found: a FOOTER, so it sits under the empty list instead
                // of being a row that was always there. Say WHY the list is empty
                // (an unnoticed padlock looks like a broken search), and how to get
                // the loose results back.
                footer: Item {
                    width: hitsList.width
                    height: hitsEmpty.visible ? hitsEmpty.implicitHeight + 24 : 0
                    Text {
                        id: hitsEmpty
                        visible: root.searchResults.length === 0
                        x: 12; y: 12
                        width: parent.width - 24
                        wrapMode: Text.WordWrap
                        text: root.exact && searchInput.text.trim() !== ""
                              ? qsTr("No exact match for “%1” — with the padlock on, the words must appear together, exactly as typed. Click the padlock to search loosely.").arg(searchInput.text.trim())
                              : (root.adapter ? root.adapter.searchHint : qsTr("Type words to find, then press Enter."))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 14
                    }
                }
            }

            // THE TABLE's citation matches — IN THE PANE (FreeShow's scripture
            // search: results are drawer content, nothing floats over the search
            // box or the text). One row per sermon matching the typed text; HOVER
            // a row and the right half (peekPanel) shows that sermon's full
            // paragraphs — the preview can't cover anything because it lives in
            // its own half. Click opens the sermon for real.
            //
            // A LISTVIEW, for the reason in the note above: "th" matches 641
            // sermons in a real library, and this list used to build them all (it
            // was capped at 150 for that reason alone — see suggest()). The cap is
            // gone with the build it was bounding, so the list now holds every
            // match and builds only the rows the viewport shows.
            ListView {
                id: matchesList
                objectName: "selfTestPaneMatchList"   // (the pane-rows scenario scrolls it by name)
                visible: !root.searching && root.citationMatches.length > 0
                // FIXED half-split while the matches show (never re-laid-out by the
                // hover): an early version narrowed the list only WHILE the preview
                // was open, so a pointer near a row's right edge oscillated — hover
                // opens the panel, the row shrinks away under the pointer, the panel
                // closes, the row widens back, hover again…
                x: 0; y: 4; width: parent.width * 0.52; height: parent.height - 8
                clip: true
                model: root.citationMatches
                boundsBehavior: Flickable.StopAtBounds
                delegate: Item {
                    id: matchRow
                    required property var modelData
                    required property int index
                    objectName: "selfTestMatchRow_" + index   // (the self-test hovers these by name)
                    readonly property bool active: root.peekIndex === index
                    // LAYOUT PROBE (VGR_LAYOUT_TEST).
                    Component.onCompleted: root.matchRowBuilds++
                    Component.onDestruction: root.matchRowDrops++
                    width: matchesList.width; height: 30
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 1; radius: 4
                        color: matchRow.active ? Theme.chip : "transparent"
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: matchHover.hovered
                            glowColor: Theme.hoverBg
                        }
                        Rectangle { visible: matchRow.active; x: 0; y: 4; width: 2; height: parent.height - 8
                                    color: root.column === 2 ? Theme.accent : Theme.border }
                    }
                    // THE ROW IS THE CITATION LINE, WHOLE (user call: "instead of
                    // making 56-0101 a subscript it should be part of the name
                    // 56-0101 - Why are ... remove the subscript"). The code used to
                    // ride the row's right edge as a small trailing label; it now
                    // reads as the head of the same line the box and the seed speak
                    // ("56-0101 - Why Are People So Tossed About"), so a result and
                    // the citation a user types are one and the same string.
                    Text {
                        x: 10; anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 20
                        text: String(matchRow.modelData.cite !== undefined
                                     ? matchRow.modelData.cite : matchRow.modelData.label)
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 15
                    }
                    // Position truth (PositionHoverArea), the codebase convention —
                    // also what the self-test can drive deterministically.
                    PositionHoverArea {
                        id: matchHover
                        anchors.fill: parent
                        checkAncestors: false
                        onHoveredChanged: {
                            if (hovered) {
                                peekClear.stop()
                                root.hoveredMatch = matchRow.index
                                // HOVER SELECTS, like the arrows: the row the eye is on is
                                // the row the keys continue from (before this the hovered
                                // row lit up while the cursor stayed behind, so the next
                                // Down re-took a row that already looked chosen).
                                root.matchCursor = matchRow.index
                                root.peekSuggestion(matchRow.index)
                            } else {
                                // Keep the current result's preview when the pointer
                                // leaves the row for its paragraph pane or controls.
                                // Entering a different result row will replace it.
                                if (root.hoveredMatch === matchRow.index)
                                    root.hoveredMatch = -2
                                peekClear.stop()
                            }
                        }
                        // SINGLE CLICK selects (its verses show on the right); OPENING it
                        // is the double-click — the same rule the verse rows and the
                        // media cards follow (user call: "when press enter or double
                        // click it should open it up and leave the search mode").
                        onClicked: {
                            root.column = 2
                            root.hoveredMatch = -2   // the click owns the preview now
                            root.matchCursor = matchRow.index
                            root.peekSuggestion(matchRow.index)
                        }
                        onDoubleClicked: {
                            root.column = 2
                            root.hoveredMatch = -2
                            root.matchCursor = matchRow.index
                            root.takeMatch()
                        }
                    }
                }
            }

            // The hovered match's sermon, FULL TEXT, in the right half — a real pane
            // (not an overlay), so it covers nothing and needs no visibility swap on
            // the left list. Appears only while a match is hovered; the open sermon's
            // view is untouched underneath and comes straight back.
            Item {
                id: peekPanel
                visible: root.peekIndex >= 0 && root.peekVerses.length > 0
                x: parent.width * 0.52 + 8; y: 4
                width: parent.width * 0.48 - 16
                height: parent.height - 8
                clip: true
                // THE SELECTOR'S SECOND SEARCH STOP: while the arrows are on these
                // verses the panel wears the accent edge, so the pane the keys are
                // walking is something the eye can confirm (the same accent a row's own
                // selector bar uses). The template column on the far right is NOT a
                // stop and carries no such edge (user call).
                Rectangle { anchors.fill: parent; radius: 6; color: Theme.windowBg
                            border.color: root.column === 3 ? Theme.accent : Theme.border
                            border.width: root.column === 3 ? 2 : 1 }
                // Moving from the result row into the preview (including its
                // scrollbar or blank space) pins that result while the user scrolls.
                PositionHoverArea {
                    anchors.fill: parent
                    checkAncestors: false
                    showCursor: false
                    clickEnabled: false
                    onEntered: {
                        root.hoveredMatch = -2
                        root.column = 3
                        peekClear.stop()
                    }
                }
                Text {
                    id: peekHeader
                    x: 12; y: 8
                    width: parent.width - 24
                    text: root.peekIndex >= 0 && root.citationMatches[root.peekIndex] ? (root.citationMatches[root.peekIndex].cite || root.citationMatches[root.peekIndex].label) : ""
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
                // A LISTVIEW, for the same reason as the two lists above: the peek is
                // a whole sermon of paragraphs (500+ in a real library) and it was
                // built in full on the HOVER that opened it — a two-character query
                // plus one hover was a thousand rows built at once, which is exactly
                // when the pointer is supposed to feel instant. Rows here are as tall
                // as their wrapped text, so the list measures them as they arrive
                // rather than assuming a height (the paragraph list above is fixed and
                // does not have to).
                ListView {
                    id: peekFlick
                    objectName: "selfTestPanePeekList"   // (the pane-rows scenario scrolls it by name)
                    anchors.fill: parent
                    anchors.topMargin: 26
                    anchors.rightMargin: 10
                    clip: true
                    model: root.peekVerses
                    boundsBehavior: Flickable.StopAtBounds
                    // The old Column sat at x: 4 with a width of parent.width - 8;
                    // the content margins reproduce that exactly, so the peek's text
                    // wraps at the same place it always did.
                    leftMargin: 4; rightMargin: 4
                    delegate: Item {
                        required property var modelData
                        required property int index
                        objectName: "selfTestPeekRow_" + index
                        readonly property bool active: root.column === 3 && root.peekRow === index
                        width: peekFlick.width - 8; height: peekText.implicitHeight + 10
                        // LAYOUT PROBE (VGR_LAYOUT_TEST).
                        Component.onCompleted: root.peekRowBuilds++
                        Component.onDestruction: root.peekRowDrops++
                        // The paragraph the arrows are reading (see stepPeek).
                        Rectangle {
                            anchors.fill: parent; radius: 4
                            color: parent.active ? Theme.chip : "transparent"
                        }
                        Text {
                            id: peekNum
                            x: 6; y: 5
                            width: 26; horizontalAlignment: Text.AlignRight
                            text: modelData.number
                            color: Theme.accent
                            font.family: Theme.fontFamily; font.pixelSize: 14; font.bold: true
                        }
                        Text {
                            id: peekText
                            x: 40; y: 5; width: parent.width - 48
                            text: modelData.text
                            color: Theme.textPrimary
                            wrapMode: Text.Wrap
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        PositionHoverArea {
                            anchors.fill: parent
                            checkAncestors: false
                            showCursor: false
                            onClicked: {
                                root.column = 3
                                root.peekRow = index
                                root.hoveredMatch = -2
                            }
                            onDoubleClicked: {
                                root.column = 3
                                root.peekRow = index
                                root.hoveredMatch = -2
                                root.takeMatch()
                            }
                            onEntered: {
                                root.column = 3
                                root.peekRow = index
                            }
                        }
                    }
                    // The old Column's 20px of room under the last paragraph.
                    footer: Item { width: peekFlick.width - 8; height: 20 }
                    // A long sermon's peek overflowed with no way to drag it.
                    AppScrollBar {
                        flickable: peekFlick
                        anchors.top: parent.top; anchors.bottom: parent.bottom
                        anchors.right: parent.right; anchors.rightMargin: 2
                    }
                }
            }
        }

        // Nothing installed / nothing loaded yet. Only over an EMPTY verse column: a
        // reload keeps its rows on screen (see syncSources), and a hint drawn over a
        // list that is still there would be the noise this state exists to remove.
        // A spinner above the words while the library is really being read — the same
        // shape MediaLibraryPane uses for a folder that is still coming in.
        Column {
            visible: root.sources.length === 0 && root.chapterVerses.length === 0
            anchors.centerIn: parent
            spacing: centreSpinner.visible ? 10 : 0

            BusySpinner {
                id: centreSpinner
                visible: root.showReloading
                anchors.horizontalCenter: parent.horizontalCenter
                width: 26; height: 26
                thickness: 2.5
                color: Theme.textMuted
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.libraryHint()
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: 15
            }
        }

        // The verses (or search results / matches) column's scrollbar — the
        // chapter can be 150 sermons or 176 verses; wheel-only was the only
        // way to move it. Each MODE now scrolls its own list (see resultsArea),
        // so the bar follows whichever one is showing.
        AppScrollBar {
            flickable: root.searching ? hitsList
                    : (root.citationMatches.length > 0 ? matchesList : versesFlick)
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 2; anchors.bottomMargin: 44   // clear the floating chip/pill
            anchors.right: parent.right; anchors.rightMargin: 4
        }

        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

        // ---- the floating reference chip (FreeShow's left FloatingInputs) ----
        Rectangle {
            visible: root.currentSource !== null && root.referenceText !== ""
            x: 12; y: parent.height - height - 12
            height: 30; width: Math.min(refRow.implicitWidth + 24, Math.max(60, parent.width - pill.width - 36)); radius: 15
            color: Qt.alpha(Theme.windowBg, 0.9)
            border.color: Theme.border
            Row {
                id: refRow
                anchors.centerIn: parent
                spacing: 6
                Text {
                    visible: versesCol.width > 560   // a narrow pane keeps just the reference
                    text: (root.currentSource ? root.currentSource.name : "") + ":"
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily; font.pixelSize: 14
                }
                Text {
                    text: root.referenceText
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 14
                }
            }
        }

        // ---- the floating controls (FreeShow's right FloatingInputs): previous / next verse, search ----
        Rectangle {
            id: pill
            x: parent.width - width - 12; y: parent.height - height - 12
            height: 34; width: pillRow.width + (root.searching ? 8 : 12) + 8; radius: 17
            color: Qt.alpha(Theme.windowBg, 0.9)
            border.color: Theme.border

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 2

                Item {   // search field, while searching
                    visible: root.searching
                    width: 220; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: Theme.surface; border.color: Theme.border }
                    TextInput {
                        id: searchInput
                        // Named per content type, like the locks beside it: BOTH
                        // reference panes own one of these, and the pane-highlight
                        // scenario types into the box on the tab it means (the
                        // search itself runs from the box's own debounce, so the
                        // scenario exercises the real path — typing — instead of
                        // calling runSearch() behind the field's back).
                        objectName: "selfTestSearchBox-" + (root.adapter && root.adapter.contentType
                                                            ? root.adapter.contentType : "")
                        // Wider right margin: the chapter lock and the padlock sit
                        // at the field's far end.
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 54
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily; font.pixelSize: 14
                        clip: true
                        // LIVE filtering (user call: no Enter press first) — every edit
                        // re-runs the search through a short debounce (the engine's
                        // indexed search is fast enough); Enter still commits instantly.
                        onTextChanged: {
                            pillDebounce.restart()
                        }
                        onAccepted: {
                            pillDebounce.stop()
                            // Enter on a row the keyboard or the pointer already
                            // walked to opens IT — the same "that row IS the
                            // answer" rule stepMatch's own Enter handling
                            // follows for citationMatches.
                            if (root.hitCursor >= 0 && root.hitCursor < root.searchResults.length) {
                                const row = root.searchResults[root.hitCursor]
                                root.goTo({ bookId: row.bookId, chapter: row.chapter,
                                            verseStart: row.verse, verseEnd: row.verse, matchText: text })
                                root.searching = false
                                searchInput.text = ""
                            } else {
                                root.runSearch(text)
                            }
                        }
                        Keys.onDownPressed: root.stepHit(1)
                        Keys.onUpPressed: root.stepHit(-1)
                        Keys.onTabPressed: {
                            // Accept the engine's corrections: "thn freind" + Tab
                            // -> "then friend", then re-run with the fixed words.
                            if (!root.resolvedWords.some((w) => w.changed === true)) return
                            let fixed = text
                            for (const w of root.resolvedWords) {
                                if (w.changed !== true) continue   // spelled right: leave it
                                fixed = fixed.replace(new RegExp("\\b" + w.typed + "\\b", "i"),
                                                      String(w.resolved).split(/\s+/).join(" "))
                            }
                            text = fixed
                            pillDebounce.stop()
                            root.runSearch(fixed)
                        }
                        Keys.onEscapePressed: { root.searching = false }
                    }
                    Timer {
                        id: pillDebounce
                        // LIVE filtering (user call: no Enter press first). The engine
                        // scan behind this is a real search over the whole library —
                        // hundreds of milliseconds, seconds when several words are
                        // typed — so the debounce waits for a typing pause instead of
                        // launching one scan per keystroke: at 80 ms, typing a
                        // four-word query queued four full scans, and the results the
                        // user watched arrive belonged to word one. Enter still commits
                        // instantly (onAccepted stops the timer).
                        interval: 150
                        onTriggered: root.runSearch(searchInput.text)
                    }
                    Text {
                        visible: searchInput.text === ""
                        anchors.verticalCenter: parent.verticalCenter; x: 12
                        // Locked, the hint names the scope — the field itself is
                        // where the user finds out what the search now covers.
                        text: (root.scopeLock && root.scopeName !== "")
                              ? qsTr("Search in %1").arg(root.scopeName)
                              : (root.adapter ? root.adapter.searchPlaceholder : qsTr("Search"))
                        color: root.scopeLock ? Theme.accent : Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 14
                    }
                    // The CHAPTER LOCK (Scripture and The Table only — see
                    // supportsChapterLock): a book pill beside the padlock. Open
                    // (dim) = the tab's normal search over everything; lit = only
                    // the passage on screen is searched. Clicking re-runs what is
                    // already typed.
                    Item {
                        id: pillScope
                        // Named per tab: BOTH reference tabs show this pill, and a
                        // scenario (or a test) that names it must reach the one on
                        // the tab it means.
                        objectName: "selfTestChapterLock"
                                    + (root.adapter && root.adapter.contentType
                                       ? "-" + root.adapter.contentType : "")
                        visible: root.supportsChapterLock
                        anchors.right: parent.right; anchors.rightMargin: 30
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20
                        Rectangle {
                            anchors.fill: parent; radius: 10
                            color: root.scopeLock ? "#1d2a2f" : "transparent"
                            HoverGlow {
                                anchors.fill: parent
                                radius: parent.radius
                                hovered: scopeHover.hovered
                                glowColor: Theme.hoverBg
                            }
                        }
                        IconGlyph {
                            anchors.centerIn: parent
                            name: root.scopeLock ? "book" : "bookOpen"
                            color: root.scopeLock ? Theme.accent : Theme.textMuted
                            width: 12; height: 12
                            fit: true
                        }
                        PositionHoverArea {
                            id: scopeHover
                            anchors.fill: parent
                            checkAncestors: false
                            onClicked: root.toggleScopeLock()
                        }
                        HoverDetail {
                            anchors.fill: parent
                            title: qsTr("Chapter lock")
                            detail: root.scopeLock
                                    ? qsTr("Searching only the passage on screen")
                                    : qsTr("Searching everything — click to search just this passage")
                        }
                    }
                    // The exact-words padlock, at the extreme end of the field (user
                    // call). Open (dim) = the search's normal fuzzy matching; closed
                    // (accent) = every typed word must appear verbatim, as a whole
                    // word. Clicking re-runs what is already typed.
                    Item {
                        id: pillLock
                        objectName: "selfTestExactLock"
                        anchors.right: parent.right; anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20
                        Rectangle {
                            anchors.fill: parent; radius: 10
                            color: lockHover.hovered ? Theme.hoverBg : (root.exact ? Theme.accentSoft : "transparent")
                        }
                        IconGlyph {
                            anchors.centerIn: parent
                            name: root.exact ? "locked" : "unlocked"
                            color: root.exact ? Theme.accent : Theme.textMuted
                            width: 12; height: 12
                            fit: true
                        }
                        PositionHoverArea {
                            id: lockHover
                            anchors.fill: parent
                            checkAncestors: false
                            onClicked: root.toggleExact()
                        }
                        HoverDetail {
                            anchors.fill: parent
                            title: qsTr("Exact words")
                            detail: root.exact
                                    ? qsTr("Every typed word must match exactly")
                                    : qsTr("Fuzzy matching — click to require exact whole words")
                        }
                    }
                }

                // ONE step pair, FreeShow's own toolbar shape: ‹ › step the
                // picked passage (and re-play it when this tab is on air —
                // their arrows carry the isActiveInOutput refresh). The old
                // SECOND pair (a verse-step ‹› AND a passage-step ‹› side by
                // side) duplicated the same job and crowded the pill.
                Repeater {
                    model: root.searching ? [] : [
                        { glyph: "chevronLeft", dir: -1, tip: qsTr("Previous") },
                        { glyph: "chevronRight", dir: 1, tip: qsTr("Next") }
                    ]
                    delegate: Item {
                        id: stepBtn
                        required property var modelData
                        // Named per tab, like the search padlocks beside it: BOTH
                        // reference panes own this pair and a scenario (or a
                        // test) must reach the one on the tab it means.
                        objectName: "selfTestStep" + (stepBtn.modelData.dir > 0 ? "Next" : "Prev")
                                    + (root.adapter && root.adapter.contentType
                                       ? "-" + root.adapter.contentType : "")
                        width: 28; height: 26
                        Rectangle { anchors.fill: parent; radius: 13; color: stepHover.hovered ? Theme.hoverBg : "transparent" }
                        IconGlyph {
                            anchors.centerIn: parent
                            name: stepBtn.modelData.glyph
                            color: Theme.textPrimary
                            width: 13; height: 13
                            fit: true
                        }
                        PositionHoverArea {
                            id: stepHover
                            anchors.fill: parent
                            checkAncestors: false
                            // THE MONITOR PREVIEW FOLLOWS THESE ARROWS (FreeShow's own toolbar;
                            // user call: "in scripture <> is not update monitor preview fix it").
                            // They step the PICK — its slides first, then the neighbouring
                            // passage — and stage what they land on (stepAndPlay). The KEYBOARD
                            // Left/Right are the selector's (see stepColumn); this pair is the
                            // pick's, exactly as the wall's ‹ › are.
                            onClicked: root.stepAndPlay(stepBtn.modelData.dir)
                        }
                        HoverDetail {
                            anchors.fill: parent
                            title: stepBtn.modelData.tip
                            detail: qsTr("Step and stage the neighbouring passage")
                        }
                    }
                }

                Rectangle { visible: !root.searching; width: 1; height: 16; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

                Item {
                    id: playPickedButton
                    visible: !root.searching
                    width: 28; height: 26
                    // lineStepOwnerRef over the live root.referenceText — same
                    // drift reasoning as refreshWallStepper's own comment —
                    // so this button doesn't flicker back to "Play" mid-step
                    // just because the selector tracked into a new paragraph.
                    readonly property bool isOurs: root.adapter && root.adapter.liveIsOurs
                                                   ? root.adapter.liveIsOurs(root.lineStepOwnerRef !== "" ? root.lineStepOwnerRef : root.referenceText)
                                                   : false
                    Rectangle { anchors.fill: parent; radius: 13; color: playPickedHover.hovered || parent.isOurs ? Theme.hoverBg : "transparent" }
                    // FreeShow's scripture play button: PLAY (outlined) when this
                    // passage is not on air; when it IS, the green filled STOP
                    // square — clicking takes it off air (their "stop" state),
                    // NOT a refresh icon (which IconGlyph doesn't even have: the
                    // unknown name fell through to the filled-dot fallback and
                    // rendered as a green blob).
                    IconGlyph {
                        anchors.centerIn: parent
                        name: parent.isOurs ? "stop" : "play"
                        color: parent.isOurs ? Theme.successLight : Theme.textPrimary
                        width: 13; height: 13
                        fit: true
                    }
                    PositionHoverArea {
                        id: playPickedHover
                        anchors.fill: parent
                        checkAncestors: false
                        onClicked: playPickedButton.isOurs ? LiveOutputService.stop() : root.playPicked()
                    }
                    HoverDetail {
                        anchors.fill: parent
                        title: playPickedButton.isOurs ? qsTr("Stop") : qsTr("Play")
                        detail: playPickedButton.isOurs
                                ? qsTr("Take this passage off air")
                                : qsTr("Go live with the picked passage")
                    }
                }

                // The highlight toggle for the picked passage (the drawer's other half):
                // gold star while verse 1 of the pick is marked — the star marks the
                // FIRST picked verse, per-verse stars live on the rows themselves.
                Item {
                    id: starToggle
                    visible: !root.searching && root.hasUserData
                    width: 28; height: 26
                    readonly property bool marked: root.hasUserData && root.selected.length > 0
                                                   && root.markFor(root.selected[0]).highlight === true
                    Rectangle { anchors.fill: parent; radius: 13; color: starHover.hovered || parent.marked ? Theme.hoverBg : "transparent" }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "star"
                        color: parent.marked ? Theme.markedInk : Theme.textPrimary
                        width: 13; height: 13
                        fit: true
                    }
                    PositionHoverArea {
                        id: starHover
                        anchors.fill: parent
                        checkAncestors: false
                        onClicked: {
                            if (!root.book || root.selected.length === 0)
                                return
                            root.toggleHighlight(root.selected[0])
                        }
                    }
                    HoverDetail {
                        anchors.fill: parent
                        title: qsTr("Highlight")
                        detail: starToggle.marked
                                ? qsTr("Marked — click to remove the highlight")
                                : qsTr("Mark the picked passage")
                    }
                }

                Rectangle { visible: !root.searching; width: 1; height: 16; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

                Item {
                    width: 28; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: searchHover.hovered || root.searching ? Theme.hoverBg : "transparent" }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "search"
                        color: Theme.textPrimary
                        width: 13; height: 13
                        fit: true
                    }
                    PositionHoverArea {
                        id: searchHover
                        anchors.fill: parent
                        checkAncestors: false
                        onClicked: {
                            root.searching = !root.searching
                            if (root.searching) {
                                root.searchResults = []
                                searchInput.text = ""
                                searchInput.forceActiveFocus()
                            }
                        }
                    }
                    HoverDetail {
                        anchors.fill: parent
                        title: qsTr("Search")
                        detail: root.adapter ? root.adapter.searchPlaceholder : qsTr("Search")
                    }
                }
            }
        }

        // ---- Notes & highlights drawer (docs/specs/24 §User Data) — the pane's
        // own written-work ledger: every note in the source, most recently
        // touched first; a click jumps the picker straight to its passage.
        Rectangle {
            id: notesDrawer
            visible: root.notesOpen && root.hasUserData
            x: 10; y: 10
            width: Math.min(340, parent.width - 20); height: parent.height - 20
            radius: 8
            color: Theme.panelBg
            border.color: Theme.border

            Item {
                id: drawerHead
                x: 12; y: 10; width: parent.width - 24; height: 20
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: (root.adapter ? root.adapter.tabLabel : qsTr("Library")) + qsTr(" — notes")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 14; font.bold: true
                }
                Item {
                    id: drawerClose
                    anchors.right: parent.right; width: 20; height: 20
                    Rectangle { anchors.fill: parent; radius: 10; color: closeHover.hovered ? Theme.hoverBg : "transparent" }
                    IconGlyph { anchors.centerIn: parent; name: "ban"; color: Theme.textSecondary; width: 10; height: 10 }
                    HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.notesOpen = false }
                }
            }

            Flickable {
                id: drawerFlick
                x: 12; y: 38; width: parent.width - 24; height: parent.height - 50
                clip: true
                contentHeight: drawerList.height
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: drawerList
                    width: parent.width
                    spacing: 4

                    Repeater {
                        model: root.notesRows
                        delegate: Item {
                            id: noteRowItem
                            required property var modelData
                            width: drawerList.width; height: noteText.implicitHeight + 26
                            Rectangle {
                                anchors.fill: parent; radius: 6
                                color: "transparent"
                                // Same shared hover layer as every other
                                // row: this is the notes list, and a row here
                                // must light like a row in The Table or Shows.
                                HoverGlow {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    hovered: noteRowHover.hovered
                                    glowColor: Theme.hoverBg
                                }
                            }
                            Text {
                                id: noteText
                                x: 8; y: 8; width: parent.width - 16
                                text: noteRowItem.modelData.text
                                color: Theme.textPrimary
                                wrapMode: Text.Wrap
                                font.family: Theme.fontFamily; font.pixelSize: 13
                            }
                            Text {
                                x: 8; anchors.bottom: parent.bottom; anchors.bottomMargin: 6
                                text: noteRowItem.modelData.reference
                                color: Theme.markedInk
                                font.family: Theme.fontFamily; font.pixelSize: 11
                            }
                            HoverHandler { id: noteRowHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.jumpToNote(noteRowItem.modelData) }
                        }
                    }

                    Text {
                        visible: root.notesRows.length === 0
                        width: parent.width
                        text: qsTr("No notes yet.\n\nHover a verse and press the pencil to write one; the star in the pill marks a passage.")
                        color: Theme.textMuted
                        wrapMode: Text.Wrap
                        font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                }
            }
            AppScrollBar {
                flickable: drawerFlick
                anchors.top: drawerFlick.top; anchors.bottom: drawerFlick.bottom
                anchors.right: parent.right; anchors.rightMargin: 2
            }
        }
    }

    // ---- Preview + template + options -------------------------------------------------------------------------------------------
    // FreeShow's own ScriptureInfo.svelte: ONE scrolling column (`.scroll { overflow-y: auto }`) holding the preview then the
    // settings, and the preview itself never shrinks to make room (`.zoomed { height: initial !important }`) - it keeps its
    // natural width-driven size, and if the settings below don't fit, you scroll the whole column instead of the preview
    // getting squeezed. Rebuilt to match: previewTile's height comes from ITS width alone, and everything - preview, template
    // row, options, the Convert-to-show button - lives in one Flickable with a real scrollbar, not a shrink formula.
    Rectangle {
        id: previewCol
        anchors.right: parent.right
        visible: !root.compactTableSearch
        width: root.compactTableSearch ? 0 : root.previewColWidth; height: parent.height
        color: Theme.windowBg

        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

        Flickable {
            id: previewFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 1
            // Reserving the strip only WHILE the bar shows was a polish loop:
            // the bar's visibility changed this Flickable's width, the column
            // inside re-laid out (its preview tile is sized from width), its
            // contentHeight moved, and the bar's own visible binding —
            // contentHeight > height — flipped straight back. The strip is part
            // of the layout, so it is always reserved (see the identical fix in
            // VGRPresenterMainScreen's shows sidebar).
            anchors.rightMargin: 9
            contentWidth: width
            contentHeight: previewFlickContent.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: previewFlickContent
                width: previewFlick.width

                // Shared by "Use default template" (in the template section) and "Convert to show" (in the always-present
                // footer below) - declared once here so both scopes can use it without redeclaring the same local type.
                component PanelButton: Rectangle {
                    id: btn
                    property string text: ""
                    property string info: ""
                    signal clicked()
                    width: parent.width; height: 44; radius: 6
                    color: btnHover.hovered ? Theme.surface : Theme.windowBg; border.color: Theme.border
                    Row {
                        anchors.centerIn: parent
                        spacing: 8
                        Text { text: btn.text; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 15 }
                        Text { visible: btn.info !== ""; text: btn.info; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 14 }
                    }
                    HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: btn.clicked() }
                }

                // ---- the slide the engine builds for the picked verses: fixed size, driven by width alone ----
                Item {
                    width: parent.width
                    height: previewTile.height + 28   // 20 above (breathing room under the tab bar's search box) + 8 below

                    Rectangle {
                        id: previewTile
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 20
                        width: parent.width - 32
                        // the 16:9-ish design ratio, purely from width - never clamped by leftover height
                        height: StageGrid.heightForWidth(width)
                        color: "#000000"
                        clip: true
                        // NOT A SELECTOR STOP (user call: "just 3 panes"): the preview
                        // follows the pick, and the arrows never park on it — no
                        // active edge to paint.
                        DesignPreview {
                            anchors.fill: parent
                            // REVERTED: this box used to always show
                            // root.preview/currentPreviewSlide — THE PANE'S
                            // OWN chosen template — and briefly tried
                            // preferring root.liveFlowSlide (LiveOutputService.
                            // stagedSlide, composed against the ACTIVE
                            // OUTPUT'S STYLE template, not this pane's own)
                            // for its scroll animation. That made this box
                            // permanently lock onto the output style's
                            // template the moment anything was staged, with
                            // no way back even after picking a different
                            // template here — the output style's template
                            // also carries bound fields this one never
                            // needed, which is where the literal "{text}"
                            // placeholders came from (user report: "the
                            // moment I click on the list... it changes
                            // [the template] and refuses to change it back").
                            // Back to always the pane's own template; the
                            // live-scroll animation for THIS box is dropped
                            // rather than left half-working.
                            blocks: root.currentPreviewSlide ? root.currentPreviewSlide.blocks : root.preview.blocks
                            // THE DESIGN'S OWN ANSWER, not an opaque plate of
                            // this pane's: an unpainted background keeps
                            // painting nothing, and the shared transparency
                            // convention (DesignPreview's checkerboard) shows
                            // there — the canvas's rule, not a black slab
                            // standing in for a keyable frame.
                            background: root.preview.background ?? "transparent"
                        }
                        Text {
                            visible: root.selected.length === 0
                            anchors.centerIn: parent
                            text: qsTr("Pick a verse")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                    }
                }

                // ---- the template controls (FreeShow's "Template" row, the note about old templates, the two buttons) ----
                Column {
                    visible: root.supportsTemplates && !root.optionsOpen
                    x: 10; width: parent.width - 20
                    spacing: 10

                    Rectangle {
                        width: parent.width; height: 56; radius: 6
                        color: Theme.windowBg; border.color: Theme.border
                        clip: true

                        Column {
                            x: 12; anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text { text: qsTr("Template"); color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: 13 }
                            Text {
                                width: 150
                                // Same three states the Show screen's template row uses: nothing chosen,
                                // a real name, or an id whose design is gone. Never a silently blank line.
                                // The name is the template that will actually render (the output style's own
                                // family template when it names one — see templateRowText).
                                text: root.templateRowText
                                color: root.effectiveTemplateId === "" ? Theme.textMuted : Theme.textPrimary
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.DemiBold
                            }
                        }

                        Row {
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            spacing: 0

                            // back to the default (only when another one is chosen)
                            Item {
                                visible: !root.usingDefaultTemplate
                                width: 36; height: 56
                                IconGlyph { anchors.centerIn: parent; name: "close"; color: clearHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 10; height: 10 }
                                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.adapter.setTemplate("") }
                            }
                            // choose another scripture template
                            Item {
                                id: pickBtn
                                width: 36; height: 56
                                IconGlyph { anchors.centerIn: parent; name: "layoutTemplate"; color: pickHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 14; height: 14 }
                                HoverHandler { id: pickHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.openTemplateMenu() }
                            }
                            // edit it on the Edit screen
                            Rectangle {
                                width: 46; height: 56; color: editHover.hovered ? Theme.hoverBg : Theme.windowBg
                                IconGlyph { anchors.centerIn: parent; name: "penTool"; color: Theme.textPrimary; width: 14; height: 14 }
                            HoverHandler { id: editHover; cursorShape: Qt.PointingHandCursor }
                            // Edits the template the OUTPUT is rendering this tab's content with —
                            // the style's family template when it has one, else this tab's own pick.
                            TapHandler { onTapped: root.templateEditRequested(root.effectiveTemplateId) }
                            }
                        }
                    }

                    Text {
                        visible: root.selected.length > 0 && !root.preview.hasValues
                        width: parent.width
                        text: qsTr("You are using a template with no values!")
                        color: Theme.textSecondary; opacity: 0.85
                        wrapMode: Text.Wrap
                        font.family: Theme.fontFamily; font.pixelSize: 14
                    }

                    PanelButton {
                        visible: root.showUseDefaultTemplateButton && !root.usingDefaultTemplate && root.selected.length > 0 && !root.preview.hasValues
                        text: qsTr("Use default template")
                        onClicked: root.adapter.setTemplate("")
                    }
                }

                // ---- the options (the round button turns this on) ----
                Column {
                    id: optionsBody
                    visible: root.supportsTemplates && root.optionsOpen
                    x: 10; width: parent.width - 20

                Text {
                    x: 16; height: 26; verticalAlignment: Text.AlignVCenter
                    text: qsTr("SLIDE OPTIONS")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 1.2
                }
                Rectangle {
                    x: 10; width: parent.width - 20; height: optionsColumn.height + 8
                    radius: 8; color: Theme.windowBg; border.color: Theme.border

                    Column {
                        id: optionsColumn
                        x: 14; y: 4; width: parent.width - 28
                        spacing: 0

                        component OptionToggle: Item {
                            id: opt
                            property string settingKey: ""
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[opt.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: opt.def ? opt.def.label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                            }
                            SettingsToggle {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                checked: root.options[opt.settingKey] === true
                                onToggled: SettingsService.setValue(opt.settingKey, !(root.options[opt.settingKey] === true))
                            }
                        }
                        // A number with - / + buttons: `step` per click, kept inside the engine's own range.
                        component OptionNumber: Item {
                            id: num
                            property string settingKey: ""
                            property int step: 1
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[num.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
                            function nudge(delta) {
                                const next = Math.max(num.def.min, Math.min(num.def.max, Number(root.options[num.settingKey]) + delta))
                                if (next !== root.options[num.settingKey])
                                    SettingsService.setValue(num.settingKey, next)
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: num.def ? num.def.label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                            }
                            Row {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Rectangle {
                                    width: 24; height: 24; radius: 6; color: minusHover.hovered ? Theme.hoverBg : Theme.surface; border.color: Theme.border
                                    Text { anchors.centerIn: parent; text: "−"; color: Theme.textPrimary; font.pixelSize: 15 }
                                    HoverHandler { id: minusHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: num.nudge(-num.step) }
                                }
                                Text {
                                    width: 40; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter
                                    text: root.options[num.settingKey] ?? ""
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                                }
                                Rectangle {
                                    width: 24; height: 24; radius: 6; color: plusHover.hovered ? Theme.hoverBg : Theme.surface; border.color: Theme.border
                                    Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 15 }
                                    HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: num.nudge(num.step) }
                                }
                            }
                        }                        // A setting with a few known values, as small pills on the
                        // right — the same pick-one language the rest of Settings
                        // uses, sized for this card. Everything it shows and writes
                        // comes from the engine's own definition.
                        component OptionChoice: Item {
                            id: pick
                            property string settingKey: ""
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[pick.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - pickRow.width - 8
                                elide: Text.ElideRight
                                text: pick.def ? pick.def.label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                            }
                            Row {
                                id: pickRow
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                spacing: 4
                                Repeater {
                                    model: pick.def ? pick.def.choices : []
                                    delegate: Rectangle {
                                        id: pillChoice
                                        required property var modelData
                                        readonly property bool on:
                                            String(root.options[pick.settingKey]) === String(modelData.value)
                                        width: pillLabel.implicitWidth + 14; height: 24; radius: 6
                                        color: pillChoice.on ? Theme.accent
                                               : (pillHover.hovered ? Theme.hoverBg : Theme.surface)
                                        border.width: 1
                                        border.color: pillChoice.on ? Theme.accentLight : Theme.border
                                        Text {
                                            id: pillLabel
                                            anchors.centerIn: parent
                                            text: pillChoice.modelData.label
                                            color: pillChoice.on ? Theme.controlInk : Theme.textPrimary
                                            font.family: Theme.fontFamily; font.pixelSize: Theme.textSub
                                        }
                                        HoverHandler { id: pillHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler {
                                            onTapped: SettingsService.setValue(pick.settingKey, pillChoice.modelData.value)
                                        }
                                    }
                                }
                            }
                        }

                        // No "... on individual lines", "Smart split" or "Tolerance"
                        // row: the first two were layout toggles (the fill does what
                        // their shipped defaults did), and Tolerance was a second,
                        // invisible rule on top of the words-per-slide number — where
                        // a long item is cut now follows from that number alone (see
                        // AppSettings::AddFillOptions).
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".verseNumbers" }
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".splitLongVerses" }
                        Column {
                            visible: root.options[root.adapter.optionsPrefix + ".splitLongVerses"] === true
                            width: parent.width
                            // NO "Number the parts (1a, 1b)" row (user call: "you can
                            // remove this") — see AppSettings::AddFillOptions: the
                            // fill's shipped behaviour is what happens now.
                            // The slide's budget, in WORDS (was "Size", in characters).
                            // It is the whole rule: a longer item is divided over the
                            // next slide at exactly this many words.
                            OptionNumber { settingKey: root.adapter.optionsPrefix + ".wordsPerSlide"; step: 5 }
                        }
                        // ---- THE READING STEP (Alt+Down / Alt+Up) ----
                        // The SAME two rows on every tab: this is how the words
                        // travel when the operator steps a line, not a property of
                        // the content, so Scripture, The Table and a show share one
                        // setting (see AppSettings' "reading." keys). They sit at
                        // the bottom of this card because they are the step's pace,
                        // next to the options that decide how much text a slide
                        // carries.
                        OptionNumber { settingKey: "reading.stepMs"; step: 10 }
                        OptionChoice { settingKey: "reading.stepEasing" }
                        }
                    }
            }

            // ---- Convert to show, with the round options button beside it - always present, part of the scroll flow ----
            Item {
                x: 10; width: parent.width - 20; height: 48 + 12   // 12 bottom breathing room, matching the old bottom margin

                PanelButton {
                    visible: !root.optionsOpen
                    y: 0
                    width: parent.width - 58
                    text: qsTr("Convert to show")
                    info: root.preview.slideCount > 1 ? qsTr("%1 slides").arg(root.preview.slideCount) : ""
                    onClicked: root.convertToShow()
                }

                // The round button that swaps the panel between the template and the options (FreeShow's tune button).
                // Scripture-only chrome — The Table hides it (no scripture options apply).
                Item {
                    id: optionsButton
                    visible: root.supportsTemplates
                    anchors.right: parent.right; y: 0
                    width: 48; height: 48
                    // a soft shadow: two faint discs under the button
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: width; radius: width / 2; color: "#26000000" }
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: width; radius: width / 2; color: "#33000000" }
                    Rectangle {
                        anchors.fill: parent; radius: width / 2
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: root.optionsOpen ? (optionsHover.hovered ? Theme.accentLight : Theme.accent) : (optionsHover.hovered ? Theme.hoverBg : Theme.surface) }
                            GradientStop { position: 1.0; color: root.optionsOpen ? Theme.accent : Theme.surface }
                        }
                        border.width: 1
                        border.color: root.optionsOpen ? Theme.dangerLight : Theme.borderSubtle
                        IconGlyph { anchors.centerIn: parent; name: "sliders"; color: Theme.controlInk; fit: true; width: 22; height: 22 }
                    }
                    HoverHandler { id: optionsHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.optionsOpen = !root.optionsOpen }
                }
            }
        }
        }

        AppScrollBar {
            id: previewScrollBar
            flickable: previewFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right
            anchors.topMargin: 4; anchors.bottomMargin: 4; anchors.rightMargin: 3
        }
    }

    // The preview column is anchored to the right, so its OWN left edge - not its width - is what the user drags; the handle sits
    // outside it (a sibling, in root's coordinates) so it can report that edge's absolute position rather than a width.
    SplitHandle {
        value: root.width - previewCol.width; minValue: root.width - 560; maxValue: root.width - 260
        onDragged: (v) => root.previewColWidth = root.width - v
        onResetRequested: root.previewColWidth = Math.max(320, Math.min(420, root.width * 0.3))
    }

    // ---- the template chooser ------------------------------------------------------------------------------------------------------
    // The same "Choose template" popup Settings uses (TemplatePickerModal), not a dropdown: every design in the engine's whole
    // template catalog (TemplateLibraryService.designs() with no filter - not just this tab's own content type), mapped to the
    // picker's `{key, name}` shape, category id and name included so its own category filter (defaulting to "All") can group them.
    // A design of a different content type still applies fine (SlideBuilder's own templateId lookup only checks the id exists, not
    // what it was made for) - its bound fields just won't match this tab's placeholders as neatly.
    function openTemplateMenu() {
        const categoryNames = {}
        for (const c of TemplateLibraryService.categories) categoryNames[c.id] = c.name
        templatePicker.templates = TemplateLibraryService.designs().map((t) => ({
            key: t.id, name: t.name, color: t.color,
            category: t.category, categoryName: t.category ? (categoryNames[t.category] ?? t.category) : qsTr("Unlabeled")
        }))
        templatePicker.selectedKey = root.templateId
        templatePicker.open = true
    }
    TemplatePickerModal {
        id: templatePicker
        z: 30000
        contentType: root.sourceId
        // "All" now that the list is the whole catalog, not just this tab's own content type - "Collections" (this tab's sidebar
        // label) would misname it once designs from every category are in the list.
        contentTypeLabel: qsTr("All")
        onApplied: (tpl) => {
            templatePicker.open = false
            root.adapter.setTemplate(tpl.key === root.adapter.defaultTemplateId() ? "" : tpl.key)
        }
        onCancelled: templatePicker.open = false
    }
    // ---- the arrow keys (from the Hotkeys map, never a local chord) ---------
    // Up/Down walk the PICK and stop there: the pane's own preview follows, the
    // output does not move until something explicitly stages or goes live.
    // Left/Right are the pill's ‹ ›: they walk and stage the pick (see stepAndPlay),
    // so the MONITOR PREVIEW follows the keys. While a search answer is up they
    // become the SELECTOR instead (results <-> verses — see stepColumn).
    //
    // GATED, both ways: only the pane on screen answers (`visible`), only when it
    // is not in its search mode, and NEVER while a text field holds the
    // keyboard — a window shortcut is delivered before the focus item, so an
    // ungated Up would swallow a caret move in the tab's search box.
    readonly property bool typingInField: {
        const w = Window.window
        const f = w ? w.activeFocusItem : null
        if (!f)
            return false
        // TextInput / TextEdit (and their Controls wrappers) expose both; a
        // ListView or a pane does not.
        return f.text !== undefined && f.cursorPosition !== undefined
    }
    readonly property bool keysMine: root.visible && !root.searching && !root.typingInField

    // ---- the selector across the panes ------------------------------------------
    // BROWSE (no search): 0 = years, 1 = sermons (chapters), 2 = the paragraphs.
    // THREE panes, and the design preview on the right is NOT one of them (user
    // call: "the selector should not go to ... just 3 panes") — the preview follows
    // the pick, the arrows never park on it.
    //
    // SEARCH (an answer is up): exactly TWO stops — 2 = the result list and 3 = the
    // verses of the highlighted result (the preview panel on the right of it). The
    // years and sermons are narrowed by CLICKING one of their rows, never by parking
    // the selector there (user call: "the <> should not move the selection to years
    // again in search mode, just the search result and the verses"). Up/Down on the
    // verses read them paragraph by paragraph — the walk it used to lack ("...not
    // even show selector at all").
    //
    // Left/Right move it; Up/Down walk inside whichever pane holds it.
    property int column: 2
    // A SEARCH IS UP while its answer is held — not while the narrowed view happens to
    // have rows (walking to a year with no hits must not silently end the search and
    // start opening chapters under the arrows).
    readonly property bool searchMode: root.allMatches.length > 0 || root.citationMatches.length > 0
    // The search's own cursors over the first two columns: while a search is up, walking
    // the years/sermons NARROWS the result list instead of opening anything — nothing
    // may move behind the operator's back (the preview is frozen until a commit).
    property int bookCursor: -1
    property int chapterCursor: -1
    // The whole search answer, kept apart from what the results column shows: narrowing
    // by year/sermon is a view over it, so walking back widens it again.
    property var allMatches: []
    function narrowMatches() {
        const bookId = (root.bookCursor >= 0 && root.bookCursor < root.books.length)
                       ? root.books[root.bookCursor].id
                       : (root.book ? root.book.id : "")
        // A sermon picked in the citations column narrows to that sermon; 0 = the
        // whole year (clicking a year resets it).
        const ch = root.chapterCursor > 0 ? root.chapterCursor : 0
        let rows = root.allMatches
        if (bookId !== "")
            rows = rows.filter((r) => !r.ref || r.ref.bookId === bookId)
        if (ch > 0)
            rows = rows.filter((r) => !r.ref || Number(r.ref.chapter) === Number(ch))
        root.citationMatches = rows
        // THE RESULT CURSOR FOLLOWS THE NARROWING: the list keeps a walkable row and
        // the preview beside it shows that row. Before this, clicking a year while the
        // selector was on the results (or on the year row) left the previous answer in
        // the pane while the list beside it had changed — the reported "it updated the
        // next pane but the 3rd pane did not update" (user call).
        root.matchCursor = rows.length > 0 ? 0 : -1
        root.peekRow = 0
        if (rows.length > 0)
            root.peekSuggestion(0)
        else {
            root.peekIndex = -1
            root.peekVerses = []
        }
        if (matchesList)
            matchesList.positionViewAtBeginning()
    }
    function stepColumn(delta) {
        if (root.searchMode) {
            // TWO STOPS ONLY while a search is up (see the property's own note): the
            // result list and the verses of the row it is on. A selector left on a
            // year/sermon row by an earlier browse lands back on the results, never on
            // those panes again.
            if (root.column !== 2 && root.column !== 3)
                root.column = 2
            if (delta < 0 && root.column === 3) {
                root.column = 2
                return true
            }
            if (delta > 0 && root.column === 2) {
                // ARRIVING ON THE VERSES takes the row the list is showing, so the pane
                // opens on something instead of an empty panel.
                if (root.matchCursor < 0 && root.citationMatches.length > 0)
                    root.stepMatch(1)
                root.column = 3
                return true
            }
            return false
        }
        const next = Math.max(0, Math.min(2, root.column + delta))
        if (next === root.column)
            return false
        root.column = next
        return true
    }
    // The verses of the highlighted result: one paragraph per press, the same walk the
    // paragraph list has. The result list keeps its own cursor, so stepping back
    // (Left) lands on the entry these verses belong to.
    function stepPeek(direction) {
        if (root.peekVerses.length === 0)
            return false
        const next = Math.max(0, Math.min(root.peekVerses.length - 1, root.peekRow + direction))
        if (next === root.peekRow)
            return false
        root.peekRow = next
        if (peekFlick)
            peekFlick.positionViewAtIndex(next, ListView.Contain)
        return true
    }
    // Up/Down: inside the CURRENT pane, in either mode — the search changes what the
    // panes hold, not which pane the selector is on (user call: "the selector move
    // but when now pressed up and down it did not move").
    function stepInColumn(direction) {
        if (root.searchMode) {
            // The search's two stops only. On the verses, Up/Down read that result's
            // paragraphs one by one (they never re-open a sermon — that only happens
            // on Enter / a double-click); on the list they walk the results, and each
            // one's verses follow on the right.
            if (root.column === 3)
                return root.stepPeek(direction)
            root.column = 2
            return root.stepMatch(direction)
        }
        if (root.column === 0) {
            const bi = root.bookIndex + direction
            if (bi < 0 || bi >= root.books.length)
                return false
            root.openBook(bi, 0)
            return true
        }
        if (root.column === 1)
            return root.stepChapter(direction)
        return root.step(direction)
    }

    // THE TWO MODES, SEPARATED (user call: "the moment we are in search mode the
    // controls have change so separate is in order not to cause confusion").
    //
    //   BROWSE — Up/Down walk the row list (paragraphs, or the chapter list when the
    //   selector is on it), Left/Right move the SELECTOR across the three panes
    //   (years / sermons / paragraphs). Nothing is staged by moving (Enter or a
    //   double-click is the commit).
    //
    //   SEARCH — the in-pane result list is up: Up/Down walk the RESULTS (each one's
    //   paragraphs preview on the right), Left/Right still move the selector, and
    //   nothing goes anywhere near the monitor until Enter takes a result.
    //
    // The reported mistake this ends: a chapter row highlighted, Up pressed, and the
    // monitor preview started showing a passage — a step read as a commit.
    Shortcut { action: "previousPassage"; enabled: root.keysMine; onActivated: root.stepInColumn(-1) }
    Shortcut { action: "nextPassage"; enabled: root.keysMine; onActivated: root.stepInColumn(1) }
    // ESC RETURNS THE PANE TO WHERE THE SEARCH STARTED (see beginSearch/endSearch),
    // from anywhere in the pane — not only while the tab's search field still holds
    // the keyboard. Once a row has been clicked the field no longer has focus, and
    // before this the pane owned no Esc at all, so the restore only worked while the
    // caret happened to still be in the box (the reported "I press esc and it does
    // not take me back"). Gated off while a dialog owns Esc (Hotkeys' scope stack),
    // so the two never fire ambiguously.
    Shortcut {
        action: "cancel"
        enabled: root.keysMine && root.searchReturn !== null && !Hotkeys.canCancel
        onActivated: root.endSearch()
    }
    // THE SAME ‹ › AS THE PILL, ON THE KEYBOARD (user call: "also hotkeys <>"): in
    // BROWSE they step the pick and STAGE it — the monitor preview moves exactly as
    // it does for the pill's arrows and the wall's (stepAndPlay). While a search
    // answer is up they are the SELECTOR's instead (results <-> verses), because
    // that is the only navigation a search has.
    Shortcut { action: "previousSlide"; enabled: root.keysMine
               onActivated: root.searchMode ? root.stepColumn(-1) : root.stepAndPlay(-1) }
    Shortcut { action: "nextSlide"; enabled: root.keysMine
               onActivated: root.searchMode ? root.stepColumn(1) : root.stepAndPlay(1) }
    // ENTER takes the pick the arrows walked to. Up/Down deliberately only move the
    // pick (nothing is staged while browsing), so this is the one key that puts it
    // on the MONITOR PREVIEW — `playPicked` -> goLiveRequested -> stageSlides, the
    // same staging a double-click does; GO LIVE still decides what goes to air.
    // `!Hotkeys.canAccept` keeps it from colliding with a dialog's own Enter.
    Shortcut {
        action: "accept"
        enabled: root.keysMine && !Hotkeys.canAccept
        // WHILE A SEARCH IS UP Enter takes the walked result (and leaves the search);
        // otherwise it stages the pick as it always has.
        onActivated: root.searchMode ? root.takeMatch() : root.playPicked()
    }

    // This pane stays instantiated when its tab isn't the one showing (so it keeps its column widths for the session) - so a picker
    // left open has to close itself when the pane is hidden, or it is still open, invisibly wrong-z-ordered against whatever the user
    // navigates to next, when the pane (and so the picker) shows again.
    onVisibleChanged: {
        if (!root.visible) {
            templatePicker.open = false
            // ...and the moment to remember this tab's own split choice (see the
            // re-apply on the way back in, below).
            const p = root.adapter ? String(root.adapter.optionsPrefix || "") : ""
            if (p !== "") {
                root.rememberedSplit = root.options[p + ".splitLongVerses"] === true
                root.hasRememberedSplit = true
            }
            // Leaving the tab is the moment to have the place in the store.
            root.saveReading()
            return
        }
        // Coming back is when the split is most likely to be stale (the template was
        // edited while this pane was hidden — see refillIfStale).
        //
        // THE TAB RE-APPLIES ITS OWN SPLIT SETTING ON ACTIVATION (user call: "make the
        // tab remember and re-apply its split setting per tab so switching tabs never
        // changes how a passage is divided").
        //
        // The choice already LIVES in this tab's own key (scripture.* / table.* /
        // shows.*, see AppSettings' fill-tab table), so it survives a visit elsewhere
        // by itself. What is remembered here is the value this pane had when it was
        // last on screen: if anything wrote that key while the pane was hidden, the
        // tab puts its own answer back on the way in, and the division the operator
        // left with is the division they return to. Nothing flickers when the values
        // agree — this writes only when they don't.
        const p = root.adapter ? String(root.adapter.optionsPrefix || "") : ""
        if (p !== "" && root.hasRememberedSplit
                && (root.options[p + ".splitLongVerses"] === true) !== root.rememberedSplit) {
            SettingsService.setValue(p + ".splitLongVerses", root.rememberedSplit)
        }
        // A NEW VISIT IS A NEW CHANCE to re-cut a passage whose rows came back
        // undivided (see refillIfStale, and the re-cut TheTableService makes when a
        // tab that says divide gets no division at all).
        root.refillTriedFor = ""
        root.refillIfStale()
    }
}
