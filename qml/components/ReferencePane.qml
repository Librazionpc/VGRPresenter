import QtQuick
import VGRPresenterUI

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
    property var selected: []            // verse numbers, ascending
    property int anchorVerse: 0
    property bool optionsOpen: false
    property bool searching: false
    property var searchResults: []
    // The books / chapters / preview columns' widths - each one dragged with its own SplitHandle (double-click puts it back). The verses
    // column takes whatever is left, so it's the one that gives way as the others grow. Remembered across restarts (shared between
    // Scripture and The Table, since it is the same pane): the initial value is whatever was last saved, and every later plain
    // assignment (a drag, a reset, the titles auto-widen below) breaks that binding, same as any other controlled QML property - only
    // `saveColumnWidths` below writes back out, on a short idle after the value stops changing rather than on every drag frame.
    property real booksColWidth: Number(SettingsService.values["session.referencePaneBooksWidth"] ?? 150)
    property real chaptersColWidth: Number(SettingsService.values["session.referencePaneChaptersWidth"] ?? 52)
    property real previewColWidth: Number(SettingsService.values["session.referencePanePreviewWidth"] ?? Math.max(320, Math.min(420, root.width * 0.3)))
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
        root.notesRows = root.hasUserData ? root.userApi.notes() : []
    }

    function toggleHighlight(v) {
        if (!root.hasUserData || !root.book)
            return
        // The engine's SetHighlight is idempotent — write the inverted state
        // straight from the batched map, no read-then-write race window.
        root.userApi.setHighlighted(root.refOfVerse(v), !root.markFor(v).highlight)
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
        root.userApi.setNote(root.refOfVerse(root.noteVerse), draft)
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
    readonly property var currentPreviewSlide: root.previewSlides.length > 0
        ? root.previewSlides[Math.max(0, Math.min(root.previewSlideIndex, root.previewSlides.length - 1))]
        : null
    onPreviewChanged: {
        if (root.previewSlideIndex >= root.previewSlides.length)
            root.previewSlideIndex = 0
    }
    // Show one row for each output slide. Several split pieces can share a
    // slide; combine them into one row labeled by its first piece. Search
    // results stay unchanged.
    readonly property var mainRows: {
        const piecesByVerse = ({})
        for (let slideIndex = 0; slideIndex < root.previewSlides.length; ++slideIndex) {
            const parts = root.previewSlides[slideIndex].parts || []
            for (const part of parts) {
                const key = String(part.number)
                if (!piecesByVerse[key]) piecesByVerse[key] = []
                piecesByVerse[key].push({ number: part.number, text: part.text, partIndex: part.partIndex,
                                          slideIndex: slideIndex, continuation: part.continuation })
            }
        }
        const rows = []
        for (const verse of root.chapterVerses) {
            const pieces = piecesByVerse[String(verse.number)] || []
            pieces.sort((a, b) => a.partIndex - b.partIndex)
            const isSplit = pieces.length > 1
            if (!isSplit) {
                rows.push({ number: verse.number, text: verse.text, continuation: false, isSplit: false,
                            partIndex: 0, slideIndex: 0 })
                continue
                }
            const groups = ({})
            const slideOrder = []
            for (const piece of pieces) {
                const key = String(piece.slideIndex)
                if (!groups[key]) {
                    groups[key] = []
                    slideOrder.push(piece.slideIndex)
                }
                groups[key].push(piece)
            }
            slideOrder.sort((a, b) => a - b)
            for (const slideIndex of slideOrder) {
                const group = groups[String(slideIndex)]
                const first = group[0]
                rows.push({ number: verse.number,
                            text: group.map((piece) => piece.text).join(" "),
                            partIndex: first.partIndex,
                            slideIndex: slideIndex,
                            continuation: first.partIndex > 0,
                            isSplit: true })
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
    readonly property var options: SettingsService.values

    // The adapter object is a plain QtObject with `changed` emitted by its
    // owner (the wrapper binds it to its service's signal) — no direct
    // service reference anywhere in this file.
    Connections {
        target: root.adapter ? root.adapter : null
        function onChanged() {
            root.engineRevision++
            root.syncSources()
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
        root.syncSources()
        SettingsService.whenReady(root.syncSources)
    }

    // ---- opening things ----

    // Keeps a valid Bible open: the one used last, else the first installed.
    function syncSources() {
        // Wait for the engine's store: before it is up the remembered key reads
        // as merely absent, and acting on that would stick the wrong source (see
        // the SettingsService connection above, which calls back once it loads).
        if (!SettingsService.ready)
            return
        const list = root.sources
        if (list.length === 0) {
            root.sourceId = ""
            root.books = []
            root.chapterVerses = []
            root.selected = []
            return
        }
        if (root.currentSource)
            return
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
        const same = root.books.findIndex((b) => b.id === previousBook)
        root.openBook(same >= 0 ? same : 0, same >= 0 ? root.chapterNumber : 0)
        const lastKey = (root.adapter && root.adapter.sessionSourceKey) ? String(root.adapter.sessionSourceKey) : ""
        if (lastKey !== "" && String(SettingsService.values[lastKey] ?? "") !== id)
            SettingsService.setValue(lastKey, id)
    }

    function openBook(index, chapter) {
        root.bookIndex = Math.max(0, Math.min(index, root.books.length - 1))
        const b = root.book
        if (!b)
            return
        revealBook(root.bookIndex)
        const wanted = chapter > 0 && b.chapters.indexOf(chapter) >= 0 ? chapter : b.chapters[0]
        root.openChapter(wanted, true)
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
        root.peekIndex = -1                   // (a real open ends any hover peek)
        root.peekVerses = []
        root.previewSlideIndex = 0
        root.chapterNumber = number
        root.chapterVerses = root.book ? root.adapter.chapter(root.sourceId, root.book.id, number) : []
        root.searching = false
        revealChapter(number)
        if (selectFirst && root.chapterVerses.length > 0) {
            root.selected = [root.chapterVerses[0].number]
            root.anchorVerse = root.chapterVerses[0].number
        } else {
            root.selected = []
        }
        versesFlick.contentY = 0
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

    function selectSubpart(number, partIndex) {
        root.selected = [number]
        root.anchorVerse = number
        root.previewSlideIndex = 0
        const slides = root.adapter.preview(root.sourceId, root.book.id, root.chapterNumber, [number]).slides || []
        for (let i = 0; i < slides.length; ++i) {
            const parts = slides[i].parts || []
            if (parts.some((part) => part.number === number && part.partIndex === partIndex)) {
                root.previewSlideIndex = i
                // Keep the Main Output tile on the same continuation as the
                // reference preview. goLiveRequested stages while idle and
                // follows the existing live-pick behavior while on air.
                const outputSlides = root.buildSlides()
                const outputSlide = outputSlides.find((slide) =>
                    (slide.parts || []).some((part) => part.number === number && part.partIndex === partIndex))
                if (outputSlide)
                    root.goLiveRequested(root.referenceText, [outputSlide])
                return
            }
        }
    }

    // The previous / next verse (crossing into the next chapter or book).
    function step(direction) {
        if (!root.book || root.chapterVerses.length === 0)
            return
        const last = root.selected.length > 0 ? root.selected[root.selected.length - 1] : 0
        const numbers = root.chapterVerses.map((v) => v.number)
        const at = numbers.indexOf(direction > 0 ? last : root.selected[0])
        const to = at + direction
        if (to >= 0 && to < numbers.length) {
            root.selected = [numbers[to]]
            root.anchorVerse = numbers[to]
            return
        }
        const chapters = root.book.chapters
        const ci = chapters.indexOf(root.chapterNumber) + direction
        if (ci >= 0 && ci < chapters.length) {
            root.openChapter(chapters[ci], false)
            root.selected = [root.chapterVerses[direction > 0 ? 0 : root.chapterVerses.length - 1].number]
        } else if (root.bookIndex + direction >= 0 && root.bookIndex + direction < root.books.length) {
            root.openBook(root.bookIndex + direction, 0)
            if (direction < 0) {
                const b = root.book
                root.openChapter(b.chapters[b.chapters.length - 1], false)
                root.selected = [root.chapterVerses[root.chapterVerses.length - 1].number]
            }
        }
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
        const chapters = root.book ? root.book.chapters : []
        const target = ref.chapter > 0 && chapters.indexOf(ref.chapter) >= 0 ? ref.chapter : (chapters.length > 0 ? chapters[0] : 0)
        if (root.bookIndex !== index || root.chapterNumber !== target)
            root.openBook(index, ref.chapter > 0 ? ref.chapter : 0)
        if (ref.verseStart > 0) {
            const to = ref.verseEnd > 0 ? ref.verseEnd : ref.verseStart
            const picked = root.chapterVerses.map((v) => v.number).filter((n) => n >= ref.verseStart && n <= to)
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
                root.previewSlideIndex = 0
                // Bring the first picked verse into view (rows are a fixed 38, as in the list).
                versesFlick.contentY = Math.max(0, (picked[0] - 1) * 38 - 40)
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

    readonly property var visibleBooks: {
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

    function highlight(text) {
        const source = String(text ?? "")
        if (source === "")
            return ""
        const words = root.highlightQuery.trim().toLowerCase().split(/\s+/).filter((w) => w.length >= 2)
        if (words.length === 0)
            return source
        const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        const safeWords = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        const re = new RegExp("(" + safeWords.join("|") + ")", "gi")
        // Qt StyledText ignores background-color on spans (bold survived, the yellow
        // didn't render — user screenshot). A colored <font> is honored.
        return esc(source).replace(re, '<b><font color="#ffd54a">$1</font></b>')
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
    function citeRow(book, index, cite) {
        let code = "", title = cite
        let m = /^(\d{2}\s*-\s*\d{4}[A-Za-z]?)\s*-\s*(.+)$/.exec(cite)
        if (!m)
            m = /^(\d{2}\s*-\s*\d{4}[A-Za-z]?)\s+(\S.*)$/.exec(cite)
        if (m) {
            code = m[1].replace(/\s+/g, "")
            title = m[2].trim()
        }
        return { label: title,
                 trailing: code,
                 cite: cite,
                 ref: { bookId: book.id, chapter: book.chapters[index], verseStart: 0, verseEnd: 0 } }
    }

    // Live autocomplete for the tab search box: what the typed text still means, as plain data
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
                    return { rows: yearMatches.slice(0, 150), complete: "-" }
                }
            }
            if (needle.length >= 2) {
                const matches = []
                for (const b of root.books) {
                    const titles = b.chapterTitles || []
                    for (let i = 0; i < b.chapters.length; i++) {
                        const cite = String(titles[i] || "")
                        if (cite === "")
                            continue
                        if (cite.toLowerCase().indexOf(needle) >= 0)
                            matches.push({ book: b, index: i, cite: cite })
                    }
                }
                // Unique at 5+ chars: complete inline, no list (the resolve jumps).
                // Unique below 5: one-row list — click it, the fill must not fire.
                if (matches.length === 1 && needle.length >= 5) {
                    const at = matches[0].cite.toLowerCase().indexOf(needle)
                    return { rows: [], complete: matches[0].cite.slice(at + needle.length) }
                }
                if (matches.length > 0)
                    // The list scrolls (matchesList sits in the pane's Flickable);
                    // the cap bounds the worst 2-char case (~a thousand delegates).
                    return { rows: matches.slice(0, 150).map((m) => root.citeRow(m.book, m.index, m.cite)), complete: "" }
            }
            return { rows: [], complete: "" }
        }
        // Inline autofill (the FreeShow Scripture input): the best completion for the typed
        // prefix as it reads ("gene" -> "sis ", "genesis 1" -> ": "). TabSearchBox COMMITS
        // the word into the box with the caret at the end (their searchValue =
        // result.autocompleted), so the next keystroke is the chapter digit; the ": "
        // marker inserts the colon so the digits after it type the verse.
        const starts = root.books.filter((b) => b.name.toLowerCase().indexOf(needle) === 0 ||
                                                needle.indexOf(b.name.toLowerCase()) === 0)
        if (starts.length === 1) {
            const bk = starts[0], bkLower = bk.name.toLowerCase()
            if (bkLower.indexOf(needle) === 0)
                completion = bk.name.slice(needle.length) + " "
            else {
                // Book typed in full + a chapter number -> the ": " marker, the next
                // digits type the verse straight through. Scripture-only now: The Table
                // returns above (citation inline search, no book completions).
                const rest = needle.slice(bkLower.length).trim()
                if (/^\d+$/.test(rest) && bk.chapters.indexOf(parseInt(rest)) >= 0)
                    completion = ": "
            }
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
        // inline completion + auto-colon carry the rest and the chapter is typed straight in.
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
    function runSearch(text) {
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
            root.resolveToken++
            root.resolvedWords = []
        } else if (SearchService.resolveWordsAsync) {
            root.resolveToken++
            SearchService.resolveWordsAsync(text, root.resolveToken)
        } else {
            root.resolvedWords = SearchService.resolveWords(text)
        }
        if (root.adapter && root.adapter.searchAsync) {
            root.searchToken++
            root.adapter.searchAsync(text, root.sourceId, root.searchToken)
        } else {
            root.searchResults = root.adapter.search(text, root.sourceId)
        }
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
        // The Table search already resolves terms while searching. Only run
        // the separate spelling hint when that search found no results.
        if (root.adapter && root.adapter.contentType === "table") {
            root.resolveToken++
            if (rows.length === 0 && searchInput.text.trim() !== "")
                SearchService.resolveWordsAsync(searchInput.text, root.resolveToken)
            else
                root.resolvedWords = []
        }
    }
    // Word-resolution worker answers (applyResolvedWords above).
    Connections {
        target: SearchService
        function onWordsResolved(token, rows) { root.applyResolvedWords(token, rows) }
    }

    // Words to highlight: what the engine actually searched for (resolved),
    // falling back to the raw typed words when nothing needed fixing.
    readonly property string highlightQuery: {
        // (No Array.flat in the QML JS engine — plain loops keep it portable.)
        const words = []
        for (const w of root.resolvedWords)
            for (const p of String(w.resolved).split(/\s+/)) words.push(p)
        return words.length > 0 ? words.join(" ") : root.activeQuery
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
            root.citationMatches = res.rows || []
            root.lastSuggestRows = root.citationMatches
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
    property var peekVerses: []          // [{ number, text }] while hovering a match
    function peekSuggestion(index) {
        if (index < 0 || !root.adapter) {
            root.peekIndex = -1
            root.peekVerses = []
            return
        }
        const row = root.citationMatches[index]
        const ref = row ? row.ref : null
        const ch = ref ? ref.chapter : 0
        if (!(ch > 0)) {
            root.peekIndex = -1
            root.peekVerses = []
            return
        }
        root.peekVerses = root.adapter.chapter(root.sourceId, ref.bookId, ch)
        root.peekIndex = index
    }
    function clearCitationMatches() {
        root.citationMatches = []
        root.lastSuggestRows = []
        root.peekIndex = -1
        root.peekVerses = []
    }
    // The debounced clear behind the row-to-row hover hand-off (see matchHover).
    Timer {
        id: peekClear
        interval: 0
        onTriggered: root.peekSuggestion(-1)
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

    // THE ON-AIR PICK (FreeShow's verse dblclick -> playScripture): the picked
    // verses' slides go straight to the live output. The adapter (or the host)
    // decides how — ScripturePane/TheTablePane hand it to LiveOutputService,
    // which checks the output style's content pills first.
    function playPicked() {
        if (!root.book || root.selected.length === 0)
            return
        // The style gate: the active output's style must admit this tab's
        // content type (the Edit dialog's pills). Greyed pill = not meant for
        // that output — say so instead of silently showing nothing.
        const kind = root.adapter.contentType
        if (kind !== "" && !OutputListModel.activeStyleAllows(kind)) {
            EventBus.notify(qsTr("%1 is switched off in this output's style.").arg(root.adapter.tabLabel),
                            "warning", qsTr("On air"), "live.style.gate")
            return
        }
        const slides = root.buildSlides()
        if (slides.length > 0)
            root.goLiveRequested(root.referenceText, slides)
    }

    // Floating pill buttons: previous / next picked passage (FreeShow's
    // _moveSelection), play what's picked. Wired by the wrappers to the live
    // service when their content is on air; plain navigation otherwise.
    // OWNERSHIP IS CHECKED BEFORE THE STEP: after root.step() the reference
    // text is already the NEW passage while the on-air title is still the
    // old one — a post-step liveIsOurs(newRef) can never match, so the
    // replay never fired and the output stayed frozen on the previous verse
    // while the pane's selection walked on (the toolbar ‹ › and the pane's
    // own pills both died this way).
    function stepAndPlay(direction) {
        const wasOurs = root.adapter && root.adapter.liveIsOurs
                        && root.adapter.liveIsOurs(root.referenceText)
        root.step(direction)
        if (wasOurs)
            root.playPicked()
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
            color: "#12131a"; radius: 6
            Text {
                x: 10; y: 7
                text: (root.adapter ? root.adapter.sidebarLabel : qsTr("Sources"))
                color: Theme.textSecondary
                font.family: Theme.fontFamily; font.pixelSize: 13; font.bold: true
            }
        }

        Flickable {
            id: sourcesFlick
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
                        color: active ? "#1a1414" : (sourceHoverPA.hovered ? "#16171e" : "transparent")

                        Rectangle { visible: sourceRow.active; x: 0; y: 4; width: 3; height: 21; color: Theme.accent; radius: 1.5 }
                        IconGlyph {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            name: "bible"
                            color: sourceRow.active ? Theme.accent : root.textDim
                            width: 12; height: 12
                        }
                        Text {
                            x: 28; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 34
                            text: sourceRow.modelData.name
                            color: sourceRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        // Position-truth hover (the verse rows' pattern) —
                        // a HoverHandler's hovered suffers this build's
                        // broken hover delivery, same as MouseArea.
                        PositionHoverArea { id: sourceHoverPA; anchors.fill: parent; checkAncestors: false; showCursor: false }
                        TapHandler { onTapped: root.openSource(sourceRow.modelData.id) }
                    }
                }

                Text {
                    visible: root.sources.length === 0
                    x: 4; width: parent.width - 8
                    text: root.adapter.loading() ? (root.adapter ? root.adapter.loadingText : qsTr("Loading…")) : (root.adapter ? root.adapter.emptyText : qsTr("Nothing here yet."))
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily; font.pixelSize: 13
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
                color: "#0f1015"
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
        width: root.booksColWidth; height: parent.height
        color: "#12131a"

        Flickable {
            id: booksFlick
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
                        readonly property bool active: root.book !== null && modelData.id === root.book.id
                        width: booksList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: bookRow.active ? "#1e1f28" : (bookHoverPA.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: bookRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.accent }
                        }
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 20
                            text: bookRow.modelData.name
                            color: bookRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 16
                        }
                        PositionHoverArea {
                            id: bookHoverPA
                            anchors.fill: parent
                            checkAncestors: false
                            showCursor: false
                            onClicked: root.openBook(root.books.findIndex((b) => b.id === bookRow.modelData.id), 0)
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
            value: booksCol.width; minValue: 100; maxValue: 340
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
        onHasTitlesChanged: if (hasTitles && !widenedForTitles && !root.chaptersWidthRemembered) { widenedForTitles = true; root.chaptersColWidth = 190 }
        x: booksCol.x + booksCol.width; y: 0
        width: root.chaptersColWidth; height: parent.height
        color: "#101118"

        Flickable {
            id: chaptersFlick
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
                        readonly property bool active: modelData === root.chapterNumber
                        width: chapterList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: chapterRow.active ? "#1e1f28" : (chapterHoverPA.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: chapterRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.accent }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: chaptersCol.hasTitles ? parent.left : undefined
                            anchors.leftMargin: chaptersCol.hasTitles ? 10 : 0
                            anchors.horizontalCenter: chaptersCol.hasTitles ? undefined : parent.horizontalCenter
                            width: chaptersCol.hasTitles ? parent.width - 18 : undefined
                            text: chapterRow.title !== "" ? chapterRow.title : chapterRow.modelData
                            elide: Text.ElideRight
                            color: chapterRow.active ? Theme.textPrimary : root.textDim
                            font.family: Theme.fontFamily; font.pixelSize: 16
                        }
                        PositionHoverArea {
                            id: chapterHoverPA
                            anchors.fill: parent
                            checkAncestors: false
                            showCursor: false
                            onClicked: root.openChapter(chapterRow.modelData, true)
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
        color: "#0f1015"

        Flickable {
            id: versesFlick
            anchors.fill: parent
            clip: true
            contentHeight: (root.searching ? searchList.height
                                            : (root.citationMatches.length > 0 ? matchesList.height : versesList.height)) + 60
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: versesList
                visible: !root.searching && root.citationMatches.length === 0
                x: 0; y: 4; width: parent.width - 8

                Repeater {
                    model: root.mainRows
                    delegate: Item {
                        id: verseRow
                        required property var modelData
                        readonly property bool continuation: modelData.continuation === true
                        readonly property int verseNumber: Number(modelData.number)
                        readonly property int partIndex: Number(modelData.partIndex ?? 0)
                        readonly property int slideIndex: Number(modelData.slideIndex ?? 0)
                        readonly property bool isSplit: modelData.isSplit === true || continuation
                        readonly property bool active: root.selected.indexOf(verseNumber) >= 0
                            && (!isSplit || root.previewSlideIndex === slideIndex)
                        // One line per row, like FreeShow's own verse list (the full paragraph is what the preview pane on the right is
                        // for). A verse spanning several of the sermon's own blank-line paragraphs still carries their break as a literal
                        // "\n" in its text - elide alone does not collapse a real line break, so it is flattened to a space here too.
                        readonly property string oneLine: verseRow.modelData.text.replace(/\s*\n+\s*/g, " ")
                        // The user's marks, re-asked whenever one lands (root.userRevision) —
                        // the verse number wears gold while the verse carries either.
                        // The user's marks, from the ONE batched chapter read
                        // (root.chapterMarks) — no per-row engine calls.
                        readonly property bool marked: !continuation && root.hasUserData && root.markFor(verseNumber).highlight === true
                        readonly property bool hasNote: !continuation && (root.markFor(verseNumber).note || "") !== ""
                        // The open note editor stretches this one row to make room for itself.
                        width: versesList.width
                        height: continuation ? 34 : (root.noteVerse === verseNumber ? 122 : 38)
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 1; radius: 4
                            // Position-truth hover (PositionHoverArea), not DragSource's
                            // containsMouse — the latter LATCHES in this build (hover-enter
                            // delivers, hover-exit never does; see PositionHoverArea's
                            // header), which is exactly the "verse row stays highlighted
                            // after I click it" report.
                            color: verseRow.active ? "#2a2b35"
                                                   : (verseHover.hovered ? "#1a1b23" : "transparent")
                        }
                        PositionHoverArea {
                            id: verseHover
                            anchors.fill: parent
                            checkAncestors: false   // (rows inside a Flickable; no hover-gated visibility in the chain)
                            clickEnabled: false     // the DragSource below owns verse clicks and drags
                        }
                        // The verse number: the usual red, gold while the user has
                        // marked this verse with a highlight or a note.
                        Text {
                            x: verseRow.continuation ? 18 : 10; anchors.verticalCenter: parent.verticalCenter
                            width: 40; horizontalAlignment: Text.AlignRight
                            text: verseRow.continuation
                                ? qsTr("%1-%2").arg(verseRow.verseNumber).arg(verseRow.partIndex)
                                : verseRow.verseNumber
                            color: verseRow.marked ? "#f5c542" : Theme.accent
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
                            payload: root.dragPayload(verseRow.active ? root.selected : [verseRow.verseNumber])
                            label: payload ? payload.items[0].name : ""
                            onActivated: {
                                if (verseRow.isSplit) {
                                    verseSource.owner.selectSubpart(verseRow.verseNumber, verseRow.partIndex)
                                } else {
                                    const mods = Qt.application.keyboardModifiers
                                    verseSource.owner.selectVerse(verseRow.verseNumber, (mods & Qt.ControlModifier) !== 0, (mods & Qt.ShiftModifier) !== 0)
                                }
                            }
                            onOpened: {
                                if (verseRow.isSplit)
                                    verseSource.owner.selectSubpart(verseRow.verseNumber, verseRow.partIndex)
                                else
                                    verseSource.owner.selectVerse(verseRow.verseNumber, false, false)
                                verseSource.owner.playPicked()
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
                            Rectangle { anchors.fill: parent; radius: 5; color: noteBtn.editing || noteHover.hovered ? "#22242e" : "transparent" }
                            IconGlyph {
                                anchors.centerIn: parent
                                name: "pencil"
                                color: noteBtn.editing || verseRow.hasNote ? "#f5c542" : Theme.textSecondary
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
                            color: "#14151d"
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
            }

            Column {
                id: searchList
                visible: root.searching
                x: 0; y: 4; width: parent.width - 8
                // The engine's resolved-words hint ("thn → then   freind →
                // friend"), gold like the match highlight. Tab in the pill box
                // accepts it — the same behavior Quick search has.
                Text {
                    visible: root.resolvedWords.some((w) => w.changed === true)
                    width: parent.width
                    leftPadding: 10
                    color: "#ffd54a"
                    font.family: Theme.fontFamily; font.pixelSize: 13
                    elide: Text.ElideRight
                    text: root.resolvedWords.filter((w) => w.changed === true)
                              .map((w) => w.typed + " \u2192 " + w.resolved).join("   ")
                              + "   (Tab to accept)"
                }
                Repeater {
                    model: root.searchResults
                    delegate: Item {
                        id: hitRow
                        required property var modelData
                        width: searchList.width; height: 32
                        Rectangle { anchors.fill: parent; anchors.margins: 1; radius: 4; color: hitHover.hovered ? "#16171e" : "transparent" }
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter; width: 170
                            text: hitRow.modelData.reference
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 15
                        }
                        Text {
                            x: 190; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 198
                            text: root.canHighlight ? root.highlight(hitRow.modelData.snippet.replace(/\s*\n+\s*/g, " "))
                                                    : hitRow.modelData.snippet.replace(/\s*\n+\s*/g, " ")
                            color: root.textDim
                            elide: Text.ElideRight
                            textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                            font.family: Theme.fontFamily; font.pixelSize: 15
                        }
                        HoverHandler { id: hitHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                root.goTo({ bookId: hitRow.modelData.bookId, chapter: hitRow.modelData.chapter, verseStart: hitRow.modelData.verse, verseEnd: hitRow.modelData.verse })
                                root.searching = false
                            }
                        }
                    }
                }
                Text {
                    visible: root.searchResults.length === 0
                    x: 12; y: 12
                    text: (root.adapter ? root.adapter.searchHint : qsTr("Type words to find, then press Enter."))
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 14
                }
            }

            // THE TABLE's citation matches — IN THE PANE (FreeShow's scripture
            // search: results are drawer content, nothing floats over the search
            // box or the text). One row per sermon matching the typed text; HOVER
            // a row and the right half (peekPanel) shows that sermon's full
            // paragraphs — the preview can't cover anything because it lives in
            // its own half. Click opens the sermon for real.
            Column {
                id: matchesList
                visible: !root.searching && root.citationMatches.length > 0
                // FIXED half-split while the matches show (never re-laid-out by the
                // hover): an early version narrowed the list only WHILE the preview
                // was open, so a pointer near a row's right edge oscillated — hover
                // opens the panel, the row shrinks away under the pointer, the panel
                // closes, the row widens back, hover again…
                x: 0; y: 4; width: parent.width * 0.52
                Repeater {
                    model: root.citationMatches
                    delegate: Item {
                        id: matchRow
                        required property var modelData
                        required property int index
                        objectName: "selfTestMatchRow_" + index   // (the self-test hovers these by name)
                        readonly property bool active: root.peekIndex === index
                        width: matchesList.width; height: 30
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 1; radius: 4
                            color: matchRow.active ? "#2a2b35" : (matchHover.hovered ? "#16171e" : "transparent")
                            Rectangle { visible: matchRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.accent }
                        }
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 86
                            text: matchRow.modelData.label
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 15
                        }
                        Text {
                            anchors.right: parent.right; anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: matchRow.modelData.trailing
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily; font.pixelSize: 11
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
                                    root.peekSuggestion(matchRow.index)
                                } else {
                                    peekClear.restart()   // row-to-row moves deliver exit(A)/enter(B) unordered — the 0-timer lets a following enter cancel the clear
                                }
                            }
                            onClicked: {
                                root.applySuggestion(matchRow.modelData.ref)
                                root.clearCitationMatches()
                            }
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
                Rectangle { anchors.fill: parent; radius: 6; color: "#101118"; border.color: Theme.border; border.width: 1 }
                Text {
                    id: peekHeader
                    x: 12; y: 8
                    width: parent.width - 24
                    text: root.peekIndex >= 0 && root.citationMatches[root.peekIndex] ? (root.citationMatches[root.peekIndex].cite || root.citationMatches[root.peekIndex].label) : ""
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Flickable {
                    id: peekFlick
                    anchors.fill: parent
                    anchors.topMargin: 26
                    anchors.rightMargin: 10
                    clip: true
                    contentHeight: peekCol.height + 20
                    boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: peekCol
                        x: 4; width: parent.width - 8
                        Repeater {
                            model: root.peekVerses
                            delegate: Item {
                                width: peekCol.width; height: peekText.implicitHeight + 10
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
                            }
                        }
                    }
                    // A long sermon's peek overflowed with no way to drag it.
                    AppScrollBar {
                        flickable: peekFlick
                        anchors.top: parent.top; anchors.bottom: parent.bottom
                        anchors.right: parent.right; anchors.rightMargin: 2
                    }
                }
            }
        }

        // Nothing installed / nothing loaded yet.
        Text {
            visible: root.sources.length === 0
            anchors.centerIn: parent
            text: root.adapter.loading() ? (root.adapter ? root.adapter.loadingText : qsTr("Loading…")) : (root.adapter ? root.adapter.emptyText : qsTr("Nothing here yet."))
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 15
        }

        // The verses (or search results / matches) column's scrollbar — the
        // chapter can be 150 sermons or 176 verses; wheel-only was the only
        // way to move it.
        AppScrollBar {
            flickable: versesFlick
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
            color: "#e6101118"
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
            color: "#e6101118"
            border.color: Theme.border

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 2

                Item {   // search field, while searching
                    visible: root.searching
                    width: 220; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: "#181a22"; border.color: Theme.border }
                    TextInput {
                        id: searchInput
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
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
                        onAccepted: { pillDebounce.stop(); root.runSearch(text) }
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
                        interval: 80
                        onTriggered: root.runSearch(searchInput.text)
                    }
                    Text {
                        visible: searchInput.text === ""
                        anchors.verticalCenter: parent.verticalCenter; x: 12
                        text: (root.adapter ? root.adapter.searchPlaceholder : qsTr("Search"))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 14
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
                        width: 28; height: 26
                        Rectangle { anchors.fill: parent; radius: 13; color: stepHover.hovered ? "#22242e" : "transparent" }
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
                            onClicked: root.stepAndPlay(stepBtn.modelData.dir)
                        }
                    }
                }

                Rectangle { visible: !root.searching; width: 1; height: 16; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

                Item {
                    id: playPickedButton
                    visible: !root.searching
                    width: 28; height: 26
                    readonly property bool isOurs: root.adapter && root.adapter.liveIsOurs
                                                   ? root.adapter.liveIsOurs(root.referenceText) : false
                    Rectangle { anchors.fill: parent; radius: 13; color: playPickedHover.hovered || parent.isOurs ? "#22242e" : "transparent" }
                    // FreeShow's scripture play button: PLAY (outlined) when this
                    // passage is not on air; when it IS, the green filled STOP
                    // square — clicking takes it off air (their "stop" state),
                    // NOT a refresh icon (which IconGlyph doesn't even have: the
                    // unknown name fell through to the filled-dot fallback and
                    // rendered as a green blob).
                    IconGlyph {
                        anchors.centerIn: parent
                        name: parent.isOurs ? "stop" : "play"
                        color: parent.isOurs ? "#6dff85" : Theme.textPrimary
                        width: 13; height: 13
                        fit: true
                    }
                    PositionHoverArea {
                        id: playPickedHover
                        anchors.fill: parent
                        checkAncestors: false
                        onClicked: playPickedButton.isOurs ? LiveOutputService.stop() : root.playPicked()
                    }
                }

                // The highlight toggle for the picked passage (the drawer's other half):
                // gold star while verse 1 of the pick is marked — the star marks the
                // FIRST picked verse, per-verse stars live on the rows themselves.
                Item {
                    visible: !root.searching && root.hasUserData
                    width: 28; height: 26
                    readonly property bool marked: root.hasUserData && root.selected.length > 0
                                                   && root.markFor(root.selected[0]).highlight === true
                    Rectangle { anchors.fill: parent; radius: 13; color: starHover.hovered || parent.marked ? "#22242e" : "transparent" }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "star"
                        color: parent.marked ? "#f5c542" : Theme.textPrimary
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
                }

                Rectangle { visible: !root.searching; width: 1; height: 16; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

                Item {
                    width: 28; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: searchHover.hovered || root.searching ? "#22242e" : "transparent" }
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
            color: "#12131a"
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
                    Rectangle { anchors.fill: parent; radius: 10; color: closeHover.hovered ? "#22242e" : "transparent" }
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
                                color: noteRowHover.hovered ? "#1a1b23" : "transparent"
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
                                color: "#f5c542"
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
        width: root.previewColWidth; height: parent.height
        color: "#101118"

        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

        Flickable {
            id: previewFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 1
            anchors.rightMargin: previewScrollBar.visible ? 9 : 0
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
                    color: btnHover.hovered ? "#181a22" : "#0f1015"; border.color: Theme.border
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
                        height: Math.round(width * 428 / 754)
                        color: "#000000"
                        clip: true
                        DesignPreview {
                            anchors.fill: parent
                            blocks: root.currentPreviewSlide ? root.currentPreviewSlide.blocks : root.preview.blocks
                            background: (root.preview.background ?? "") === "" || root.preview.background === "transparent" ? "#000000" : root.preview.background
                        }
                        Text {
                            visible: root.selected.length === 0
                            anchors.centerIn: parent
                            text: qsTr("Pick a verse")
                            color: "#5c6475"
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
                        color: "#0f1015"; border.color: Theme.border
                        clip: true

                        Column {
                            x: 12; anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text { text: qsTr("Template"); color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: 13 }
                            Text {
                                width: 150
                                // Same three states the Show screen's template row uses: nothing chosen,
                                // a real name, or an id whose design is gone. Never a silently blank line.
                                text: root.templateId === ""
                                      ? qsTr("Default — no template")
                                      : (root.templateName !== "" ? root.templateName : qsTr("Missing template"))
                                color: root.templateId === "" ? Theme.textMuted : Theme.textPrimary
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
                                width: 46; height: 56; color: editHover.hovered ? "#1a1b23" : "#0b0c11"
                                IconGlyph { anchors.centerIn: parent; name: "penTool"; color: Theme.textPrimary; width: 14; height: 14 }
                                HoverHandler { id: editHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.templateEditRequested(root.templateId) }
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
                    radius: 8; color: "#0f1015"; border.color: Theme.border

                    Column {
                        id: optionsColumn
                        x: 14; y: 4; width: parent.width - 28
                        spacing: 0

                        component OptionToggle: Item {
                            id: opt
                            property string settingKey: ""
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[opt.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1a1c25" }
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
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1a1c25" }
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
                                    width: 24; height: 24; radius: 6; color: minusHover.hovered ? "#22242e" : "#181a22"; border.color: Theme.border
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
                                    width: 24; height: 24; radius: 6; color: plusHover.hovered ? "#22242e" : "#181a22"; border.color: Theme.border
                                    Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 15 }
                                    HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: num.nudge(num.step) }
                                }
                            }
                        }

                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".verseNumbers" }
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".versesOnIndividualLines" }
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".splitLongVerses" }
                        Column {
                            visible: root.options[root.adapter.optionsPrefix + ".splitLongVerses"] === true
                            width: parent.width
                            OptionToggle { settingKey: root.adapter.optionsPrefix + ".splitLongVersesSuffix" }
                            OptionNumber { settingKey: root.adapter.optionsPrefix + ".longVersesChars"; step: 10 }
                            OptionNumber { settingKey: root.adapter.optionsPrefix + ".longVersesTolerance"; step: 5 }
                        }
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".smartSplit" }
                        // The max caps BOTH modes now — smart split packs by the
                        // template box's capacity but never past this many — so
                        // it stays visible with smart split on (it used to hide,
                        // and was then silently ignored: the "max does nothing" gap).
                        OptionNumber { settingKey: root.adapter.optionsPrefix + ".versesPerSlide" }
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
                            GradientStop { position: 0.0; color: root.optionsOpen ? (optionsHover.hovered ? Theme.accentLight : Theme.accent) : (optionsHover.hovered ? "#2a2d3b" : "#222533") }
                            GradientStop { position: 1.0; color: root.optionsOpen ? "#5a50d6" : "#181a24" }
                        }
                        border.width: 1
                        border.color: root.optionsOpen ? "#ff8a7e" : "#34384a"
                        IconGlyph { anchors.centerIn: parent; name: "sliders"; color: "#ffffff"; fit: true; width: 22; height: 22 }
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
    // This pane stays instantiated when its tab isn't the one showing (so it keeps its column widths for the session) - so a picker
    // left open has to close itself when the pane is hidden, or it is still open, invisibly wrong-z-ordered against whatever the user
    // navigates to next, when the pane (and so the picker) shows again.
    onVisibleChanged: if (!root.visible) templatePicker.open = false
}
