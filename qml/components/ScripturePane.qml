import QtQuick
import VGRPresenterUI

ReferencePane {
    id: pane

    supportsTemplates: true

    adapter: QtObject {
        id: ad
        // The tab's wording (the shared pane is fully generic).
        readonly property string sidebarLabel: qsTr("Bibles")
        readonly property string addLabel: qsTr("New scripture")
        readonly property string loadingText: qsTr("Reading the Bibles…")
        readonly property string emptyText: qsTr("No Bible is installed yet.")
        readonly property string searchHint: qsTr("Type words to find in this Bible, then press Enter.")
        readonly property string searchPlaceholder: qsTr("Search in this Bible")
        // Scripture's options live under the "scripture." settings prefix.
        readonly property string optionsPrefix: "scripture"
        // GO-LIVE identity: the content kind this tab produces (checked against
        // the active output's style pills) and its display name for toasts.
        readonly property string contentType: "scripture"
        readonly property string tabLabel: qsTr("Scripture")
        readonly property string sessionSourceKey: "session.scriptureBible"
        // WHERE THE READER WAS: the open Bible, book, chapter, picked verses and the
        // row at the top of the list, as one JSON value (ReferencePane's readingKey).
        readonly property string readingKey: "session.scriptureReading"
        readonly property var setTemplate: (id) => SettingsService.setValue("scripture.template", id)

        readonly property var sources: () => ScriptureService.bibles
        readonly property var loading: () => ScriptureService.loading
        readonly property var books: (id) => ScriptureService.books(id)
        readonly property var chapter: (src, bookId, n) => ScriptureService.chapter(src, bookId, n)
        readonly property var chapterAsync: (src, bookId, n, token) => ScriptureService.chapterAsync(src, bookId, n, token)
        readonly property var reference: (book, ch, nums) => ScriptureService.reference(book, ch, nums)
        readonly property var resolve: (text, srcId) => ScriptureService.resolve(text, srcId)
        readonly property bool supportsChapterLock: true
        readonly property var search: (text, srcId, exact, bookId, chapter) => ScriptureService.search(text, srcId, 60, exact, bookId, chapter)
        // ASYNC pill search: the pane prefers this when present (token-guarded,
        // answers via ScriptureService.searchResultsReady -> the pane's Connections).
        readonly property var searchAsync: (text, srcId, token, exact, bookId, chapter) => ScriptureService.searchAsync(text, srcId, 60, token, exact, bookId, chapter)
        readonly property var preview: (src, bookId, ch, nums) => ScriptureService.preview(src, bookId, ch, nums)
        readonly property var slides: (src, bookId, ch, nums) => ScriptureService.slides(src, bookId, ch, nums)
        readonly property var goLive: (name, slides) => LiveOutputService.stageSlides(name, slides)
        readonly property var liveIsOurs: (refText) => LiveOutputService.live
                                             && refText !== "" && LiveOutputService.onAirTitle === refText
        readonly property var stagedIsOurs: (refText) => LiveOutputService.stagedIsOurs(refText)
        readonly property var importNew: () => ScriptureService.importBible()
        // No bulk folder action on Scripture — Bibles install one JSON at a time.
        readonly property string addFolderLabel: ""
        readonly property var importFolder: null
        readonly property var importing: () => SearchService.bibleImporting
        readonly property var progress: () => SearchService.bibleProgress
        readonly property var templateId: () => ScriptureService.templateId()
        readonly property var templateName: (id) => ScriptureService.templateName(id)
        readonly property var defaultTemplateId: () => ScriptureService.defaultTemplateId()
        readonly property var templates: () => ScriptureService.templates()
        // ---- User data (notes & highlights, the engine's §User Data store) ----
        readonly property var chapterUserData: (src, bookId, ch) => ScriptureService.chapterUserData(src, bookId, ch)
        readonly property var setNote: (src, reference, text) => ScriptureService.setNote(src, reference, text)
        readonly property var setHighlighted: (src, reference, on) => ScriptureService.setHighlighted(src, reference, on)
        readonly property var notes: (src) => ScriptureService.notes(src)
        readonly property var userData: ({
            chapterUserData: ad.chapterUserData,
            setNote: ad.setNote,
            setHighlighted: ad.setHighlighted,
            notes: ad.notes
        })
        signal changed()
        signal searchCached(string text)
        property int userDataRevision: 0
    }

    // Service -> adapter: the pane's own Connections re-syncs it.
    Connections {
        target: ScriptureService
        function onChanged() { pane.adapter.changed() }
        function onUserDataChanged() { pane.adapter.userDataRevision++ }
        // The async pill search's answer (searchAsync's token guard lives in
        // the pane — applySearchResults drops stale tokens).
        function onSearchResultsReady(token, rows) { pane.applySearchResults(token, rows) }
        function onChapterCached(bibleId, bookId, chapter) { pane.corpusCacheLanded() }
        function onReferenceCached(bibleId, text) { pane.corpusCacheLanded() }
        // The sync search()'s worker answer (the pane's fallback path re-asks on it).
        function onSearchCached(text) { pane.adapter.searchCached(text) }
    }

    // GO LIVE: pane -> adapter -> live service.
    onGoLiveRequested: (name, slides) => pane.adapter.goLive(name, slides)
}