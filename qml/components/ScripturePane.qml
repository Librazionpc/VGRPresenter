import QtQuick
import VGRPresenterUI

// The Scripture tab: ReferencePane (the shared scripture-architecture UI)
// with the ScriptureService adapter. All data, search and slide-building
// belong to the engine; this only binds the two together and carries the
// Scripture-specific wording + chrome switches.
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
        // REGISTERED settings key remembering the last-opened Bible across
        // sessions (the old code wrote "session.referenceSource", which was
        // never declared — every open toasted "there is no setting …").
        readonly property string sessionSourceKey: "session.scriptureBible"
        readonly property var setTemplate: (id) => SettingsService.setValue("scripture.template", id)

        readonly property var sources: () => ScriptureService.bibles
        readonly property var loading: () => ScriptureService.loading
        readonly property var books: (id) => ScriptureService.books(id)
        readonly property var chapter: (src, bookId, n) => ScriptureService.chapter(src, bookId, n)
        readonly property var reference: (book, ch, nums) => ScriptureService.reference(book, ch, nums)
        readonly property var resolve: (text, srcId) => ScriptureService.resolve(text, srcId)
        readonly property var search: (text, srcId) => ScriptureService.search(text, srcId)
        // ASYNC pill search: the pane prefers this when present (token-guarded,
        // answers via ScriptureService.searchResultsReady -> the pane's Connections).
        readonly property var searchAsync: (text, srcId, token) => ScriptureService.searchAsync(text, srcId, 60, token)
        readonly property var preview: (src, bookId, ch, nums) => ScriptureService.preview(src, bookId, ch, nums)
        readonly property var slides: (src, bookId, ch, nums) => ScriptureService.slides(src, bookId, ch, nums)
        // A picked passage STAGES (Main Output tile preview only) — the
        // SAME gate ShowCenter.qml's own double-click pick already obeys:
        // nothing reaches the real output until the top GO LIVE button
        // actually commits it (live report: "media, overlay, scripture,
        // the table all need to be aware of the go live gate — they can't
        // just push live"). Gated on the style's scripture pill first.
        readonly property var goLive: (name, slides) => LiveOutputService.stageSlides(name, slides)
        // Is THIS tab's content what's on air right now? (the play button's
        // refresh glyph — FreeShow's isActiveInOutput.) The on-air title of a
        // scripture pick IS the reference text, so a title match reads "ours".
        readonly property var liveIsOurs: (refText) => LiveOutputService.live
                                             && refText !== "" && LiveOutputService.onAirTitle === refText
        readonly property var importNew: () => ScriptureService.importBible()
        // No bulk folder action on Scripture — Bibles install one JSON at a time.
        readonly property string addFolderLabel: ""
        readonly property var importFolder: null
        // The import progress bar (the same one The Table's folder import
        // feeds): importing() shows the bar, progress() { done, total, current }
        // moves it — verses indexed so far / verses in the file. The state
        // lives on SearchService (it owns the Bible import worker); reading it
        // inside these functions is what lets the pane's bindings track the
        // service's notify signals.
        readonly property var importing: () => SearchService.bibleImporting
        readonly property var progress: () => SearchService.bibleProgress
        readonly property var templateId: () => ScriptureService.templateId()
        readonly property var templateName: (id) => ScriptureService.templateName(id)
        readonly property var defaultTemplateId: () => ScriptureService.defaultTemplateId()
        readonly property var templates: () => ScriptureService.templates()
        // ---- User data (notes & highlights, the engine's §User Data store) ----
        // Reads are ID-SHAPED and BATCHED (one chapterUserData call per open
        // chapter — no reference synthesis, no per-row resolution in JS);
        // writes take the pane's display reference, which the ENGINE resolves
        // (the reference grammar lives engine-side, once).
        readonly property var chapterUserData: (src, bookId, ch) => ScriptureService.chapterUserData(src, bookId, ch)
        readonly property var setNote: (reference, text) => ScriptureService.setNote(src(), reference, text)
        readonly property var setHighlighted: (reference, on) => ScriptureService.setHighlighted(src(), reference, on)
        readonly property var notes: () => ScriptureService.notes(src())
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
        target: ScriptureService
        function onChanged() { pane.adapter.changed() }
        function onUserDataChanged() { pane.adapter.userDataRevision++ }
        // The async pill search's answer (searchAsync's token guard lives in
        // the pane — applySearchResults drops stale tokens).
        function onSearchResultsReady(token, rows) { pane.applySearchResults(token, rows) }
    }

    // GO LIVE: pane -> adapter -> live service.
    onGoLiveRequested: (name, slides) => pane.adapter.goLive(name, slides)
    // (templateEditRequested needs NO re-emit here: the host's handler on this
    // pane instance receives the signal directly — a self-emit recursion
    // ("Maximum call stack size exceeded") used to kill every pen click
    // before the Edit screen ever opened.)
}
