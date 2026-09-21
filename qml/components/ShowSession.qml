import QtQuick
import VGRPresenterUI

// The seam between the Edit screen's UI state and the ENGINE that owns the show.
//
// Who owns what:
//   * The ENGINE (ShowService -> ShowEditor/PresentationDocument) owns the show:
//     which slides exist and in what order, which items are on each slide,
//     duplicate/delete, ids, and the .vgr file. Every structural change is asked
//     of the engine first.
//   * The UI (SlideListModel + SlideCanvasStore) is a PROJECTION of that, plus the
//     live gesture state (an item being dragged, text being typed) that is too
//     fine-grained to send to the engine per frame. That live state is FLUSHED to
//     the engine when it settles (see flushSlide) and always before anything that
//     needs the engine to be current: a structural change, a slide switch, Save.
//
// One rule keeps them honest: the engine issues every id (slides and items). The
// UI never invents one, so the two can never disagree about identity.
QtObject {
    id: session

    // Wired by EditScreen.
    property var slideModel: null
    property var slideStore: null
    property var canvasHistory: null
    // function(then) — asks "save / don't save / cancel?" when the show has unsaved
    // changes, and calls `then` if the user lets us proceed. Set by Main.qml (the
    // dialog lives there so it can appear over any screen). Unset = proceed.
    property var askUnsaved: null

    // What the engine calls go to. Normally the open SHOW (ShowService); while the Edit screen is editing a DESIGN (an
    // overlay or a template from a library) it is that design (DesignBackend), which answers the same calls. Every slide and
    // item action below goes through this, so the screen edits a design with exactly the code it edits a slide with.
    property var backend: ShowService
    // Set while a design is open (see openDesign).
    property bool designMode: false
    property string designKind: ""        // "overlay" | "template"
    property string designId: ""
    property var designService: null
    property DesignBackend designBackend: DesignBackend { }
    // The show slide that was open before a design was, to come back to it.
    property string _slideBeforeDesign: ""

    // What was last sent to the engine per slide (UI slide id -> signature), so a
    // flush of an unchanged slide is a no-op and never marks the show modified.
    property var _flushed: ({})
    // True while the UI is being rebuilt from the engine (open / new / refresh):
    // flushes are suppressed so half-built state is never written back.
    property bool _applying: false

    // ---- helpers ---------------------------------------------------------------

    function rowForSlideId(slideId) {
        const n = session.slideModel.rowCount()
        for (let i = 0; i < n; ++i)
            if (session.slideModel.slideIdAt(i) === slideId)
                return i
        return -1
    }

    function activeRow() {
        return session.rowForSlideId(session.slideModel.activeSlideId)
    }

    function activeEngineId() {
        const row = session.activeRow()
        return row < 0 ? "" : session.slideModel.engineIdAt(row)
    }

    // Everything the UI edits on a slide, in the engine's patch shape.
    function slidePatch(slideId) {
        const row = session.rowForSlideId(slideId)
        if (row < 0)
            return null
        return Object.assign({}, session.slideModel.slideFieldsAt(row), {
            background: session.slideStore.backgroundOf(slideId).toString(),
            blocks: session.slideStore.blocksOf(slideId)
        })
    }

    function ensureDocument() {
        session.backend.ensureShow(qsTr("Untitled show"))
    }

    // ---- UI -> engine ----------------------------------------------------------

    // Sends one slide's current content to the engine. Skipped when it has not
    // changed since the last send. Returns false only if the engine refused.
    function flushSlide(slideId) {
        if (session._applying || slideId === undefined || slideId < 0)
            return true
        const row = session.rowForSlideId(slideId)
        if (row < 0)
            return true
        const eid = session.slideModel.engineIdAt(row)
        if (eid === "")
            return true
        const patch = session.slidePatch(slideId)
        const signature = JSON.stringify(patch)
        if (session._flushed[slideId] === signature)
            return true
        if (!session.backend.updateSlide(eid, patch))
            return false
        session._flushed[slideId] = signature
        return true
    }

    function flushAll() {
        const n = session.slideModel.rowCount()
        for (let i = 0; i < n; ++i)
            session.flushSlide(session.slideModel.slideIdAt(i))
    }

    // The canvas settled (an edit was committed, or undo/redo ran): flag the show
    // modified now, and send the slide once things go quiet.
    function scheduleFlush() {
        if (session._applying)
            return
        session.backend.markShowDirty()
        flushTimer.restart()
    }

    property Timer flushTimer: Timer {
        interval: 700
        onTriggered: session.flushSlide(session.slideStore.activeSlideId)
    }

    // ---- engine -> UI ----------------------------------------------------------

    function _rememberFlushed(slideId) {
        const patch = session.slidePatch(slideId)
        if (patch)
            session._flushed[slideId] = JSON.stringify(patch)
    }

    // Rebuilds the slide list (and per-slide canvases of NEW slides) from the
    // engine after a structural change. Slides that still exist keep their live
    // UI state. `selectEngineId` = which slide to show afterwards ("" = keep the
    // current one); `fallbackIndex` = where to land if the current one was deleted.
    function refresh(selectEngineId, fallbackIndex) {
        session._applying = true
        const show = session.backend.currentShow
        const result = session.slideModel.syncFromEngine(show.slides)
        result.removed.forEach((id) => {
            session.slideStore.dropSlide(id)
            delete session._flushed[id]
        })
        result.added.forEach((a) => {
            const s = show.slides.find((x) => x.id === a.engineId)
            session.slideStore.archiveSlide(a.slideId, s ? s.blocks : [], s ? s.background : "transparent")
        })
        session._applying = false

        const count = session.slideModel.rowCount()
        let target = selectEngineId ? session.slideModel.indexOfEngineId(selectEngineId) : -1
        if (target < 0 && result.activeRemoved && count > 0)
            target = Math.max(0, Math.min(fallbackIndex !== undefined ? fallbackIndex : 0, count - 1))
        if (target >= 0)
            session.slideModel.selectSlide(target)
        result.added.forEach((a) => session._rememberFlushed(a.slideId))
    }

    // Replaces the whole UI with an opened show's content.
    function applyShow(show) {
        session._applying = true
        session.canvasHistory.abandonPending()
        session.slideStore.clear()
        session.slideModel.clear()
        session._flushed = ({})
        const result = session.slideModel.syncFromEngine(show.slides)
        result.added.forEach((a) => {
            const s = show.slides.find((x) => x.id === a.engineId)
            session.slideStore.archiveSlide(a.slideId, s ? s.blocks : [], s ? s.background : "transparent")
        })
        session._applying = false
        if (session.slideModel.rowCount() > 0)
            session.slideModel.selectSlide(0)
        result.added.forEach((a) => session._rememberFlushed(a.slideId))
        session.canvasHistory.clear()
    }

    // ---- slide actions (all engine-first) -----------------------------------------

    function addSlide() {
        session.ensureDocument()
        session.flushSlide(session.slideStore.activeSlideId)
        const eid = session.backend.addSlide({})
        if (eid !== "")
            session.refresh(eid)
    }

    function duplicateSlide(index) {
        const eid = session.slideModel.engineIdAt(index)
        if (eid === "")
            return
        session.flushSlide(session.slideModel.slideIdAt(index))   // copy what is on screen
        if (session.backend.duplicateSlide(eid) !== "")
            session.refresh("")
    }

    function removeSlide(index) {
        const eid = session.slideModel.engineIdAt(index)
        if (eid === "")
            return
        if (session.backend.removeSlide(eid))
            session.refresh("", index)
    }

    // ---- canvas item actions (engine-first) ----------------------------------------

    // Adds an item to the active slide; the ENGINE assigns its id. `copyFrom` =
    // duplicate that item (engine copy, offset). Returns the live CanvasItem, or
    // null if the engine refused (no slide / no show).
    function addItem(kind, x, y, width, height, copyFrom) {
        const sid = session.activeEngineId()
        if (sid === "")
            return null
        if (copyFrom) {
            session.flushSlide(session.slideStore.activeSlideId)
            const key = session.backend.duplicateBlock(sid, copyFrom.key)
            if (key === "")
                return null
            const block = session.backend.blockOf(sid, key)
            const item = session.slideStore.itemFromBlock(block)
            session.slideStore.addItem(item)
            session._rememberFlushed(session.slideStore.activeSlideId)
            return item
        }
        const key = session.backend.addBlock(sid, { kind: kind, text: "", x: x, y: y, width: width, height: height })
        if (key === "")
            return null
        const item = session.slideStore.createItem(kind, "", x, y, width, height, null, key)
        session.slideStore.addItem(item)
        return item
    }

    function removeItems(keys) {
        const sid = session.activeEngineId()
        if (sid !== "")
            keys.forEach((k) => session.backend.removeBlock(sid, k))
    }

    // ---- designs: the Edit screen editing an overlay / template --------------------------------------

    // Opens a design from a library (kind "overlay" | "template") on the canvas. The show that was open is sent to the
    // engine first and comes back untouched when the design is closed. Opening another design while one is open just
    // saves the first.
    function openDesign(kind, id) {
        const service = kind === "template" ? TemplateLibraryService : OverlayLibraryService
        if (service.design(id).id === undefined)
            return false
        session.flushAll()   // the show's (or the previous design's) on-screen state goes to the engine
        if (!session.designMode)
            session._slideBeforeDesign = session.activeEngineId()
        session.designBackend.service = service
        session.designBackend.designId = id
        session.designService = service
        session.designKind = kind
        session.designId = id
        session.backend = session.designBackend
        session.designMode = true
        session.applyShow(session.backend.currentShow)
        return true
    }

    // Leaves the design (its edits are already saved) and puts the show back on the canvas. No-op when no design is open.
    function closeDesign() {
        if (!session.designMode)
            return
        session.flushAll()
        session.designMode = false
        session.backend = ShowService
        session.designBackend.designId = ""
        session.designService = null
        session.designKind = ""
        session.designId = ""
        if (ShowService.hasShow) {
            session.applyShow(ShowService.currentShow)
            const back = session._slideBeforeDesign !== "" ? session.slideModel.indexOfEngineId(session._slideBeforeDesign) : -1
            if (back >= 0)
                session.slideModel.selectSlide(back)
        } else {
            session.applyShow({ slides: [] })   // nothing open: an empty canvas, as at launch
        }
        session._slideBeforeDesign = ""
    }

    // ---- show document actions ------------------------------------------------------

    function guardUnsaved(then) {
        if (!ShowService.hasShow || !ShowService.showDirty || !session.askUnsaved) {
            then()
            return
        }
        session.askUnsaved(then)
    }

    // "New show": a fresh engine document with one empty slide.
    function newShow() {
        session.closeDesign()
        session.guardUnsaved(function () {
            session._applying = true
            session.canvasHistory.abandonPending()
            ShowService.newShowDocument(qsTr("Untitled show"))
            session.slideStore.clear()
            session.slideModel.clear()
            session._flushed = ({})
            session._applying = false
            const eid = session.backend.addSlide({})
            if (eid !== "")
                session.refresh(eid)
            ShowService.markShowClean()   // nothing worth prompting about yet
            session.canvasHistory.clear()
        })
    }

    function openShow() {
        session.closeDesign()
        session.guardUnsaved(function () {
            const path = ShowService.pickShowToOpen()
            if (path === "")
                return
            const result = ShowService.openShowFile(path)
            if (result.ok)
                session.applyShow(result.show)
        })
    }

    // Opens a library path directly (shows-table row click) — the same
    // unsaved-changes guard as the picker flow.
    function openShowPath(path) {
        session.closeDesign()
        session.guardUnsaved(function () {
            const result = ShowService.openShowFile(path)
            if (result.ok)
                session.applyShow(result.show)
        })
    }

    // Returns true when the show ended up saved.
    function saveShow() {
        if (session.designMode) {   // a design saves as it goes: Save just makes sure the last edit is in
            session.flushAll()
            return true
        }
        if (!ShowService.hasShow)
            return false
        session.flushAll()
        if (ShowService.showPath === "")
            return session.saveShowAs()
        return ShowService.saveCurrentShow("")
    }

    function saveShowAs() {
        if (session.designMode) {
            session.flushAll()
            return true
        }
        session.ensureDocument()
        session.flushAll()
        const path = ShowService.pickShowSavePath(ShowService.showName)
        if (path === "")
            return false
        return ShowService.saveCurrentShow(path)
    }
}
