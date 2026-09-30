import QtQuick
import VGRPresenterUI

// The Table tab: ReferencePane (the shared scripture-architecture UI) with
// the TheTableService adapter — the sermon library (years = books, sermons =
// chapters, paragraphs = verses). Same UI as Scripture, its own engine
// module, and the SAME template + options chrome (its own "table" templates
// and "table." settings keys).
ReferencePane {
    id: pane

    supportsTemplates: true
    // No "Use default template" here (user call) - The Table's sermon templates aren't scripture-style
    // placeholders with a single built-in fallback the way Scripture's are.
    showUseDefaultTemplateButton: false

    adapter: QtObject {
        id: ad
        // The tab's wording (the shared pane is fully generic).
        readonly property string sidebarLabel: qsTr("Collections")
        readonly property string addLabel: qsTr("New sermon")
        readonly property string loadingText: qsTr("Opening the library…")
        readonly property string emptyText: qsTr("No sermons yet — add one with \"New sermon\".")
        readonly property string searchHint: qsTr("Type words to find in the sermons, then press Enter.")
        readonly property string searchPlaceholder: qsTr("Search in the sermons")
        // The Table's options live under the "table." settings prefix (its own
        // sliders — paragraph numbers, splitting, per-slide counts).
        readonly property string optionsPrefix: "table"
        // GO-LIVE identity (same contract as ScripturePane's adapter).
        readonly property string contentType: "table"
        readonly property string tabLabel: qsTr("The Table")

        readonly property var sources: () => TheTableService.sources()
        readonly property var loading: () => TheTableService.loading
        // The Table's template hooks: its own setting key and its own
        // "table"-type layouts in the SHARED template library.
        readonly property var templateId: () => TheTableService.templateId()
        readonly property var defaultTemplateId: () => TheTableService.defaultTemplateId()
        readonly property var templateName: (id) => TheTableService.templateName(id)
        readonly property var templates: () => TheTableService.templates()
        readonly property var setTemplate: (id) => TheTableService.setTemplate(id)
        readonly property var books: (id) => TheTableService.books(id)
        readonly property var chapter: (src, bookId, n) => TheTableService.chapter(bookId, n)
        readonly property var reference: (book, ch, nums) => TheTableService.reference(book, ch, nums)
        readonly property var resolve: (text, srcId) => TheTableService.resolve(text)
        readonly property var search: (text, srcId) => TheTableService.search(text)
        // ASYNC pill search: the pane prefers this when present (token-guarded,
        // answers via TheTableService.searchResultsReady -> the pane's Connections).
        readonly property var searchAsync: (text, srcId, token) => TheTableService.searchAsync(text, 60, token)
        readonly property var preview: (src, bookId, ch, nums) => TheTableService.preview(bookId, ch, nums)
        // The picked paragraphs as show slides, template-split by the tab's options.
        readonly property var slides: (src, bookId, ch, nums) => TheTableService.slides(bookId, ch, nums)
        // GO LIVE (FreeShow's playScripture): the picked sermon on air, gated on
        // the style's table pill. Title match = ours (the play glyph turns green).
        readonly property var goLive: (name, slides) => LiveOutputService.goLiveWithSlides(name, slides)
        readonly property var liveIsOurs: (refText) => LiveOutputService.live
                                             && refText !== "" && LiveOutputService.onAirTitle === refText
        readonly property var importNew: () => TheTableService.newSermon()
        // The bulk action: pick the sermons root once, every .pdf/.txt under
        // it lands in the library (worker thread, live progress, dedup on).
        readonly property string addFolderLabel: qsTr("Add sermons folder")
        readonly property var importFolder: () => TheTableService.newSermonFolder()
        readonly property var importing: () => TheTableService.importing
        readonly property var progress: () => TheTableService.progress
        // ---- User data (notes & highlights, persisted in the library JSON) ----
        // The SAME id-shaped surface ScripturePane's adapter answers, so the
        // shared pane's stars/pencils/drawer serve both tabs. Reads pass the
        // book's engine id straight through (the year-book grammar lives in
        // TheTableService, once); writes take the pane's display reference
        // ("1953 12:3" or a citation) and the service resolves it.
        readonly property var chapterUserData: (src, bookId, ch) => TheTableService.chapterUserData(bookId, ch)
        readonly property var setNote: (reference, text) => TheTableService.setNote(reference, text)
        readonly property var setHighlighted: (reference, on) => TheTableService.setHighlighted(reference, on)
        readonly property var notes: () => TheTableService.notes()
        readonly property var userData: ({
            chapterUserData: ad.chapterUserData,
            setNote: ad.setNote,
            setHighlighted: ad.setHighlighted,
            notes: ad.notes
        })
        // The shared pane re-syncs when this adapter signal fires (see below).
        // (User-data notice is a revision counter, NOT a signal: the userData
        // property already owns the change signal name `userDataChanged` — an
        // explicit signal of that name is a QML duplicate-signal error.)
        signal changed()
        property int userDataRevision: 0
    }

    // Service -> adapter: the pane's own Connections re-syncs it.
    Connections {
        target: TheTableService
        function onChanged() { pane.adapter.changed() }
        function onUserDataChanged() { pane.adapter.userDataRevision++ }
        // The async pill search's answer (searchAsync's token guard lives in
        // the pane — applySearchResults drops stale tokens).
        function onSearchResultsReady(token, rows) { pane.applySearchResults(token, rows) }
    }

    // GO LIVE: pane -> adapter -> live service (same path Scripture uses).
    onGoLiveRequested: (name, slides) => pane.adapter.goLive(name, slides)
}
