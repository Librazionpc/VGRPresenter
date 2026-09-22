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

    adapter: QtObject {
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
        readonly property var preview: (src, bookId, ch, nums) => TheTableService.preview(bookId, ch, nums)
        // The picked paragraphs as show slides, template-split by the tab's options.
        readonly property var slides: (src, bookId, ch, nums) => TheTableService.slides(bookId, ch, nums)
        readonly property var importNew: () => TheTableService.newSermon()
        // The bulk action: pick the sermons root once, every .pdf/.txt under
        // it lands in the library (worker thread, live progress, dedup on).
        readonly property string addFolderLabel: qsTr("Add sermons folder")
        readonly property var importFolder: () => TheTableService.newSermonFolder()
        readonly property var importing: () => TheTableService.importing
        readonly property var progress: () => TheTableService.progress
        // The shared pane re-syncs when this adapter signal fires (see below).
        signal changed()
    }

    // Service -> adapter: the pane's own Connections re-syncs it.
    Connections {
        target: TheTableService
        function onChanged() { pane.adapter.changed() }
    }
}
