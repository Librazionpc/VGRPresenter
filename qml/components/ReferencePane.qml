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

    signal templateEditRequested(string templateId)
    // "Convert to show": the show's name and its slides ([{ title, background, blocks }]), from the engine.
    signal convertToShowRequested(string name, var slides)

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
        return root.book ? root.adapter.preview(root.sourceId, root.book.id, root.chapterNumber, root.selected) : { blocks: [], background: "", reference: "", hasValues: true, slideCount: 0 }
    }
    readonly property string templateId: { root.engineRevision; return root.adapter.templateId() }
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
    Component.onCompleted: root.syncSources()

    // ---- opening things ----

    // Keeps a valid Bible open: the one used last, else the first installed.
    function syncSources() {
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
        const last = String(SettingsService.values["session.referenceSource"] ?? "")
        const known = list.find((b) => b.id === last)
        root.openSource((known ?? list[0]).id)
    }

    function openSource(id) {
        if (id === root.sourceId && root.books.length > 0)
            return
        const previousBook = root.book ? root.book.id : ""
        root.sourceId = id
        root.books = root.adapter.books(id)
        const same = root.books.findIndex((b) => b.id === previousBook)
        root.openBook(same >= 0 ? same : 0, same >= 0 ? root.chapterNumber : 0)
        if (String(SettingsService.values["session.referenceSource"] ?? "") !== id)
            SettingsService.setValue("session.referenceSource", id)
    }

    function openBook(index, chapter) {
        root.bookIndex = Math.max(0, Math.min(index, root.books.length - 1))
        const b = root.book
        if (!b)
            return
        const wanted = chapter > 0 && b.chapters.indexOf(chapter) >= 0 ? chapter : b.chapters[0]
        root.openChapter(wanted, true)
    }

    function openChapter(number, selectFirst) {
        root.chapterNumber = number
        root.chapterVerses = root.book ? root.adapter.chapter(root.sourceId, root.book.id, number) : []
        root.searching = false
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
        const index = root.books.findIndex((b) => b.id === ref.bookId)
        if (index < 0)
            return
        root.openBook(index, ref.chapter > 0 ? ref.chapter : 0)
        if (ref.verseStart > 0) {
            const to = ref.verseEnd > 0 ? ref.verseEnd : ref.verseStart
            const picked = root.chapterVerses.map((v) => v.number).filter((n) => n >= ref.verseStart && n <= to)
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
            }
        }
    }

    onFilterChanged: {
        const ref = root.adapter.resolve(root.filter, root.sourceId)
        if (ref.bookId !== undefined)
            root.goTo(ref)
    }

    readonly property var visibleBooks: {
        const needle = root.filter.trim().toLowerCase()
        if (needle === "" || root.adapter.resolve(root.filter, root.sourceId).bookId !== undefined)
            return root.books
        return root.books.filter((b) => b.name.toLowerCase().indexOf(needle) >= 0)
    }

    function runSearch(text) {
        root.searchResults = text.trim() === "" ? [] : root.adapter.search(text, root.sourceId)
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
    function convertToShow() {
        if (root.selected.length === 0)
            return
        const slides = root.adapter.slides(root.sourceId, root.book.id, root.chapterNumber, root.selected)
        if (slides.length > 0)
            root.convertToShowRequested(root.referenceText, slides)
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
                font.family: Theme.fontFamily; font.pixelSize: 11; font.bold: true
            }
        }

        Flickable {
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
                        color: active ? "#1a1414" : (sourceHover.hovered ? "#16171e" : "transparent")

                        Rectangle { visible: sourceRow.active; x: 0; y: 4; width: 3; height: 21; color: Theme.danger; radius: 1.5 }
                        IconGlyph {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            name: "bookOpen"
                            color: sourceRow.active ? Theme.danger : root.textDim
                            width: 12; height: 12
                        }
                        Text {
                            x: 28; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 34
                            text: sourceRow.modelData.name
                            color: sourceRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12
                        }
                        HoverHandler { id: sourceHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openSource(sourceRow.modelData.id) }
                    }
                }

                Text {
                    visible: root.sources.length === 0
                    x: 4; width: parent.width - 8
                    text: root.adapter.loading() ? (root.adapter ? root.adapter.loadingText : qsTr("Loading…")) : (root.adapter ? root.adapter.emptyText : qsTr("Nothing here yet."))
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }
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
                    font.family: Theme.fontFamily; font.pixelSize: 10
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
            anchors.fill: parent
            clip: true
            contentHeight: booksList.height + 8
            boundsBehavior: Flickable.StopAtBounds

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
                            color: bookRow.active ? "#1e1f28" : (bookHover.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: bookRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.danger }
                        }
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 20
                            text: bookRow.modelData.name
                            color: bookRow.active ? Theme.textPrimary : root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        HoverHandler { id: bookHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openBook(root.books.findIndex((b) => b.id === bookRow.modelData.id), 0) }
                    }
                }
            }
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
        readonly property bool hasTitles: root.book && root.book.chapterTitles && root.book.chapterTitles.length > 0
        // Widened once, the first time there turn out to be names to show (a bare Bible chapter number needs far less room than a
        // sermon's title) - after that the user's own drag is what decides, same as every other column here.
        property bool widenedForTitles: false
        onHasTitlesChanged: if (hasTitles && !widenedForTitles && !root.chaptersWidthRemembered) { widenedForTitles = true; root.chaptersColWidth = 190 }
        x: booksCol.x + booksCol.width; y: 0
        width: root.chaptersColWidth; height: parent.height
        color: "#101118"

        Flickable {
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
                            color: chapterRow.active ? "#1e1f28" : (chapterHover.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: chapterRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.danger }
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
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        HoverHandler { id: chapterHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openChapter(chapterRow.modelData, true) }
                    }
                }
            }
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
            contentHeight: (root.searching ? searchList.height : versesList.height) + 60
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: versesList
                visible: !root.searching
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: root.chapterVerses
                    delegate: Item {
                        id: verseRow
                        required property var modelData
                        readonly property bool active: root.selected.indexOf(modelData.number) >= 0
                        // One line per row, like FreeShow's own verse list (the full paragraph is what the preview pane on the right is
                        // for). A verse spanning several of the sermon's own blank-line paragraphs still carries their break as a literal
                        // "\n" in its text - elide alone does not collapse a real line break, so it is flattened to a space here too.
                        readonly property string oneLine: verseRow.modelData.text.replace(/\s*\n+\s*/g, " ")
                        width: versesList.width; height: 38
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 1; radius: 4
                            color: verseRow.active ? "#2a2b35" : (verseSource.containsMouse ? "#16171e" : "transparent")
                        }
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            width: 40; horizontalAlignment: Text.AlignRight
                            text: verseRow.modelData.number
                            color: Theme.danger
                            font.family: Theme.fontFamily; font.pixelSize: 14; font.bold: true
                        }
                        Text {
                            x: 60; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 70
                            text: verseRow.oneLine
                            color: Theme.textPrimary
                            wrapMode: Text.NoWrap
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 14
                        }
                        // Click selects (Ctrl / Shift add); drag carries the selection into a project; double-click adds it to the open one.
                        DragSource {
                            id: verseSource
                            anchors.fill: parent
                            payload: root.dragPayload(verseRow.active ? root.selected : [verseRow.modelData.number])
                            label: payload ? payload.items[0].name : ""
                            onActivated: {
                                const mods = Qt.application.keyboardModifiers
                                root.selectVerse(verseRow.modelData.number, (mods & Qt.ControlModifier) !== 0, (mods & Qt.ShiftModifier) !== 0)
                            }
                            onOpened: {
                                root.selectVerse(verseRow.modelData.number, false, false)
                                const p = root.dragPayload(root.selected)
                                if (p) ProjectService.dropOnProject(p.kind, p.items)
                            }
                        }
                    }
                }
            }

            Column {
                id: searchList
                visible: root.searching
                x: 0; y: 4; width: parent.width - 8
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
                            font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                        Text {
                            x: 190; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 198
                            text: hitRow.modelData.snippet.replace(/\s*\n+\s*/g, " ")
                            color: root.textDim
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 13
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
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }
        }

        // Nothing installed / nothing loaded yet.
        Text {
            visible: root.sources.length === 0
            anchors.centerIn: parent
            text: root.adapter.loading() ? (root.adapter ? root.adapter.loadingText : qsTr("Loading…")) : (root.adapter ? root.adapter.emptyText : qsTr("Nothing here yet."))
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 13
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
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Text {
                    text: root.referenceText
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 12
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
                        font.family: Theme.fontFamily; font.pixelSize: 12
                        clip: true
                        onAccepted: root.runSearch(text)
                        Keys.onEscapePressed: { root.searching = false }
                    }
                    Text {
                        visible: searchInput.text === ""
                        anchors.verticalCenter: parent.verticalCenter; x: 12
                        text: (root.adapter ? root.adapter.searchPlaceholder : qsTr("Search"))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                }

                Repeater {
                    model: root.searching ? [] : [ { glyph: "chevronUp", dir: -1, tip: "previous" }, { glyph: "chevronDown", dir: 1, tip: "next" } ]
                    delegate: Item {
                        id: stepBtn
                        required property var modelData
                        width: 28; height: 26
                        Rectangle { anchors.fill: parent; radius: 13; color: stepHover.hovered ? "#22242e" : "transparent" }
                        IconGlyph {
                            anchors.centerIn: parent
                            name: stepBtn.modelData.glyph
                            color: Theme.textPrimary
                            width: 12; height: 12
                        }
                        HoverHandler { id: stepHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.step(stepBtn.modelData.dir) }
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
                        width: 12; height: 12
                    }
                    HoverHandler { id: searchHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
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
    }

    // ---- Preview + template + options -------------------------------------------------------------------------------------------
    // The column never scrolls as a whole: the controls (template, Convert to show, the options button) are pinned to the bottom, the
    // preview takes whatever height is left above them (it shrinks to fit instead of pushing them off the screen), and the options -
    // when the round button turns them on - scroll in between.
    Rectangle {
        id: previewCol
        anchors.right: parent.right
        width: root.previewColWidth; height: parent.height
        color: "#101118"

        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

        // ---- the controls, pinned to the bottom ----
        Column {
            id: controls
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 10; anchors.rightMargin: 10; anchors.bottomMargin: 12
            spacing: 10

            // ---- the template (FreeShow's "Template" row, the note about old templates, the two buttons) ----
            Rectangle {
                visible: root.supportsTemplates && !root.optionsOpen
                width: parent.width; height: 56; radius: 6
                color: "#0f1015"; border.color: Theme.border
                clip: true

                Column {
                    x: 12; anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text { text: qsTr("Template"); color: Theme.danger; font.family: Theme.fontFamily; font.pixelSize: 11 }
                    Text {
                        width: 150
                        text: root.adapter.templateName(root.templateId)
                        color: Theme.textPrimary; elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold
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
                visible: root.supportsTemplates && !root.optionsOpen && root.selected.length > 0 && !root.preview.hasValues
                width: parent.width
                text: qsTr("You are using a template with no values!")
                color: Theme.textSecondary; opacity: 0.85
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily; font.pixelSize: 12
            }

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
                    Text { text: btn.text; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 13 }
                    Text { visible: btn.info !== ""; text: btn.info; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 12 }
                }
                HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: btn.clicked() }
            }

            PanelButton {
                visible: root.supportsTemplates && !root.optionsOpen && !root.usingDefaultTemplate && root.selected.length > 0 && !root.preview.hasValues
                text: qsTr("Use default template")
                onClicked: root.adapter.setTemplate("")
            }

            // Convert to show, with the round options button beside it (so it never floats over anything)
            Item {
                width: parent.width; height: 48

                PanelButton {
                    visible: !root.optionsOpen
                    anchors.verticalCenter: parent.verticalCenter
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
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    width: 48; height: 48
                    // a soft shadow: two faint discs under the button
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: width; radius: width / 2; color: "#26000000" }
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: width; radius: width / 2; color: "#33000000" }
                    Rectangle {
                        anchors.fill: parent; radius: width / 2
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: root.optionsOpen ? (optionsHover.hovered ? "#ff6a5c" : "#ff5a4b") : (optionsHover.hovered ? "#2a2d3b" : "#222533") }
                            GradientStop { position: 1.0; color: root.optionsOpen ? "#e23f30" : "#181a24" }
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

        // ---- the slide the engine builds for the picked verses, in the room above the controls ----
        Item {
            id: previewArea
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            height: root.optionsOpen ? Math.min(previewCol.height * 0.34, 190) : Math.max(70, controls.y - 4)

            Rectangle {
                id: previewTile
                anchors.horizontalCenter: parent.horizontalCenter
                y: 8
                // the largest 16:9 that fits both the width and the height that is left
                height: Math.max(48, Math.min((parent.width - 16) * 428 / 754, parent.height - 16))
                width: Math.round(height * 754 / 428)
                color: "#000000"
                clip: true
                DesignPreview {
                    anchors.fill: parent
                    blocks: root.preview.blocks
                    background: (root.preview.background ?? "") === "" || root.preview.background === "transparent" ? "#000000" : root.preview.background
                }
                Text {
                    visible: root.selected.length === 0
                    anchors.centerIn: parent
                    text: qsTr("Pick a verse")
                    color: "#5c6475"
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }
        }

        // ---- the options (the round button turns this on): they scroll between the preview and the button ----
        Flickable {
            id: optionsFlick
            visible: root.supportsTemplates && root.optionsOpen
            anchors.top: previewArea.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: controls.top; anchors.bottomMargin: 8
            clip: true
            contentHeight: optionsBody.height + 8
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: optionsBody
                width: parent.width

                Text {
                    x: 16; height: 26; verticalAlignment: Text.AlignVCenter
                    text: qsTr("SLIDE OPTIONS")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1.2
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
                                    Text { anchors.centerIn: parent; text: "−"; color: Theme.textPrimary; font.pixelSize: 13 }
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
                                    Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 13 }
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
                        OptionNumber { visible: root.options[root.adapter.optionsPrefix + ".smartSplit"] === false; settingKey: root.adapter.optionsPrefix + ".versesPerSlide" }
                    }
                }
            }
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
    // The same "Choose template" popup Settings uses (TemplatePickerModal), not a dropdown: it just gets handed this tab's own
    // template list (the engine's design catalog, `{id, name, color}`) mapped to the picker's `{key, name}` shape instead of the
    // fixed layouts list the picker defaults to.
    function openTemplateMenu() {
        templatePicker.templates = root.adapter.templates().map((t) => ({ key: t.id, name: t.name }))
        templatePicker.selectedKey = root.templateId
        templatePicker.open = true
    }
    TemplatePickerModal {
        id: templatePicker
        z: 30
        contentType: root.sourceId
        contentTypeLabel: root.adapter ? root.adapter.sidebarLabel : ""
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
