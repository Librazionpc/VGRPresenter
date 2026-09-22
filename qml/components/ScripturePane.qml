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
        // The tab's wording (the shared pane is fully generic).
        readonly property string sidebarLabel: qsTr("Bibles")
        readonly property string addLabel: qsTr("New scripture")
        readonly property string loadingText: qsTr("Reading the Bibles…")
        readonly property string emptyText: qsTr("No Bible is installed yet.")
        readonly property string searchHint: qsTr("Type words to find in this Bible, then press Enter.")
        readonly property string searchPlaceholder: qsTr("Search in this Bible")
        // Scripture's options live under the "scripture." settings prefix.
        readonly property string optionsPrefix: "scripture"
        readonly property var setTemplate: (id) => SettingsService.setValue("scripture.template", id)

        readonly property var sources: () => ScriptureService.bibles
        readonly property var loading: () => ScriptureService.loading
        readonly property var books: (id) => ScriptureService.books(id)
        readonly property var chapter: (src, bookId, n) => ScriptureService.chapter(src, bookId, n)
        readonly property var reference: (book, ch, nums) => ScriptureService.reference(book, ch, nums)
        readonly property var resolve: (text, srcId) => ScriptureService.resolve(text, srcId)
        readonly property var search: (text, srcId) => ScriptureService.search(text, srcId)
        readonly property var preview: (src, bookId, ch, nums) => ScriptureService.preview(src, bookId, ch, nums)
        readonly property var slides: (src, bookId, ch, nums) => ScriptureService.slides(src, bookId, ch, nums)
        readonly property var importNew: () => ScriptureService.importBible()
        // No bulk folder action on Scripture — Bibles install one JSON at a time.
        readonly property string addFolderLabel: ""
        readonly property var importFolder: null
        readonly property var templateId: () => ScriptureService.templateId()
        readonly property var templateName: (id) => ScriptureService.templateName(id)
        readonly property var defaultTemplateId: () => ScriptureService.defaultTemplateId()
        readonly property var templates: () => ScriptureService.templates()
        // The shared pane re-syncs when this adapter signal fires (see below).
        signal changed()
    }

    // Service -> adapter: the pane's own Connections re-syncs it.
    Connections {
        target: ScriptureService
        function onChanged() { pane.adapter.changed() }
    }

    onTemplateEditRequested: (id) => pane.templateEditRequested(id)
}
