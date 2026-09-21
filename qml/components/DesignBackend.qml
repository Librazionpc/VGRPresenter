import QtQuick
import VGRPresenterUI

// The engine seam of the Edit screen while it edits a DESIGN - one overlay or template from a library - instead of
// the open show. ShowSession makes the same calls on its `backend` whichever it is:
//
//   a SHOW      -> ShowService (slides with blocks in a .vgr document)
//   a DESIGN    -> this object: the design is shown as a show with ONE slide whose blocks are the design's blocks, and
//                  every call is handed to the library service (bps::library::DesignLibrary in the engine), which
//                  applies ShowEditor's block rules and saves. A design has no save button: an edit that settles is
//                  written straight away, like FreeShow's overlay editor.
//
// So the Edit screen edits a design with exactly the code it edits a slide with - canvas, undo, inspector, text panel -
// and only this file knows the difference. The engine issues every block id; the UI never invents one.
QtObject {
    id: backend

    // OverlayLibraryService | TemplateLibraryService
    property var service: null
    property string designId: ""

    // Bumped when the library changes, so currentShow is read afresh.
    property int revision: 0
    property Connections watch: Connections {
        target: backend.service
        function onChanged() { backend.revision++ }
    }

    // The ShowService surface ShowSession uses.
    readonly property bool hasShow: designId !== ""
    readonly property bool showDirty: false   // designs save as they go: nothing is ever "unsaved"

    // { slides: [ slide ] } - the design as the one slide the canvas edits.
    readonly property var currentShow: {
        const _ = backend.revision
        if (!backend.service || backend.designId === "")
            return ({ slides: [] })
        const d = backend.service.design(backend.designId)
        if (d.id === undefined)
            return ({ slides: [] })
        return {
            id: d.id,
            name: d.name,
            slides: [{
                id: d.id, title: d.name, tag: "", tagColor: "", line1: "", line2: "", ref: "", categoryId: "",
                background: d.background, blocks: d.blocks
            }]
        }
    }

    function ensureShow(name) { }          // the design already exists
    function markShowDirty() { }

    // What the canvas settled on: its background and blocks (the slide's other fields mean nothing here).
    function updateSlide(slideId, patch) {
        return backend.service.setDesignBlocks(backend.designId, patch.background, patch.blocks)
    }

    // A design is one canvas: there are no slides to add, copy or remove.
    function addSlide(spec) { return "" }
    function duplicateSlide(slideId) { return "" }
    function removeSlide(slideId) { return false }

    function addBlock(slideId, spec) { return backend.service.addBlock(backend.designId, spec) }
    function duplicateBlock(slideId, key) { return backend.service.duplicateBlock(backend.designId, key) }
    function blockOf(slideId, key) { return backend.service.block(backend.designId, key) }
    function removeBlock(slideId, key) { return backend.service.removeBlock(backend.designId, key) }
}
