import QtQuick
import VGRPresenterUI
import "../shortcuts"

Item {
    id: root

    property int verseRowBuilds: 0
    property int verseRowDrops: 0
    property int hitRowBuilds: 0
    property int hitRowDrops: 0
    property int matchRowBuilds: 0
    property int matchRowDrops: 0
    property int peekRowBuilds: 0
    property int peekRowDrops: 0
    readonly property var probeEvals: ({ verse: 0, book: 0, chapter: 0, source: 0 })

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
    property bool chapterSelectLast: false
    property double chapterStartedAt: 0
    property var pendingGoTo: null
    property var selected: []            // verse numbers, ascending
    property int anchorVerse: 0
    property string lineStepOwnerRef: ""
    property bool optionsOpen: false
    property bool searching: false
    property bool exact: false
    property bool scopeLock: false
    // The scope the lock currently names, resolved from what is open: "" / 0
    // mean "not locked", which is what every adapter without the feature reads.
    readonly property bool supportsChapterLock: !!(root.adapter && root.adapter.supportsChapterLock)
    readonly property string lockedBookId: (root.scopeLock && root.book) ? String(root.book.id) : ""
    readonly property int lockedChapter: root.scopeLock ? root.chapterNumber : 0
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
    property real booksColWidth: Number(SettingsService.values["session.referencePaneBooksWidth"] ?? 150)
    property real chaptersColWidth: Number(SettingsService.values["session.referencePaneChaptersWidth"] ?? 52)
    property real previewColWidth: Number(SettingsService.values["session.referencePanePreviewWidth"] ?? Math.max(320, Math.min(420, root.width * 0.3)))
    readonly property bool compactReferenceSearch: !!(root.adapter && root.searching)
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
    property int userRevision: 0
    readonly property var userApi: (root.adapter && root.adapter.userData) ? root.adapter.userData : null
    readonly property bool hasUserData: !!(root.userApi && root.userApi.chapterUserData && root.userApi.notes)
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
    readonly property var previewSlideParts: {
        const slides = root.previewSlides
        if (slides.length === 0)
            return []
        return slides[Math.max(0, Math.min(root.previewSlideIndex, slides.length - 1))].parts || []
    }
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

    function revealRow(index) {
        if (index < 0 || index >= root.mainRows.length)
            return
        Qt.callLater(function () { versesFlick.positionViewAtIndex(index, ListView.Beginning) })
    }

    function revealRowCentered(index) {
        if (index < 0 || index >= root.mainRows.length)
            return
        Qt.callLater(function () { versesFlick.positionViewAtIndex(index, ListView.Center) })
    }

    property int followedRowIndex: -1
    function rowFullyVisible(row) {
        if (!row)
            return false
        const pad = 6
        return row.y - pad >= versesFlick.contentY
            && row.y + row.height + pad <= versesFlick.contentY + versesFlick.height
    }
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
            if (force !== true) {
                const previous = root.followedRowIndex >= 0
                    ? versesFlick.itemAtIndex(root.followedRowIndex) : null
                const tracking = root.followedRowIndex < 0
                    || centerRowScroll.running || root.rowFullyVisible(previous)
                if (!tracking)
                    return
            }
            if (row) {
                const targetY = Math.max(0, Math.min(
                    versesFlick.contentHeight - versesFlick.height,
                    row.y + row.height / 2 - versesFlick.height / 2))
                centerRowScroll.to = targetY
                centerRowScroll.restart()
                root.followedRowIndex = index
                return
            }
            versesFlick.positionViewAtIndex(index, ListView.Center)
            root.followedRowIndex = index
        })
    }
    // THE SELECTOR STAYS IN VIEW. A change that re-cuts the rows (Max words, split,
    // verse numbers) rebuilds the list under the reader: the selection is still there but
    // the list can be left anywhere. Once the rows settle, bring the active row back if it
    // is off screen — never while the reader is scrolling the list themselves, and never
    // when it is already visible (so a manual scroll afterwards is left alone).
    property int settledRowCount: -1
    onMainRowsChanged: {
        if (root.mainRows.length === root.settledRowCount)
            return
        root.settledRowCount = root.mainRows.length
        keepSelectionTimer.restart()
    }
    Connections {
        target: LiveOutputService
        function onReadingWordBudgetChanged() { keepSelectionTimer.restart() }
    }
    Timer {
        id: keepSelectionTimer
        interval: 150
        repeat: false
        onTriggered: root.keepSelectionInView()
    }
    function keepSelectionInView() {
        if (root.chapterLoading) {
            keepSelectionTimer.restart()    // the rows are still arriving
            return
        }
        if (root.selected.length === 0 || root.mainRows.length === 0
                || versesFlick.moving || versesFlick.dragging)
            return
        let index = -1
        for (let i = 0; i < root.mainRows.length; ++i) {
            const r = root.mainRows[i]
            if (root.rowIsActive(Number(r.number), Number(r.partIndex ?? 0), r.isSplit === true)) {
                index = i
                break
            }
        }
        if (index < 0)
            index = root.rowIndexFor(root.selected[0])
        if (index < 0)
            return
        const row = root.mainRows[index]
        root.followedRowIndex = -1          // the old index meant a different row list
        root.centerRow(row.number, row.partIndex ?? 0, true)
    }
    NumberAnimation {
        id: centerRowScroll
        target: versesFlick
        property: "contentY"
        duration: 220
        easing.type: Easing.InOutQuad
    }

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

    property var liveFlowSlide: null
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
    readonly property var mainRows: {
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
    readonly property string templateName: { root.engineRevision; return root.adapter.templateName(root.templateId) }
    readonly property bool usingDefaultTemplate: root.templateId === root.adapter.defaultTemplateId()

    // ---- which template the OUTPUT really renders --------------------------------------------
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
    readonly property string readingKey: (root.adapter && root.adapter.readingKey)
        ? String(root.adapter.readingKey) : ""
    // The place read back from the store, held until the open it belongs to has
    // applied it (then it is cleared, so a later open never jumps backwards).
    property var readingRestore: null
    property var readingPlace: null
    property string readingStamp: ""
    // What the NEXT open puts back: the picked numbers, and the row that was on
    // screen (paragraph + piece — see topRowIdentity).
    property var restorePicked: []
    property var restoreRow: null
    property bool rememberedSplit: false
    property bool hasRememberedSplit: false
    // The fill options the rows ON SCREEN were cut with, captured when their chapter
    // was opened; and the one stale state a re-read was already tried for.
    property string openFillSignature: ""
    property string rowsFillSignature: ""
    property string refillTriedFor: ""
    readonly property string fillSignature: {
        const p = root.adapter ? String(root.adapter.optionsPrefix || "") : ""
        const v = root.options || ({})
        return [root.templateId, p, v[p + ".verseNumbers"],
                v[p + ".splitLongVerses"], v[p + ".wordsPerSlide"]].join("|")
    }
    // What the walk picked last time it was read back. `selected` is re-set on every
    // walk step, usually to the same place, and the line was written each time; the walk's
    // own moves are what `ScrollFollow walk` reports.
    property string loggedSelection: ""
    onSelectedChanged: {
        if (root.scrollSourced && LiveOutputService.scrollMode) {
            const now = JSON.stringify(root.selected)
            if (now !== root.loggedSelection) {
                root.loggedSelection = now
                EngineBridge.log("info", "ScrollFollow", "selection is now " + now
                                 + " (walk paragraph " + (root.followedScrollPiece || "none") + ")")
            }
        }
        root.touchReading()
    }
    onChapterNumberChanged: root.touchReading()
    onBookIndexChanged: root.touchReading()
    Timer {
        id: saveReadingTimer
        interval: 350
        onTriggered: root.saveReading()
    }

    // ---- "RELOADING", NOT "EMPTY" (user call: give the library panes a quiet
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
    function libraryHint() {
        if (root.sources.length > 0)
            return ""
        if (root.showReloading) {
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

    Connections {
        target: root.adapter ? root.adapter : null
        function onChanged() {
            root.engineRevision++
            root.syncSources()
            // A template edit, a reload or an option written behind the pane leaves
            // the list holding a cut the tab no longer asks for (see refillIfStale).
            root.refillIfStale()
        }
        function onSearchCached(text) {
            if (root.searching && searchInput.text.trim() === text)
                root.runSearch(text)
        }
    }
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
        if (root.book !== null)
            return
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
        if (!SettingsService.ready && root.readingPlace === null)
            return
        root.loadReading()
        const list = root.sources
        if (list.length === 0) {
            if (!root.book && root.chapterVerses.length === 0) {
                root.sourceId = ""
                root.books = []
            }
            return
        }
        if (root.currentSource) {
            if (root.books.length === 0)
                root.openSource(root.sourceId)
            return
        }
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
        if (root.readingRestore !== null) {
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
        const opened = root.chapterLoading
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

    function stageSubpart(number, partIndex) {
        root.selectSubpart(number, partIndex)
        root.stageOutputPart(number, partIndex)
    }

    property bool followSelectorMoves: true
    // THE PIECE THE OPERATOR JUST PICKED (see followLiveStep): a click and the ‹ ›
    // set it before staging, so the follow can tell an operator's page from a step's.
    property var operatorPiece: null
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
        const op = root.operatorPiece
        const opMatches = (part) => op && Number(part.number) === Number(op.number)
                                         && Number(part.partIndex ?? 0) === Number(op.part)
        if (op && parts.some(opMatches)) {
            root.operatorPiece = null
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
        if (root.scrollSourced && LiveOutputService.scrollMode) {
            EngineBridge.log("info", "ScrollFollow",
                "staged slide names paragraph " + parts[0].number + " part=" + (parts[0].partIndex ?? 0)
                + " — ignored while scrolling; walk is on " + (root.followedScrollPiece || "none")
                + " selected=" + JSON.stringify(root.selected))
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
        Qt.callLater(function () {
            const i = root.slideIndexForPart(row.number, row.partIndex ?? 0)
            if (i >= 0)
                root.previewSlideIndex = i
        })
    }

    function step(direction) {
        if (!root.book || root.mainRows.length === 0)
            return false
        // A chapter still being read has no rows to walk yet: a press now would find no
        // current row and (below) read as "past the edge".
        if (root.chapterLoading)
            return false
        const at = root.currentRowIndex(direction)
        const to = at + direction
        if (at >= 0 && to >= 0 && to < root.mainRows.length) {
            root.focusRow(root.mainRows[to])
            return true
        }
        if (at < 0) {
            if (root.chapterVerses.length > 0) {
                root.focusRow(root.mainRows[direction > 0 ? 0 : root.mainRows.length - 1])
                return true
            }
            return false
        }
        // A REAL edge: the pick is the first (or last) row of what is being read. The
        // step STOPS here: it does not throw the reader into the neighbouring chapter,
        // sermon or book (that is what the chapter and book lists are for).
        return false
    }

    function stepChapter(direction, withinBook) {
        if (!root.book)
            return false
        const chapters = root.book.chapters || []
        const ci = chapters.indexOf(root.chapterNumber) + direction
        if (ci >= 0 && ci < chapters.length) {
            root.openChapter(chapters[ci], direction > 0)
            root.chapterSelectLast = direction < 0
            return true
        }
        if (withinBook === true)
            return false
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
        root.peekIndex = -1
        root.peekVerses = []
        const index = root.books.findIndex((b) => b.id === ref.bookId)
        if (index < 0)
            return
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

    property int corpusRevision: 0
    function corpusCacheLanded() {
        root.corpusRevision++
        if (root.peekIndex >= 0)
            root.peekSuggestion(root.peekIndex)
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
        return named.length > 0 ? named : root.books
    }

    readonly property string activeQuery: root.searching ? searchInput.text : root.filter

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

    function suggest(text, sourceId) {
        const rows = []
        let completion = null
        if (!root.adapter || !text)
            return { rows: rows, complete: completion }
        const needle = String(text).trim().toLowerCase()
        if (needle === "")
            return { rows: rows, complete: completion }
        if (root.books.length > 0 && root.books[0].chapterTitles !== undefined) {
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
                    return { rows: matches.map((m) => root.citeRow(m.book, m.index, m.cite)), complete: "" }
            }
            return { rows: [], complete: "" }
        }
        const starts = root.books.filter((b) => b.name.toLowerCase().indexOf(needle) === 0 ||
                                                needle.indexOf(b.name.toLowerCase()) === 0)
        if (starts.length === 1) {
            const bk = starts[0], bkLower = bk.name.toLowerCase()
            if (bkLower.indexOf(needle) === 0)
                completion = bk.name.slice(needle.length) + " "
        }
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

    property var resolvedWords: []
    property int searchToken: 0
    property int resolveToken: 0
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
    function toggleScopeLock() {
        if (!root.book) return   // nothing open to lock to
        root.scopeLock = !root.scopeLock
    }
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

    readonly property string highlightQuery: {
        const typed = root.activeQuery.trim().toLowerCase().match(/[a-z0-9]+/g) || []
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
    function paneSuggestions(text) {
        const res = root.suggest(text, root.sourceId)
        if (root.books.length > 0 && root.books[0].chapterTitles !== undefined) {
            root.allMatches = res.rows || []
            root.citationMatches = root.allMatches
            root.lastSuggestRows = root.allMatches
            root.matchCursor = -1
            root.bookCursor = -1
            root.chapterCursor = -1
            if (root.citationMatches.length > 0)
                root.column = 2
            return { rows: [], complete: res.complete || "" }
        }
        root.lastSuggestRows = res.rows || []
        return { rows: (res.rows || []).map((row) => ({ label: row.label, trailing: row.trailing || row.detail || "",
                                                        payload: row.cite !== undefined ? { ref: row.ref, cite: row.cite } : { ref: row.ref } })),
                 complete: res.complete || "" }
    }
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
    property var citationMatches: []    // [{ label, trailing, cite, ref }] (citeRow shape)
    property var lastSuggestRows: []    // (the popup's rows — Scripture only now)
    property int peekIndex: -1
    property int peekRequestToken: 0
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
    // ---- ESC: the search is abandoned, not just emptied --------------------------
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
        root.searching = false
        searchInput.text = ""
        // TAKEN IS TAKEN: the search is committed, so there is nothing left for Esc to
        // take the reader back from.
        root.searchReturn = null
        root.matchTaken()
        return cite
    }
    property var peekVerses: []          // [{ number, text }] while hovering a match
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

    function chapterItemText(verse) {
        const numbersOn = root.options[root.adapter.optionsPrefix + ".verseNumbers"] === true
        const body = String(verse.text ?? "")
        if (!numbersOn)
            return body
        return String(verse.number ?? "") + " " + body
    }

    function readingPagesFromChapter(number, partIndex) {
        // (No pre-check on the list here: the passage is built first and the pick is
        // matched against the items it actually contains, below.)
        const row = root.mainRows.find((r) => Number(r.number) === Number(number)
                                              && Number(r.partIndex ?? 0) === Number(partIndex ?? 0))
        // EVERY ITEM STARTS ON A NEW LINE, Table paragraph and Bible verse alike. Joined by a space, a
        // verse began part-way along a line, and each screen (its own width and wrap) started the picked
        // verse at a DIFFERENT word — whichever line it fell on. A hard break makes the pick's first word
        // the first word of a line on every screen, as a Table paragraph already is. (One character either
        // way, so every offset below and pieceAtChapterOffset's joining arithmetic are unchanged.)
        const paragraphBreak = "\n"
        let text = ""
        const items = []
        for (const verse of root.chapterVerses) {
            const continuation = Number(verse.partIndex ?? 0) > 0
                || String(verse.number ?? "").indexOf("-") >= 0
            if (continuation)
                continue
            if (text !== "")
                text += paragraphBreak
            const verseText = root.chapterItemText(verse)
            items.push({ number: Number(verse.number),
                         partIndex: Number(verse.partIndex ?? 0),
                         start: text.length,
                         end: text.length + verseText.length })
            text += verseText
        }
        const picked = items.find((it) => it.number === Number(number)
                                          && it.partIndex === Number(partIndex ?? 0))
                       || items.find((it) => it.number === Number(number))
        if (!picked)
            return { pages: [], index: 0 }
        let selectedOffset = picked.start
        if (row) {
            const found = text.indexOf(String(row.text ?? ""), picked.start)
            if (found >= 0 && found < picked.end)
                selectedOffset = found
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
            // A page that ends where a paragraph ends would carry that break as a
            // trailing blank line; a space is the same one character and draws nothing.
            pages.push({ body: text.slice(start, end).replace(/\n$/, " "), start: start })
        }
        const before = []
        for (let back = pivot; back > 0; ) {
            const from = Math.max(0, back - wordsPerScreen)
            before.unshift({ from: from, to: back })
            back = from
        }
        for (const range of before)
            appendRange(range.from, range.to)
        let selectedPage = pages.length
        let cursor = pivot
        while (cursor < matches.length) {
            const next = Math.min(cursor + wordsPerScreen, matches.length)
            appendRange(cursor, next)
            cursor = next
        }
        if (pages.length > 0)
            selectedPage = Math.min(selectedPage, pages.length - 1)
        if (typeof EngineBridge !== "undefined")
            EngineBridge.log("info", "ChapterPassage",
                "number=" + number + " part=" + partIndex
                + " itemStart=" + picked.start + " selectedOffset=" + selectedOffset
                + " pivot=" + pivot
                + " pages=" + pages.length + " startPage=" + Math.max(0, selectedPage)
                + " pageAt=" + (pages.length > 0
                                ? String(pages[Math.max(0, selectedPage)].body).slice(0, 30) : "-")
                + " head=" + text.slice(0, 30))
        return { pages: pages, index: Math.max(0, selectedPage) }
    }

    function pieceAtChapterOffset(charOffset) {
        if (root.chapterVerses.length === 0)
            return null
        let at = 0
        let lastKept = null
        for (const verse of root.chapterVerses) {
            const continuation = Number(verse.partIndex ?? 0) > 0
                || String(verse.number ?? "").indexOf("-") >= 0
            if (continuation)
                continue
            if (at > 0)
                at += 1                       // the joining space between items
            const text = root.chapterItemText(verse)
            lastKept = verse
            if (charOffset > at + text.length) {
                at += text.length
                continue
            }
            let part = Number(verse.partIndex ?? 0)
            let best = -1
            for (const row of root.mainRows) {
                if (Number(row.number) !== Number(verse.number))
                    continue
                const found = text.indexOf(String(row.text ?? ""))
                if (found < 0 || found > charOffset - at)
                    continue
                if (found >= best) {
                    best = found
                    part = Number(row.partIndex ?? 0)
                }
            }
            return { number: Number(verse.number), partIndex: part }
        }
        return lastKept ? { number: Number(lastKept.number),
                            partIndex: Number(lastKept.partIndex ?? 0) } : null
    }

    function playPicked() {
        if (!root.book || root.selected.length === 0)
            return
        LiveOutputService.noteOperatorJump()
        const kind = root.adapter.contentType
        if (kind !== "" && !OutputListModel.activeStyleAllows(kind)) {
            EventBus.notify(qsTr("%1 is switched off in this output's style.").arg(root.adapter.tabLabel),
                            "warning", qsTr("On air"), "live.style.gate")
            return
        }
        const previewed = root.previewSlides[root.previewSlideIndex]
        const shownParts = (previewed && previewed.parts) || []
        const inPick = shownParts.length > 0
            && shownParts.every((p) => root.selected.indexOf(Number(p.number)) >= 0)
        if (inPick && root.playSlideAt(root.previewSlideIndex))
            return
        const slides = root.buildSlides()
        if (slides.length === 0)
            return
        const first = ((slides[0] && slides[0].parts) || [])[0]
        if (first)
            root.operatorPiece = { number: Number(first.number),
                                   part: Number(first.partIndex ?? 0) }
        root.lineStepOwnerRef = root.referenceText
        if (first && root.sendScrollPick(root.referenceText, slides, 0, slides, 0,
                                         first.number, first.partIndex ?? 0)) {
            root.refreshLiveFlowSlide()
            return
        }
        root.goLiveRequested(root.referenceText, slides)
        if (first)
            root.rememberPick(slides, 0, first.number, first.partIndex ?? 0)
        else
            root.scrollPiece = null
        if (LiveOutputService.scrollMode)
            root.ensureScrollSource()
        root.refreshLiveFlowSlide()
    }

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
        if (root.matchCursor >= 0 && root.matchCursor < root.citationMatches.length) {
            const row = root.citationMatches[root.matchCursor]
            if (row && row.ref) {
                root.applySuggestion(row.ref)
                root.clearCitationMatches()
            }
        }
        if (root.stepSlide(direction, true))
            return
        const before = root.referenceText
        const moved = root.step(direction)
        if (!moved && root.referenceText === before)
            return          // nothing that way (the first/last passage of them all)
        if (!moved) {
            if (root.previewSlides.length > 0)
                root.previewSlideIndex = direction < 0 ? root.previewSlides.length - 1 : 0
        }
        Qt.callLater(function () { root.playSlideAt(root.previewSlideIndex) })
    }

    function stepSlide(direction, publish) {
        const slides = root.previewSlides
        if (slides.length < 2)
            return false                          // nothing to walk: one slide
        const next = root.previewSlideIndex + direction
        if (next < 0 || next >= slides.length)
            return false                          // at this end of the pick
        root.previewSlideIndex = next             // the pane's preview follows at once
        const piece = (slides[next].parts || [])[0]
        if (piece) {
            root.operatorPiece = { number: Number(piece.number), part: Number(piece.partIndex ?? 0) }
            root.centerRow(piece.number, piece.partIndex ?? 0, true)
        }
        if (publish)
            root.playSlideAt(next)
        return true
    }

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

    property var scrollPiece: null
    property var scrollOutput: null
    property int scrollIndex: 0
    property bool scrollSourced: false
    property var scrollPages: []
    // The chapter passage a Scroll pick reads from, or null where there is none. The
    // single place it is built, for the pick itself and for a later switch to Scroll.
    function scrollPassageFor(number, part) {
        // The active reading tab supplies its own JSON passage and word budget.
        // Other content, such as a show's slides, has no chapter passage to read.
        const reading = root.adapter ? String(root.adapter.contentType || "") : ""
        if (!root.book || !root.adapter || (reading !== "table" && reading !== "scripture"))
            return null
        root.syncReadingWordBudget()   // the walk's budget, read where the walk begins
        return root.readingPagesFromChapter(number, part)
    }
    // EVERY PICK IN SCROLL MODE (a double-click, Play / Enter, a ‹ › stage; Table and
    // Scripture alike) goes out through here: ONE commit that carries the passage,
    // never a slide with the passage following. False where there is no passage to
    // read (Scroll off, or content that has none): the caller sends the plain slide.
    function sendScrollPick(ownerRef, slides, slideIndex, rememberOutput, rememberIndex, number, part) {
        const passage = LiveOutputService.scrollMode
            ? root.scrollPassageFor(Number(number), Number(part)) : null
        if (!passage)
            return false
        LiveOutputService.stagePassagePick(ownerRef, slides, slideIndex,
                                           passage.pages, passage.index)
        root.scrollPages = passage.pages
        root.rememberPick(rememberOutput, rememberIndex, number, part, true)
        return true
    }
    function ensureScrollSource() {
        if (root.scrollSourced || !root.scrollPiece)
            return
        const ours = LiveOutputService.live
            ? LiveOutputService.onAirTitle === root.lineStepOwnerRef
            : LiveOutputService.stagedIsOurs(root.lineStepOwnerRef)
        if (!ours)
            return
        const source = root.scrollPassageFor(root.scrollPiece.number, root.scrollPiece.part)
        if (!source)
            return
        root.scrollSourced = true
        root.lastFollowedScrollChar = -1
        root.scrollPages = source.pages
        LiveOutputService.setReadingTextSource(root.lineStepOwnerRef, source.pages, source.index)
    }
    // passageSourced: the pick already sent its passage together with the slide.
    function rememberPick(output, index, number, part, passageSourced) {
        root.scrollOutput = output
        root.scrollIndex = index
        root.scrollPiece = { number: Number(number), part: Number(part) }
        root.scrollSourced = !!passageSourced
        root.followedScrollPiece = ""
        root.lastFollowedScrollChar = -1
        if (passageSourced)
            return
        // In Scroll, a click shows the chapter text from the clicked paragraph on, in
        // the template's box at the budget size, not the slide's own split.
        if (LiveOutputService.scrollMode)
            root.ensureScrollSource()
        else if (!output)
            pickSetFill.restart()   // slide mode: the set a step needs, after the tile
    }
    function fillPickSet() {
        if (root.scrollOutput || !root.scrollPiece || LiveOutputService.scrollMode
                || !root.book || !root.adapter)
            return
        const set = root.chapterOutputSlides()
        if (set.length === 0)
            return
        const at = set.findIndex((slide) => (slide.parts || []).some(
            (p) => Number(p.number) === Number(root.scrollPiece.number)
                   && Number(p.partIndex ?? 0) === Number(root.scrollPiece.part)))
        // Only if this is still the pick (a second click may have landed meanwhile).
        if (at < 0)
            return
        root.scrollOutput = set
        root.scrollIndex = at
    }
    Timer {
        id: pickSetFill
        interval: 0
        repeat: false
        onTriggered: root.fillPickSet()
    }
    function applyScrollMode() {
        if (!root.scrollPiece)
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
            const stored = !!root.scrollOutput
            const set = root.scrollOutput || root.chapterOutputSlides()
            if (set.length === 0)
                return
            root.scrollOutput = set
            let index = root.scrollIndex
            // A SET BUILT JUST NOW has no remembered index: find the pick's own slide.
            if (!stored && root.scrollPiece) {
                const at = set.findIndex((slide) => (slide.parts || []).some(
                    (p) => Number(p.number) === Number(root.scrollPiece.number)
                           && Number(p.partIndex ?? 0) === Number(root.scrollPiece.part)))
                if (at >= 0)
                    index = at
            }
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
        function onReadingScrollChanged() { root.followScrollWalk() }
        function onReadingPageChanged() { root.followScrollWalk() }
    }

    property string followedScrollPiece: ""
    property int lastFollowedScrollChar: -1
    function followScrollWalk() {
        if (!root.scrollSourced || root.scrollPages.length === 0 || !root.followSelectorMoves)
            return
        const passage = LiveOutputService.readingPassageChar()
        if (passage < 0 || passage === root.lastFollowedScrollChar)
            return
        const origin = Number(root.scrollPages[0].start ?? 0)
        const piece = root.pieceAtChapterOffset(origin + passage)
        if (!piece)
            return
        root.lastFollowedScrollChar = passage
        const sig = piece.number + "|" + piece.partIndex
        if (sig === root.followedScrollPiece)
            return
        EngineBridge.log("info", "ScrollFollow",
            "walk moved to paragraph " + piece.number + " part=" + piece.partIndex
            + " (was " + (root.followedScrollPiece || "none") + ")"
            + " passageChar=" + passage + " pageOrigin=" + origin
            + " chapterOffset=" + (origin + passage)
            + " readingPage=" + LiveOutputService.readingPage + "/" + root.scrollPages.length
            + " selectedBefore=" + JSON.stringify(root.selected))
        root.followedScrollPiece = sig
        root.selected = [piece.number]
        root.anchorVerse = piece.number
        root.centerRow(piece.number, piece.partIndex)
    }

    function playSlideAt(index) {
        const hit = root.outputSlideAt(index)
        if (!hit)
            return false
        root.lineStepOwnerRef = root.referenceText
        const piece = (root.previewSlides[index].parts || [])[0]
        if (!root.sendScrollPick(root.referenceText, hit.output, hit.index, hit.output, hit.index,
                                 piece.number, piece.partIndex ?? 0)) {
            LiveOutputService.stageSlideSet(root.referenceText, hit.output, hit.index)
            root.rememberPick(hit.output, hit.index, piece.number, piece.partIndex ?? 0)
        }
        root.refreshLiveFlowSlide()
        return true
    }

    function stageOutputPart(number, partIndex) {
        if (!root.book || !root.adapter)
            return false
        const pickT0 = Date.now()
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
        if (!pickedSlide)
            return false
        // Scroll is already decided: the pick goes out once, carrying its passage.
        if (!root.sendScrollPick(root.lineStepOwnerRef, [pickedSlide], 0, null, 0, number, part)) {
            LiveOutputService.stageSlideSet(root.lineStepOwnerRef, [pickedSlide], 0)
            root.rememberPick(null, 0, number, part)
        }
        root.refreshLiveFlowSlide()
        const pickMs = Date.now() - pickT0
        if (pickMs >= 80)
            console.log("[PICKCOST] " + kind + " " + number + " staged in " + pickMs
                        + " ms" + (LiveOutputService.scrollMode ? " (scroll)" : " (slide)"))
        return true
    }

    // ---- The wall's ‹ ›: is THIS tab the one that can step the tile? --------
    property string wallStepperId: ""
    function refreshWallStepper() {
        const id = root.adapter && root.adapter.contentType ? root.adapter.contentType : ""
        if (id === "" || !root.adapter.stagedIsOurs) {
            if (root.wallStepperId !== "")
                LiveOutputService.setWallStepper(root.wallStepperId, false)
            return
        }
        root.wallStepperId = id
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
                        PositionHoverArea { id: sourceHoverPA; anchors.fill: parent; checkAncestors: false; showCursor: false }
                        TapHandler { onTapped: { root.column = 0; root.openSource(sourceRow.modelData.id) } }
                    }
                }

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
        visible: !root.compactReferenceSearch
        width: root.compactReferenceSearch ? 0 : root.booksColWidth; height: parent.height
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
            value: booksCol.width; minValue: 50; maxValue: 340
            onDragged: (v) => root.booksColWidth = v
            onResetRequested: root.booksColWidth = 150
        }
    }

    // ---- Chapters --------------------------------------------------------------------------------------------------------
    Rectangle {
        id: chaptersCol
        readonly property bool hasTitles: !!(root.book && root.book.chapterTitles && root.book.chapterTitles.length > 0)
        // Widened once, the first time there turn out to be names to show (a bare Bible chapter number needs far less room than a
        // sermon's title) - after that the user's own drag is what decides, same as every other column here.
        property bool widenedForTitles: false
        visible: !root.compactReferenceSearch
        onHasTitlesChanged: if (hasTitles && !widenedForTitles && !root.chaptersWidthRemembered) { widenedForTitles = true; root.chaptersColWidth = 190 }
        x: booksCol.x + booksCol.width; y: 0
        width: root.compactReferenceSearch ? 0 : root.chaptersColWidth; height: parent.height
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

        ListView {
            id: versesFlick
            objectName: "selfTestVersesList-" + root.paneTag   // (the scroll probe scrolls it by name)
            anchors.fill: parent
            visible: !root.searching && root.citationMatches.length === 0
            clip: true
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
                objectName: "selfTestVerseRow_" + Number(modelData.number)
                            + (continuation ? "-" + partIndex : "")
                required property var modelData
                required property int index
                readonly property bool continuation: modelData.continuation === true
                readonly property int verseNumber: Number(modelData.number)
                readonly property int partIndex: Number(modelData.partIndex ?? 0)
                readonly property bool isSplit: modelData.isSplit === true || continuation
                readonly property bool active: root.rowIsActive(verseNumber, partIndex, isSplit)
                readonly property string oneLine: { root.probeEvals.verse++; return verseRow.modelData.text.replace(/\s*\n+\s*/g, " ") }   // (scroll-cost probe: the row's text binding)
                readonly property bool marked: modelData.marked === true
                readonly property bool hasNote: modelData.hasNote === true
                // The open note editor stretches this one row to make room for itself.
                width: versesFlick.width - 8
                height: continuation ? 34 : (root.noteVerse === verseNumber ? 122 : 38)
                Rectangle {
                    anchors.fill: parent; anchors.margins: 1; radius: 4
                    color: verseRow.active ? Qt.alpha(Theme.accent, 0.24)
                                           : ListRows.stripeOverlay(verseRow.index)
                    border.width: verseRow.active ? 1.5 : 0
                    border.color: verseRow.active ? Qt.alpha(Theme.accentLight, 0.95) : "transparent"
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
                HoverDetail {
                    anchors.fill: parent
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
                    text: root.canHighlight ? root.highlight(verseRow.oneLine) : verseRow.oneLine
                    color: Theme.textPrimary
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                    textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                    font.family: Theme.fontFamily; font.pixelSize: 16
                }
                DragSource {
                    id: verseSource
                    anchors.fill: parent
                    owner: root
                    payloadProvider: () => {
                        const p = root.dragPayload(verseRow.active ? root.selected : [verseRow.verseNumber])
                        return p ? { payload: p, label: p.items[0].name } : null
                    }
                    onActivated: (mods) => {
                        if (typeof verseSource === "undefined" || !verseSource)
                            return
                        const pane = verseSource.owner
                        if (!pane)
                            return
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
                        if (verseRow.isSplit) {
                            pane.stageSubpart(verseRow.verseNumber, verseRow.partIndex)
                            return
                        }
                        pane.selectVerse(verseRow.verseNumber, false, false)
                        pane.stageOutputPart(verseRow.verseNumber, verseRow.partIndex)
                    }
                }
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
                header: Item {
                    width: hitsList.width
                    height: hitsHint.visible ? hitsHint.implicitHeight : 0
                    Text {
                        id: hitsHint
                    objectName: "selfTestPaneSearchHint-" + root.paneTag
                    visible: root.resolvedWords.some((w) => w.changed === true
                                                     || (w.candidates && w.candidates.length > 0))
                    width: parent.width
                    leftPadding: 10
                    color: Theme.highlightInk
                    font.family: Theme.fontFamily; font.pixelSize: 13
                    elide: Text.ElideRight
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
                        readonly property string shown: root.centeredSnippet(
                            hitSnippet.collapsed, width, hitMetrics.advanceWidth)
                        text: root.canHighlight ? root.highlight(shown) : shown
                    }
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

            ListView {
                id: matchesList
                objectName: "selfTestPaneMatchList"   // (the pane-rows scenario scrolls it by name)
                visible: !root.searching && root.citationMatches.length > 0
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
                                root.matchCursor = matchRow.index
                                root.peekSuggestion(matchRow.index)
                            } else {
                                if (root.hoveredMatch === matchRow.index)
                                    root.hoveredMatch = -2
                                peekClear.stop()
                            }
                        }
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

            Item {
                id: peekPanel
                visible: root.peekIndex >= 0 && root.peekVerses.length > 0
                x: parent.width * 0.52 + 8; y: 4
                width: parent.width * 0.48 - 16
                height: parent.height - 8
                clip: true
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
                ListView {
                    id: peekFlick
                    objectName: "selfTestPanePeekList"   // (the pane-rows scenario scrolls it by name)
                    anchors.fill: parent
                    anchors.topMargin: 26
                    anchors.rightMargin: 10
                    clip: true
                    model: root.peekVerses
                    boundsBehavior: Flickable.StopAtBounds
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
            id: referenceChip
            visible: root.currentSource !== null && root.referenceText !== ""
            x: 12; y: parent.height - height - 12
            height: 30
            width: Math.min(referenceTextMetrics.advanceWidth
                             + (versesCol.width > 560
                                ? referenceSourceLabel.implicitWidth + refRow.spacing : 0) + 24,
                             Math.max(60, parent.width - pill.width - 36))
            radius: 15
            color: Qt.alpha(Theme.windowBg, 0.9)
            border.color: Theme.border
            clip: true
            Row {
                id: refRow
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 6
                Text {
                    id: referenceSourceLabel
                    visible: versesCol.width > 560   // a narrow pane keeps just the reference
                    text: (root.currentSource ? root.currentSource.name : "") + ":"
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily; font.pixelSize: 14
                }
                Text {
                    id: referenceText
                    width: Math.max(0, refRow.width
                                    - (referenceSourceLabel.visible
                                       ? referenceSourceLabel.implicitWidth + refRow.spacing : 0))
                    text: root.referenceText
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 14
                    elide: Text.ElideRight
                    clip: true
                }
            }
            TextMetrics {
                id: referenceTextMetrics
                font.family: Theme.fontFamily
                font.pixelSize: 14
                text: root.referenceText
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
                        objectName: "selfTestSearchBox-" + (root.adapter && root.adapter.contentType
                                                            ? root.adapter.contentType : "")
                        // Wider right margin: the chapter lock and the padlock sit
                        // at the field's far end.
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 54
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily; font.pixelSize: 14
                        clip: true
                        onTextChanged: {
                            pillDebounce.restart()
                        }
                        onAccepted: {
                            pillDebounce.stop()
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
                    Item {
                        id: pillScope
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

                Repeater {
                    model: root.searching ? [] : [
                        { glyph: "chevronLeft", dir: -1, tip: qsTr("Previous") },
                        { glyph: "chevronRight", dir: 1, tip: qsTr("Next") }
                    ]
                    delegate: Item {
                        id: stepBtn
                        required property var modelData
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
                    readonly property bool isOurs: root.adapter && root.adapter.liveIsOurs
                                                   ? root.adapter.liveIsOurs(root.lineStepOwnerRef !== "" ? root.lineStepOwnerRef : root.referenceText)
                                                   : false
                    Rectangle { anchors.fill: parent; radius: 13; color: playPickedHover.hovered || parent.isOurs ? Theme.hoverBg : "transparent" }
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
    Rectangle {
        id: previewCol
        anchors.right: parent.right
        visible: !root.compactReferenceSearch
        width: root.compactReferenceSearch ? 0 : root.previewColWidth; height: parent.height
        color: Theme.windowBg

        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

        Flickable {
            id: previewFlick
            anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 1
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
                        DesignPreview {
                            anchors.fill: parent
                            blocks: root.currentPreviewSlide ? root.currentPreviewSlide.blocks : root.preview.blocks
                            background: root.preview.background ?? "transparent"
                            animateLineSteps: LiveOutputService.scrollMode
                            // ITS OWN SURFACE, like every output tile: it measures its own column and
                            // wrap, and turns the shared character position into ITS line the way the
                            // outputs and the engine do (flowscroll::LineForChar). Unnamed, it borrowed
                            // the reference output's line numbers and moved by lines that were not its own.
                            readingStyleId: "reference-pane"
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

                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".verseNumbers" }
                        OptionToggle { settingKey: root.adapter.optionsPrefix + ".splitLongVerses" }
                        Column {
                            visible: root.options[root.adapter.optionsPrefix + ".splitLongVerses"] === true
                            width: parent.width
                            OptionNumber { settingKey: root.adapter.optionsPrefix + ".wordsPerSlide"; step: 5 }
                        }
                        // ---- THE READING STEP (Alt+Down / Alt+Up) ----
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
    property int column: 2
    readonly property bool searchMode: root.allMatches.length > 0 || root.citationMatches.length > 0
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
    function stepInColumn(direction) {
        if (root.searchMode) {
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
            return root.stepChapter(direction, true)   // the end of the book is the end
        return root.step(direction)
    }

    Shortcut { action: "previousPassage"; enabled: root.keysMine; onActivated: root.stepInColumn(-1) }
    Shortcut { action: "nextPassage"; enabled: root.keysMine; onActivated: root.stepInColumn(1) }
    Shortcut {
        action: "cancel"
        enabled: root.keysMine && root.searchReturn !== null && !Hotkeys.canCancel
        onActivated: root.endSearch()
    }
    Shortcut { action: "previousSlide"; enabled: root.keysMine
               onActivated: root.searchMode ? root.stepColumn(-1) : root.stepAndPlay(-1) }
    Shortcut { action: "nextSlide"; enabled: root.keysMine
               onActivated: root.searchMode ? root.stepColumn(1) : root.stepAndPlay(1) }
    Shortcut {
        action: "accept"
        enabled: root.keysMine && !Hotkeys.canAccept
        // WHILE A SEARCH IS UP Enter takes the walked result (and leaves the search);
        // otherwise it stages the pick as it always has.
        onActivated: root.searchMode ? root.takeMatch() : root.playPicked()
    }

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
        const p = root.adapter ? String(root.adapter.optionsPrefix || "") : ""
        if (p !== "" && root.hasRememberedSplit
                && (root.options[p + ".splitLongVerses"] === true) !== root.rememberedSplit) {
            SettingsService.setValue(p + ".splitLongVerses", root.rememberedSplit)
        }
        root.refillTriedFor = ""
        root.refillIfStale()
    }
}