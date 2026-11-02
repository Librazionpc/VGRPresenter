import QtQuick
import VGRPresenterUI

QtObject {
    id: session

    // Wired by EditScreen.
    property var slideModel: null
    property var slideStore: null
    property var canvasHistory: null
    property var askUnsaved: null
    property var _jobs: ({})
    function _awaitJob(kind, token, then) {
        if (!token || token <= 0) {
            if (then)
                then(false, ({}))
            return
        }
        session._jobs[token] = { kind: kind, then: then }
    }
    property Connections _jobResults: Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok, error, payload) {
            const job = session._jobs[token]
            if (job === undefined)
                return
            delete session._jobs[token]
            if (job.then)
                job.then(ok, payload !== undefined && payload !== null ? payload : ({}))
        }
        function onDocumentJobCancelled(token, kind) {
            delete session._jobs[token]
        }
    }

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
            filters: session.slideStore.filtersOf(slideId),
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
            session.slideStore.archiveSlide(a.slideId, s ? s.blocks : [], s ? s.background : "transparent",
                                            s ? s.filters : undefined)
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
            session.slideStore.archiveSlide(a.slideId, s ? s.blocks : [], s ? s.background : "transparent",
                                            s ? s.filters : undefined)
        })
        session._applying = false
        if (session.slideModel.rowCount() > 0)
            session.slideModel.selectSlide(0)
        result.added.forEach((a) => session._rememberFlushed(a.slideId))
        session.canvasHistory.clear()
    }

    function applyTextEditResult(slides) {
        EngineBridge.log("info", "Text edit", "Applying " + slides.length + " returned slide(s) to the edit canvas")
        session._applying = true
        for (const slide of slides) {
            const row = session.slideModel.indexOfEngineId(String(slide.id ?? ""))
            if (row < 0) {
                EngineBridge.log("warning", "Text edit", "Returned slide id was not found in the current slide model: "
                                 + String(slide.id ?? ""))
                continue
            }
            const slideId = session.slideModel.slideIdAt(row)
            const returnedTextItems = (slide.blocks ?? []).filter((block) => block.kind === "text").length
            EngineBridge.log("info", "Text edit", "Replacing canvas slide " + slideId + " from engine slide "
                             + String(slide.id ?? "") + "; returned text items=" + returnedTextItems)
            session.slideStore.replaceSlide(slideId, slide.blocks ?? [], slide.background,
                                            slide.filters)
            delete session._flushed[slideId]
            session._rememberFlushed(slideId)
        }
        session._applying = false
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

    function addItem(kind, x, y, width, height, copyFrom, insertAt) {
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
        const key = session.backend.addBlock(sid, { kind: kind, text: "", x: x, y: y, width: width, height: height },
                                             insertAt === undefined ? -1 : insertAt)
        if (key === "")
            return null
        const item = session.slideStore.createItem(kind, "", x, y, width, height, null, key)
        if (insertAt !== undefined && insertAt >= 0) {
            const items = session.slideStore.current.items.slice()
            items.splice(Math.min(insertAt, items.length), 0, item)
            session.slideStore.current.items = items
        } else {
            session.slideStore.addItem(item)
        }
        return item
    }

    function removeItems(keys) {
        const sid = session.activeEngineId()
        if (sid !== "")
            keys.forEach((k) => session.backend.removeBlock(sid, k))
    }

    // ---- designs: the Edit screen editing an overlay / template --------------------------------------

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
            session.openPath(path)   // the guard has already been asked
        })
    }

    // Opens a library path directly (shows-table row click) — the same
    // unsaved-changes guard as the picker flow.
    function openShowPath(path) {
        session.closeDesign()
        session.guardUnsaved(function () { session.openPath(path) })
    }

    function openPath(path) {
        ShowService.cancelDocumentJobs(["open", "templateLoad"])
        const token = ShowService.openShowFile(path)
        session._awaitJob("open", token, function (ok, payload) {
            if (ok && payload.show)
                session.applyShow(payload.show)
        })
    }

    function saveShow(then) {
        if (session.designMode) {   // a design saves as it goes: Save just makes sure the last edit is in
            session.flushAll()
            if (then)
                then(true, ({}))
            return
        }
        if (!ShowService.hasShow) {
            if (then)
                then(false, ({}))
            return
        }
        session.flushAll()
        if (ShowService.showPath === "")
            return session.saveShowAs(then)
        session._awaitJob("save", ShowService.saveCurrentShow(""), then)
    }

    function saveShowAs(then) {
        if (session.designMode) {
            session.flushAll()
            if (then)
                then(true, ({}))
            return
        }
        session.ensureDocument()
        session.flushAll()
        const path = ShowService.pickShowSavePath(ShowService.showName)
        if (path === "") {
            if (then)
                then(false, ({}))   // the user cancelled the picker
            return
        }
        session._awaitJob("save", ShowService.saveCurrentShow(path), then)
    }
}