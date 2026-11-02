import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../shortcuts"
import "../../components"

Rectangle {
    id: root

    clip: true
    color: Theme.panelBg

    // The editor keeps a usable center workspace as the window narrows:
    // side panels yield width first, then the stage preview scales to fit.
    readonly property real slidePanelWidth: Math.min(280, Math.max(160, width * 0.195))
    readonly property real inspectorPanelWidth: Math.min(400, Math.max(240, width * 0.278))
    readonly property real centerRegionWidth: Math.max(0, width - slidePanelWidth - inspectorPanelWidth)
    readonly property real canvasFitScale: Math.max(0.3, Math.min(1,
        StageGrid.fitScale(centerRegionWidth - 20, height - 160)))

    SlideListModel {
        id: slideModel
    }

    SlideCanvasStore {
        id: slideStore
    }

    ShowSession {
        id: showSession
        slideModel: slideModel
        slideStore: slideStore
        canvasHistory: canvasHistory
    }
    readonly property var session: showSession

    // ---- Designs: the same screen edits an overlay or a template from a library ----
    readonly property bool designMode: showSession.designMode
    // Opens design `id` of `kind` ("overlay" | "template"); false if it does not exist.
    function openDesign(kind, id) { return showSession.openDesign(kind, id) }
    function closeDesign() { showSession.closeDesign() }
    // Done was pressed: the design is closed and the host should take the user back to the library.
    signal designClosed()

    // Every settled edit (and every undo/redo) is sent on to the engine.
    Connections {
        target: canvasHistory
        function onCommitted() { showSession.scheduleFlush() }
    }

    Connections {
        target: slideModel
        function onActiveSlideChanged() {
            showSession.flushSlide(slideStore.activeSlideId)
            canvasHistory.clear()
            root.textUndoKey = ""
            root.anyTextEditing = false
            slideStore.load(slideModel.activeSlideId)
        }
    }

    readonly property bool hasActiveSlide: slideModel.activeSlideId > 0

    function newShow() {
        // The engine starts the new document; the UI resets to its single empty slide
        // (asking first if the current show has unsaved changes).
        showSession.newShow()
    }
    // Show file actions (menu bar): the engine reads/writes the .vgr; these keep the
    // UI in step and prompt about unsaved changes.
    function openShow() { showSession.openShow() }
    // Opens a library path directly (shows-table row click, Main.qml routes it here).
    function openShowPath(path) { showSession.openShowPath(path) }
    // `then(saved)` is optional: the write is queued off the interface thread, so a
    // caller that must wait for the file (the "save changes?" dialog) passes it.
    function saveShow(then) { showSession.saveShow(then) }
    function saveShowAs(then) { showSession.saveShowAs(then) }

    property bool diagUndoHarness: false
    Component.onCompleted: {
        if (!root.diagUndoHarness)
            return
        slideModel.addSlide()
        const freshItem = root.addCanvasItem("text")
        root.handleCanvasSelect(freshItem.key, 0)
        function afterMs(ms, fn) {
            const t = Qt.createQmlObject('import QtQuick; Timer { interval: ' + ms + '; running: true; repeat: false }', root, "diagTimer")
            t.triggered.connect(fn)
        }
        afterMs(200, function () {
            const item = slideStore.current.items[0]
            console.log("[DIAG] fresh item key=", item.key)
            // Session 1: type "AAA", then commit (leave edit mode).
            root.beginTextEdit(item.key)
            item.text = "AAA"
            root.endTextEdit()
            afterMs(200, function () {
                console.log("[DIAG] after session 1 commit: text=", slideStore.current.items[0].text, "canUndo=", canvasHistory.canUndo)
                // Session 2: re-enter edit on the SAME item, type more —
                // but do NOT end the session yet.
                const item2 = slideStore.current.items[0]
                root.beginTextEdit(item2.key)
                item2.text = "AAABBB"
                console.log("[DIAG] mid session 2 (still editing): text=", item2.text, "anyTextEditing=", root.anyTextEditing)
                // Undo WHILE still mid-session-2 — the exact repro.
                root.undo()
                console.log("[DIAG] after undo WHILE mid-session-2: text=", slideStore.current.items[0].text,
                             "canUndo=", canvasHistory.canUndo, "canRedo=", canvasHistory.canRedo,
                             "anyTextEditing=", root.anyTextEditing)
                afterMs(50, function () {
                    console.log("[DIAG] commit-tick: text=", slideStore.current.items[0].text,
                                 "canUndo=", canvasHistory.canUndo, "canRedo=", canvasHistory.canRedo,
                                 "(canRedo MUST still be true — no junk step)")
                    root.undo()
                    const drained = slideStore.current.items.length === 0
                    console.log("[DIAG] second undo: items empty=", drained,
                                 "(expect true — the add command's before-state)",
                                 "text=", drained ? "(no items)" : slideStore.current.items[0].text,
                                 "canUndo=", canvasHistory.canUndo, "(expect false — stack drained)",
                                 "canRedo=", canvasHistory.canRedo,
                                 "(expect true — redo of session 1 intact)")
                })
            })
        })
    }

    property int contextMenuSlideIndex: -1

    property string rightPanelTab: "items"
    property string editWorkspaceMode: "slide"
    property string textEditBuffer: ""
    property string textEditStatus: ""
    property bool loadingTextEditBuffer: false
    property bool textEditBufferDirty: false
    property bool textEditApplying: false
    property bool textEditSwitchAfterApply: false
    property int textEditApplyToken: 0
    property string textEditSubmittedBuffer: ""
    property bool rightTabPinned: false
    onRightPanelTabChanged: { textFontMenu.visible = false; textFontFamilyMenu.visible = false; textAutoSizeMenu.visible = false; textCaseMenu.visible = false; borderStyleMenu.visible = false }

    function textEditGroupLabel(fields) {
        const raw = String(fields.tag ?? "")
        if (!raw) return String(fields.title ?? "")
        return raw.replace(/[_-]+/g, " ").replace(/\b\w/g, (letter) => letter.toUpperCase())
    }
    function exportTextEditBuffer() {
        const slides = []
        let previousGroup = null
        for (let row = 0; row < slideModel.rowCount(); ++row) {
            slides.push("[#slide:" + (row + 1) + "]")
            const slideId = slideModel.slideIdAt(row)
            const fields = slideModel.slideFieldsAt(row)
            const textItems = slideStore.blocksOf(slideId).filter((block) => block.kind === "text").reverse()
            const group = root.textEditGroupLabel(fields)
            const lines = []
            if (group && group !== previousGroup)
                lines.push("[" + group + "]")
            previousGroup = group
            textItems.forEach((item, index) => {
                lines.push("[" + "#" + (index + 1) + "]")
                const itemText = String(item.text ?? "")
                if (itemText !== "") lines.push(itemText)
            })
            slides.push(lines.join("\n"))
        }
        return slides.join("\n\n")
    }
    function enterTextEditMode() {
        if (root.designMode || root.editWorkspaceMode === "text" || root.textEditApplying) return
        if (root.anyTextEditing) root.endTextEdit()
        if (!root.textEditBufferDirty) {
            showSession.flushAll()
            showSession.flushTimer.stop()
            root.loadingTextEditBuffer = true
            root.textEditBuffer = root.exportTextEditBuffer()
            root.loadingTextEditBuffer = false
        }
        root.editWorkspaceMode = "text"
    }
    function leaveTextEditMode() {
        if (root.editWorkspaceMode !== "text") return
        textEditApplyTimer.stop()
        root.startTextEditApply(true)
    }
    function startTextEditApply(switchToSlide) {
        if (root.textEditApplying) {
            EngineBridge.log("debug", "Text edit", "Apply request coalesced; already applying, switchToSlide=" + switchToSlide)
            root.textEditSwitchAfterApply = root.textEditSwitchAfterApply || switchToSlide
            return
        }
        root.textEditSwitchAfterApply = switchToSlide
        root.textEditApplying = true
        EngineBridge.log("info", "Text edit", "Apply timer started; bufferChars=" + String(root.textEditBuffer).length
                         + ", switchToSlide=" + switchToSlide)
        textEditWorkTimer.start()
    }
    function finishTextEditApply() {
        const switchToSlide = root.textEditSwitchAfterApply
        EngineBridge.log("info", "Text edit", "Apply timer fired; bufferChars=" + String(root.textEditBuffer).length)
        let token = -1
        try {
            token = root.applyTextEditBufferNow()
        } catch (e) {
            root.textEditStatus = qsTr("Couldn't apply the text edits. Your text is still here.")
            EngineBridge.log("warning", "Text edit", "Apply failed: " + String(e))
        }
        if (token > 0) {
            root.textEditSubmittedBuffer = String(root.textEditBuffer)
            root.textEditApplyToken = token
            return
        }
        root.textEditApplying = false
        root.textEditSwitchAfterApply = false
        if (token === 0)
            root.textEditBufferDirty = false
        if (switchToSlide)
            root.editWorkspaceMode = "slide"
    }
    function applyTextEditBufferNow() {
        const source = String(root.textEditBuffer).replace(/\r/g, "").trim()
        let chunks = []
        const markedSlides = /^\s*\[#slide:\d+\]\s*$/m.test(source)
        const itemSectionCount = (source.match(/^\s*\[#1\]\s*$/gm) || []).length
        const numberedItemSections = !markedSlides && itemSectionCount > 1
        const chunkSlideNumbers = []
        if (markedSlides) {
            let current = null
            source.split("\n").forEach((line) => {
                const slideMarker = line.match(/^\s*\[#slide:(\d+)\]\s*$/)
                if (slideMarker) {
                    if (current !== null) chunks.push(current.join("\n"))
                    chunkSlideNumbers.push(Number(slideMarker[1]))
                    current = []
                } else if (current !== null) {
                    current.push(line)
                }
            })
            if (current !== null) chunks.push(current.join("\n"))
        } else if (numberedItemSections) {
            let current = []
            let sawItemMarker = false
            const finishSection = () => {
                while (current.length > 0 && String(current[current.length - 1]).trim() === "")
                    current.pop()
                chunks.push(current.join("\n"))
                chunkSlideNumbers.push(chunks.length)
                current = []
            }
            source.split("\n").forEach((line) => {
                const startsItem = /^\s*\[#1\]\s*$/.test(line)
                if (startsItem && sawItemMarker)
                    finishSection()
                current.push(line)
                if (startsItem)
                    sawItemMarker = true
            })
            if (sawItemMarker)
                finishSection()
        } else {
            chunks = source.split(/\n[\t ]*\n+/)
        }
        if (!markedSlides && !numberedItemSections && chunks.length !== slideModel.rowCount()) {
            root.textEditStatus = qsTr("Slide count changed. Restore the original slide breaks before applying text.")
            EngineBridge.log("warning", "Text edit", "Paste has " + chunks.length
                             + " slides; this show has " + slideModel.rowCount()
                             + ". Keep the slide count unchanged to preserve slide layouts.")
            return -1
        }
        if (markedSlides) {
            const seenSlides = ({})
            for (const slideNumber of chunkSlideNumbers) {
                if (slideNumber < 1 || slideNumber > slideModel.rowCount() || seenSlides[slideNumber]) {
                    root.textEditStatus = qsTr("A slide marker is invalid or repeated. Your text is still here.")
                    EngineBridge.log("warning", "Text edit", "Rejected slide marker " + slideNumber
                                     + " in a " + slideModel.rowCount() + "-slide show")
                    return -1
                }
                seenSlides[slideNumber] = true
            }
            if (chunks.length !== slideModel.rowCount())
                EngineBridge.log("info", "Text edit", "Applying " + chunks.length + " marked slide section(s) to a "
                                 + slideModel.rowCount() + "-slide show; unlisted slides will be left unchanged")
        }
        const parsedSlides = []
        for (let i = 0; i < chunks.length; ++i) {
            const lines = chunks[i].split("\n")
            if (lines.length && /^\s*\[#slide:\d+\]\s*$/.test(lines[0]))
                lines.shift()
            if (lines.length && /^\s*\[[^#\]]+\]\s*$/.test(lines[0]))
                lines.shift() // FreeShow group heading; slide order/groups stay intact here.
            const values = ({})
            let currentItem = 0
            let sawMarker = false
            for (const line of lines) {
                const marker = line.match(/^\s*\[#(\d+)(?::[^\]]+)?\](.*)$/)
                if (marker) {
                    sawMarker = true
                    currentItem = Math.max(0, Number(marker[1]) - 1)
                    if (values[currentItem] === undefined) values[currentItem] = ""
                    const rest = marker[2].replace(/^\s+/, "")
                    if (rest) values[currentItem] += (values[currentItem] ? "\n" : "") + rest
                } else {
                    values[currentItem] = (values[currentItem] === undefined ? "" : values[currentItem])
                        + (values[currentItem] ? "\n" : "") + line
                }
            }
            EngineBridge.log("debug", "Text edit", "slide "
                             + ((markedSlides || numberedItemSections) ? chunkSlideNumbers[i] : i + 1)
                             + ": parsed item indexes="
                             + Object.keys(values).join(",") + ", marker=" + sawMarker)
            parsedSlides.push({ values: values, sawMarker: sawMarker,
                                slideNumber: (markedSlides || numberedItemSections) ? chunkSlideNumbers[i] : 0 })
        }

        const slideEdits = []
        for (let row = 0; row < parsedSlides.length; ++row) {
            const parsed = parsedSlides[row]
            const targetRow = parsed.slideNumber > 0 ? parsed.slideNumber - 1 : row
            const uiSlideId = slideModel.slideIdAt(targetRow)
            const slideId = slideModel.engineIdAt(targetRow)
            const textBlocks = slideStore.blocksOf(uiSlideId)
                .filter((block) => block.kind === "text").reverse()
            let requestedCount = textBlocks.length
            if (parsed.sawMarker) {
                for (const key in parsed.values)
                    requestedCount = Math.max(requestedCount, Number(key) + 1)
            }
            const textByBlockId = ({})
            if (parsed.sawMarker) {
                for (const key in parsed.values) {
                    const index = Number(key)
                    const next = String(parsed.values[key] ?? "")
                    if (index < textBlocks.length) {
                        const target = textBlocks[index]
                        if (target.text !== next)
                            textByBlockId[target.key] = next
                    }
                }
            } else if (textBlocks.length > 0) {
                // FreeShow's unmarked text maps to its first text item; other
                // existing boxes are left untouched unless explicitly marked.
                const next = String(parsed.values[0] ?? "")
                if (textBlocks[0].text !== next)
                    textByBlockId[textBlocks[0].key] = next
            }
            for (let index = textBlocks.length; index < requestedCount; ++index) {
                textByBlockId["@new:" + index] = String(parsed.values[index] ?? "")
            }
            const newItemCount = Math.max(0, requestedCount - textBlocks.length)
            EngineBridge.log("info", "Text edit",
                             "slide " + (targetRow + 1) + " (" + slideId + "): parsedMarkers="
                             + parsed.sawMarker + ", existingTextItems=" + textBlocks.length
                             + ", requestedTextItems=" + requestedCount + ", newItems="
                             + newItemCount + ", changedExistingItems="
                             + Object.keys(textByBlockId).filter((key) => key.indexOf("@new:") !== 0).length)
            if (Object.keys(textByBlockId).length > 0)
                slideEdits.push({ slideId: slideId, textByBlockId: textByBlockId,
                                  fontFamily: FontService.defaultContentFamily })
        }
        if (slideEdits.length === 0) {
            root.textEditStatus = qsTr("No changes detected. Add the next [#N] marker on a slide to create another text box.")
            EngineBridge.log("info", "Text edit", "Parsed " + parsedSlides.length
                             + " slide(s); no slide edits were submitted")
            return 0
        }
        EngineBridge.log("info", "Text edit", "Submitting " + slideEdits.length
                         + " changed slide(s) from " + parsedSlides.length + " parsed slide(s)")
        const token = ShowService.applyTextEditBatch(slideEdits)
        if (token <= 0) {
            root.textEditStatus = qsTr("Couldn't start the text update. Your text is still here.")
            EngineBridge.log("warning", "Text edit", "Batch submission returned token " + token)
            return -1
        }
        EngineBridge.log("info", "Text edit", "Submitted batch job token " + token)
        return token
    }

    property bool bgModalOpen: false
    property string bgModalTarget: "background"
    property var bgModalTargetKeys: []
    function openBackgroundPicker(target) {
        root.bgModalTarget = target
        root.bgModalTargetKeys = root.selectedCanvasObjects.slice()
        root.bgModalOpen = true
    }
    // The items the picker is acting on: what it was OPENED for, falling back
    // to the live selection (a caller that opened the dialog without latching).
    function bgPickerKeys() {
        return root.bgModalTargetKeys.length > 0 ? root.bgModalTargetKeys
                                                 : root.selectedCanvasObjects
    }
    // The first of those items, resolved now rather than at every read of the
    // live selection (see bgModalTargetKeys).
    function bgPickerPrimaryItem() {
        if (root.bgPickerKeys().length === 0)
            return null
        return root.canvasObjectByKey(root.bgPickerKeys()[0])
    }

    // ---- ONE WAY TO COLOUR AN ITEM ----------------------------------------
    function applyItemColor(item, target, hex) {
        if (!item || !item.style)
            return false
        const old = item.style
        const isBorder = target === "border"
        const next = canvasItemStylePrototype.createObject(item, {
            kind: old.kind,
            padding: old.padding,
            cornerRadius: old.cornerRadius,
            borderEnabled: old.borderEnabled,
            borderWidth: old.borderWidth,
            borderStyle: old.borderStyle,
            backgroundColor: isBorder ? old.backgroundColor : hex,
            borderColor: isBorder ? hex : old.borderColor,
            opacity: old.opacity,
            mediaFit: old.mediaFit
        })
        if (!next)
            return false
        item.style = next
        return true
    }

    // Prototype for applyItemColor. Parented per apply (see that function), so it
    // dies with the item it belongs to.
    Component { id: canvasItemStylePrototype; CanvasItemStyle {} }

    property var selectedCanvasObjects: []

    // Canvas items live in slideStore (per-slide, archived on slide switch)
    // — no screen-level array anymore.

    property real canvasZoom: 1

    // ---- Keyboard shortcuts --------------------------------------------

    // ---- Undo/redo -------------------------------------------------------
    UndoHistory {
        id: canvasHistory
        capture: function () {
            return { items: slideStore.snapshotItems(), background: slideStore.current.background,
                     filters: slideStore.current.filters }
        }
        apply: function (snap) {
            slideStore.restoreItems(snap.items)
            slideStore.current.background = snap.background
            slideStore.current.filters = snap.filters !== undefined ? snap.filters
                                                                   : slideStore.identityFilters()
        }
        afterRestore: function () { root.selectedCanvasObjects = [] }
    }

    function pushUndoSnapshot() {
        canvasHistory.push()
    }

    function reopenEditAfterHistoryJump(wasEditing, editingKey) {
        if (!wasEditing || editingKey === "")
            return
        if (!root.canvasObjectByKey(editingKey))
            return
        root.selectedCanvasObjects = [editingKey]
        Qt.callLater(() => {
            const d = root.delegateForKey(editingKey)
            if (d && d.textLabel)
                d.textLabel.editing = true
        })
    }
    function undo() {
        canvasHistory.commitNow()
        const wasEditing = root.anyTextEditing
        const editingKey = wasEditing && root.selectedCanvasObjects.length === 1 ? root.selectedCanvasObjects[0] : ""
        canvasHistory.undo()
        root.textUndoKey = ""
        root.anyTextEditing = false
        if (wasEditing && editingKey !== "")
            root.reopenEditAfterHistoryJump(wasEditing, editingKey)
        else if (!slideListFlick.activeFocus)
            mCanvas.forceActiveFocus()
    }
    function redo() {
        canvasHistory.commitNow()
        const wasEditing = root.anyTextEditing
        const editingKey = wasEditing && root.selectedCanvasObjects.length === 1 ? root.selectedCanvasObjects[0] : ""
        canvasHistory.redo()
        root.textUndoKey = ""
        root.anyTextEditing = false
        if (wasEditing && editingKey !== "")
            root.reopenEditAfterHistoryJump(wasEditing, editingKey)
        else if (!slideListFlick.activeFocus)
            mCanvas.forceActiveFocus()
    }

    property bool dragSnapshotTaken: false

    readonly property bool canvasFiltersActive:
        !slideStore.filtersAreIdentity(slideStore.current.filters)

    property string textUndoKey: ""
    property bool anyTextEditing: false
    function beginTextEdit(key) {
        if (root.textUndoKey === key)
            return
        root.textUndoKey = key
        canvasHistory.push(qsTr("Edit text"), false)
    }
    function endTextEdit() {
        root.textUndoKey = ""
        canvasHistory.commit()
    }

    function toggleTextStyle(key) {
        const item = root.primarySelectedItem
        if (!item || item.kind !== "text")
            return
        canvasHistory.push(qsTr("Change text style"))
        const m = Object.assign({}, item.meta)
        m[key] = !(m[key] === true)
        item.meta = m
    }

    // ---- Copy/paste --------------------------------------------------
    property var clipboardItems: []

    function copySelectedCanvasObjects() {
        const all = slideStore.snapshotItems()
        root.clipboardItems = all.filter((d) => root.selectedCanvasObjects.indexOf(d.key) >= 0)
    }

    function pasteClipboardItems() {
        if (root.clipboardItems.length === 0)
            return
        const newKeys = []
        root.clipboardItems.forEach((d) => {
            const item = root.addCanvasItem(d.kind)
            item.text = d.text
            item.x = d.x + 16
            item.y = d.y + 16
            item.width = d.width
            item.height = d.height
            item.meta = d.meta
            item.style.padding = d.style.padding
            item.style.backgroundColor = d.style.backgroundColor
            item.style.cornerRadius = d.style.cornerRadius
            item.style.borderEnabled = d.style.borderEnabled
            item.style.borderWidth = d.style.borderWidth
            item.style.borderStyle = d.style.borderStyle
            item.style.borderColor = d.style.borderColor
            item.style.opacity = d.style.opacity
            item.style.mediaFit = d.style.mediaFit
            newKeys.push(item.key)
        })
        root.selectedCanvasObjects = newKeys
        EventBus.notify(qsTr("%1 item(s) pasted").arg(newKeys.length),
                        "success", "Edit", "ui.edit.itemsPasted")
    }

    function duplicateSelectedCanvasObjects() {
        const src = root.canvasObjectByKey(root.selectedCanvasObjects[0])
        if (src)
            root.addCanvasItem(src.kind, src)
    }

    // Select all (Ctrl+A / the menu's own row): every item on the slide, in
    // canvas order, so a following Duplicate/Delete/align acts on the lot.
    function selectAllCanvasObjects() {
        root.selectedCanvasObjects = slideStore.current.items.map((it) => it.key)
        mCanvas.forceActiveFocus()
    }

    // ---- Stacking, from the canvas menu's Rearrange submenu -----------------
    function restackCanvasItem(key, where) {
        const items = slideStore.current.items
        let from = -1
        for (let i = 0; i < items.length; ++i)
            if (items[i].key === key)
                from = i
        if (from < 0)
            return
        let to = from
        if (where === "front")
            to = items.length - 1
        else if (where === "back")
            to = 0
        else if (where === "forward")
            to = Math.min(items.length - 1, from + 1)
        else if (where === "backward")
            to = Math.max(0, from - 1)
        if (to === from)
            return
        canvasHistory.push(where === "front" || where === "back" ? qsTr("Reorder item") : qsTr("Move item"))
        const next = items.slice()
        next.splice(to, 0, next.splice(from, 1)[0])
        slideStore.current.items = next
        showSession.scheduleFlush()
        mCanvas.forceActiveFocus()
    }

    // ---- Per-item triggers and timers (the menu's Item actions submenu) -----
    function setItemAction(key, field, value) {
        const item = root.canvasObjectByKey(key)
        if (!item)
            return
        canvasHistory.push(qsTr("Item action"))
        const meta = item.meta ? Object.assign({}, item.meta) : ({})
        if (field === "clickReveal") {
            meta.clickReveal = !!value
        } else {
            const actions = Object.assign({}, meta.actions ? meta.actions : {})
            actions[field] = value
            meta.actions = actions
        }
        item.meta = meta
        showSession.scheduleFlush()
    }

    function openItemTimer(key, field) {
        const item = root.canvasObjectByKey(key)
        if (!item)
            return
        const actions = (item.meta && item.meta.actions) ? item.meta.actions : ({})
        itemTimerDialog.itemKey = key
        itemTimerDialog.action = field
        itemTimerDialog.open(String(Math.round(actions[field] ?? 0)))
    }

    // ---- The canvas right-click menu -------------------------------------
    property string canvasMenuKey: ""

    function canvasMenuModel() {
        const key = root.canvasMenuKey
        const item = key !== "" ? root.canvasObjectByKey(key) : null
        const hasItem = item !== null
        const meta = (item && item.meta) ? item.meta : ({})
        const actions = meta.actions ? meta.actions : ({})
        const items = slideStore.current.items
        let index = -1
        for (let i = 0; i < items.length; ++i)
            if (items[i].key === key)
                index = i
        const canForward = index >= 0 && index < items.length - 1
        const canBackward = index > 0
        // A set duration reads on the row itself (`Display duration — 5s`), so
        // the submenu answers "what is it now" without opening the field.
        const withSeconds = (label, seconds) => seconds > 0
            ? label + qsTr(" — %1s").arg(seconds) : label
        return [
            {
                label: qsTr("Item actions"), icon: "actions", color: "#d497ff",
                disabled: !hasItem,
                children: [
                    { id: "clickReveal", label: qsTr("Reveal on click"), icon: "clickAction",
                      color: "#d4a3f6", checked: meta.clickReveal === true },
                    { id: "displayDuration", icon: "clock", color: "#d497ff",
                      label: withSeconds(qsTr("Display duration"), actions.displayDuration ?? 0) },
                    { divider: true },
                    { id: "showTimer", icon: "timeIn", color: "#cd86ff",
                      label: withSeconds(qsTr("Time until show"), actions.showTimer ?? 0) },
                    { id: "hideTimer", icon: "timeOut", color: "#cd86ff",
                      label: withSeconds(qsTr("Time until hide"), actions.hideTimer ?? 0) }
                ]
            },
            {
                label: qsTr("Rearrange"), icon: "rearrange", color: "#93f190",
                disabled: !hasItem,
                children: [
                    { id: "toFront", label: qsTr("Move to front"), icon: "toFront", color: "#93f190", disabled: !canForward },
                    { id: "forward", label: qsTr("Move forward"), icon: "moveUp", color: "#93f190", disabled: !canForward },
                    { id: "backward", label: qsTr("Move backward"), icon: "moveDown", color: "#93f190", disabled: !canBackward },
                    { id: "toBack", label: qsTr("Move to back"), icon: "toBack", color: "#93f190", disabled: !canBackward }
                ]
            },
            { divider: true },
            { id: "duplicate", label: qsTr("Duplicate"), icon: "duplicate", color: "#97c7ff",
              shortcut: "Ctrl+D", disabled: root.selectedCanvasObjects.length === 0 },
            { id: "delete", label: qsTr("Delete"), icon: "delete", color: "#ff5454",
              shortcut: "Del", disabled: root.selectedCanvasObjects.length === 0 },
            { divider: true },
            { id: "paste", label: qsTr("Paste"), icon: "paste", color: "#97c7ff",
              shortcut: "Ctrl+V", disabled: root.clipboardItems.length === 0 },
            { id: "selectAll", label: qsTr("Select all"), icon: "selectAll",
              shortcut: "Ctrl+A", disabled: items.length === 0 }
        ]
    }

    // `at` is the press point in CANVAS coordinates (mCanvas's own space).
    function openCanvasMenu(x, y, at) {
        const under = root.canvasKeysAt(at)
        const key = under.length > 0 ? under[0] : ""
        if (key !== "")
            root.handleCanvasSelect(key, 0)
        else
            root.selectedCanvasObjects = []
        root.canvasMenuKey = key
        canvasMenu.model = root.canvasMenuModel()
        canvasMenu.openAt(mCanvas, x, y)
    }

    function canvasMenuPicked(id) {
        const key = root.canvasMenuKey
        switch (id) {
        case "toFront": root.restackCanvasItem(key, "front"); break
        case "forward": root.restackCanvasItem(key, "forward"); break
        case "backward": root.restackCanvasItem(key, "backward"); break
        case "toBack": root.restackCanvasItem(key, "back"); break
        case "clickReveal": {
            const item = root.canvasObjectByKey(key)
            root.setItemAction(key, "clickReveal", !(item && item.meta && item.meta.clickReveal))
            break
        }
        case "displayDuration": root.openItemTimer(key, "displayDuration"); break
        case "showTimer": root.openItemTimer(key, "showTimer"); break
        case "hideTimer": root.openItemTimer(key, "hideTimer"); break
        case "duplicate": root.duplicateSelectedCanvasObjects(); break
        case "delete": root.deleteSelectedCanvasObjects(); break
        case "paste": root.pasteClipboardItems(); break
        case "selectAll": root.selectAllCanvasObjects(); break
        }
    }

    // The slide row's menu (see slideContextMenu): the slide the menu was
    // opened on is `contextMenuSlideIndex`.
    function openSlideMenu(index, source, x, y) {
        root.contextMenuSlideIndex = index
        slideContextMenu.openAt(source, x, y)
    }

    function slideMenuPicked(id) {
        const index = root.contextMenuSlideIndex
        const eid = slideModel.engineIdAt(index)
        switch (id) {
        case "edit":
            slideModel.selectSlide(index)
            break
        case "group": {
            if (eid !== "") {
                groupPickerMenu.slideId = eid
                groupPickerMenu.openAt(slideContextMenu.panelRect, 0, 0, root)
            }
            break
        }
        case "nextTimer":
            // How long this slide stays up before the show moves on by itself (0 = it waits).
            if (eid !== "") {
                slideTimerDialog.slideId = eid
                slideTimerDialog.open(String(Math.round(ShowService.slideOf(eid).nextTimer ?? 0)))
            }
            break
        case "duplicate":
            // The ENGINE duplicates the slide (fresh id, its items copied); the list
            // and the copy's canvas are rebuilt from what it made.
            showSession.duplicateSlide(index)
            break
        case "delete":
            // The engine deletes the slide (and anything scoped to it); the UI
            // drops its archive and lands on the neighbouring slide.
            showSession.removeSlide(index)
            break
        }
    }

    function isCanvasObjectSelected(key) {
        return root.selectedCanvasObjects.indexOf(key) >= 0
    }
    // ---- CLICKING THROUGH A STACK OF ITEMS --------------------------------
    property string canvasCycleKey: ""
    property point canvasCycleAt: Qt.point(-1, -1)
    property real canvasCycleAtMs: 0
    readonly property real canvasCycleSlop: 4
    readonly property real canvasCycleMinGapMs: 350
    readonly property real canvasCycleMaxGapMs: 900

    // Every item whose box covers `p`, TOPMOST FIRST (a later item is declared
    // later, so it paints and hits above an earlier one).
    function canvasKeysAt(p) {
        const out = []
        const items = slideStore.current.items
        for (let i = items.length - 1; i >= 0; --i) {
            const it = items[i]
            if (p.x >= it.x && p.x <= it.x + it.width
                    && p.y >= it.y && p.y <= it.y + it.height)
                out.push(it.key)
        }
        return out
    }

    function handleCanvasSelect(key, modifiers, at) {
        if (at !== undefined && (modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) === 0) {
            const now = Date.now()
            const samePoint = Math.abs(at.x - root.canvasCycleAt.x) <= root.canvasCycleSlop
                           && Math.abs(at.y - root.canvasCycleAt.y) <= root.canvasCycleSlop
            const gap = now - root.canvasCycleAtMs
            if (samePoint && gap >= root.canvasCycleMinGapMs && gap <= root.canvasCycleMaxGapMs) {
                const stack = root.canvasKeysAt(at)     // topmost first
                const from = stack.indexOf(root.canvasCycleKey)
                if (from >= 0) {
                    root.canvasCycleAtMs = now
                    if (from + 1 < stack.length) {
                        const next = stack[from + 1]
                        root.canvasCycleKey = next
                        root.selectedCanvasObjects = [next]
                        mCanvas.forceActiveFocus()
                    }
                    return
                }
            }
            root.canvasCycleKey = key
            root.canvasCycleAt = at
            root.canvasCycleAtMs = now
        }
        if (modifiers & Qt.ShiftModifier) {
            const idx = root.selectedCanvasObjects.indexOf(key)
            if (idx >= 0) {
                const copy = root.selectedCanvasObjects.slice()
                copy.splice(idx, 1)
                root.selectedCanvasObjects = copy
            } else {
                root.selectedCanvasObjects = root.selectedCanvasObjects.concat([key])
            }
        } else {
            // Plain click and Ctrl+click both just select this one object.
            root.selectedCanvasObjects = [key]
        }
        mCanvas.forceActiveFocus()
    }

    function canvasObjectByKey(key) {
        const items = slideStore.current.items
        for (let i = 0; i < items.length; ++i) {
            if (items[i].key === key)
                return items[i]
        }
        return null
    }

    readonly property CanvasItemStyle primarySelectedItemStyle: root.selectedCanvasObjects.length > 0
        ? (root.canvasObjectByKey(root.selectedCanvasObjects[0])?.style ?? null)
        : null

    // The raw CanvasItem behind primarySelectedItemStyle — TextItemPanel
    // needs the whole item (meta, x/y/width/height), not just its style.
    readonly property var primarySelectedItem: root.selectedCanvasObjects.length > 0
        ? root.canvasObjectByKey(root.selectedCanvasObjects[0])
        : null

    readonly property bool primaryIsMedia: root.primarySelectedItem !== null
        && ["image", "video", "audio", "bus"].includes(root.primarySelectedItem.kind)
    readonly property var primaryMediaItem: root.primaryIsMedia ? root.primarySelectedItem : null

    readonly property bool primaryIsText: root.primarySelectedItem !== null
        && root.primarySelectedItem.kind === "text"
    readonly property bool textTabAvailable: root.primaryIsText
    readonly property bool mediaTabAvailable: root.primaryMediaItem !== null

    onPrimarySelectedItemChanged: {
        if (!root.primarySelectedItem) {
            textFontMenu.visible = false
            textFontFamilyMenu.visible = false
            textAutoSizeMenu.visible = false
            textCaseMenu.visible = false
            borderStyleMenu.visible = false
        }
        if (!root.primarySelectedItem) {
            if (root.rightPanelTab === "text" || root.rightPanelTab === "media" || !root.rightTabPinned)
                root.rightPanelTab = "items"
        } else if (!root.rightTabPinned && root.textTabAvailable) {
            root.rightPanelTab = "text"
        } else if (!root.rightTabPinned && root.mediaTabAvailable) {
            root.rightPanelTab = "media"
        } else if ((root.rightPanelTab === "text" && !root.textTabAvailable)
                   || (root.rightPanelTab === "media" && !root.mediaTabAvailable)) {
            root.rightPanelTab = "items"
        }
    }

    readonly property bool primarySelectedSupportsFill: root.primarySelectedItemStyle === null
        || ["text", "shape", "clock", "timer", "vignette", "corners"].includes(root.primarySelectedItemStyle.kind)

    // ---- Alignment-guide snapping helpers ----
    readonly property int snapDistance: 8

    function snapCandidates(canvasWidth, canvasHeight, others) {
        const xs = [0, canvasWidth / 2, canvasWidth]
        const ys = [0, canvasHeight / 2, canvasHeight]
        others.forEach((o) => {
            xs.push(o.x, o.x + o.width / 2, o.x + o.width)
            ys.push(o.y, o.y + o.height / 2, o.y + o.height)
        })
        return { xs: xs, ys: ys }
    }

    function snapClosest(value, candidates) {
        let best = null
        let bestDist = root.snapDistance
        candidates.forEach((c) => {
            const d = Math.abs(value - c)
            if (d < bestDist) {
                bestDist = d
                best = c
            }
        })
        return best
    }

    function snapMove(x, y, width, height, canvasWidth, canvasHeight, others) {
        const cand = root.snapCandidates(canvasWidth, canvasHeight, others)
        const guides = []

        let snappedX = x
        for (const ex of [x + width / 2, x, x + width]) {
            const mx = root.snapClosest(ex, cand.xs)
            if (mx !== null) {
                snappedX = x + (mx - ex)
                guides.push({ axis: "x", pos: mx })
                break
            }
        }

        let snappedY = y
        for (const ey of [y + height / 2, y, y + height]) {
            const my = root.snapClosest(ey, cand.ys)
            if (my !== null) {
                snappedY = y + (my - ey)
                guides.push({ axis: "y", pos: my })
                break
            }
        }

        return { x: snappedX, y: snappedY, guides: guides }
    }

    function snapResize(x, y, width, height, canvasWidth, canvasHeight, others, activeLeft, activeRight, activeTop, activeBottom) {
        const cand = root.snapCandidates(canvasWidth, canvasHeight, others)
        const guides = []

        let rx = x, rw = width
        if (activeRight) {
            const mr = root.snapClosest(x + width, cand.xs)
            if (mr !== null) {
                rw = Math.max(1, mr - x)
                guides.push({ axis: "x", pos: mr })
            }
        } else if (activeLeft) {
            const ml = root.snapClosest(x, cand.xs)
            if (ml !== null) {
                rw = Math.max(1, width + (x - ml))
                rx = x + width - rw
                guides.push({ axis: "x", pos: ml })
            }
        }

        let ry = y, rh = height
        if (activeBottom) {
            const mb = root.snapClosest(y + height, cand.ys)
            if (mb !== null) {
                rh = Math.max(1, mb - y)
                guides.push({ axis: "y", pos: mb })
            }
        } else if (activeTop) {
            const mt = root.snapClosest(y, cand.ys)
            if (mt !== null) {
                rh = Math.max(1, height + (y - mt))
                ry = y + height - rh
                guides.push({ axis: "y", pos: mt })
            }
        }

        return { x: rx, y: ry, width: rw, height: rh, guides: guides }
    }

    property var canvasMoveDrag: null

    function applyCanvasMove(key, dx, dy, snapDisabled) {
        if (!root.canvasMoveDrag || root.canvasMoveDrag.key !== key) {
            canvasHistory.push(qsTr("Move"), false)
            const positions = {}
            root.selectedCanvasObjects.forEach((k) => {
                const o = root.canvasObjectByKey(k)
                if (o) positions[k] = { x: o.x, y: o.y }
            })
            root.canvasMoveDrag = { key: key, startPositions: positions, accumDx: 0, accumDy: 0 }
        }
        root.canvasMoveDrag.accumDx += dx
        root.canvasMoveDrag.accumDy += dy

        const primaryStart = root.canvasMoveDrag.startPositions[key]
        const primaryObj = root.canvasObjectByKey(key)
        let finalDx = root.canvasMoveDrag.accumDx
        let finalDy = root.canvasMoveDrag.accumDy

        if (!snapDisabled && primaryStart && primaryObj) {
            const others = []
            slideStore.current.items.forEach((it) => {
                if (root.selectedCanvasObjects.indexOf(it.key) >= 0) return
                others.push({ x: it.x, y: it.y, width: it.width, height: it.height })
            })
            const snap = root.snapMove(primaryStart.x + finalDx, primaryStart.y + finalDy, primaryObj.width, primaryObj.height, mCanvas.width, mCanvas.height, others)
            finalDx = snap.x - primaryStart.x
            finalDy = snap.y - primaryStart.y
            canvasSnapGuides.guides = snap.guides
        } else {
            canvasSnapGuides.guides = []
        }

        root.selectedCanvasObjects.forEach((k) => {
            const start = root.canvasMoveDrag.startPositions[k]
            const o = root.canvasObjectByKey(k)
            if (o && start) {
                o.x = start.x + finalDx
                o.y = start.y + finalDy
            }
        })
    }

    function applyCanvasResize(key, geom) {
        const primaryObj = root.canvasObjectByKey(key)
        if (!primaryObj)
            return

        if (geom.snapDisabled) {
            canvasSnapGuides.guides = []
            return
        }

        if (!root.dragSnapshotTaken) {
            canvasHistory.push(qsTr("Resize"), false)
            root.dragSnapshotTaken = true
        }

        const others = []
        slideStore.current.items.forEach((it) => {
            if (it.key === key) return
            others.push({ x: it.x, y: it.y, width: it.width, height: it.height })
        })
        const snap = root.snapResize(geom.x, geom.y, geom.width, geom.height, mCanvas.width, mCanvas.height, others, geom.left, geom.right, geom.top, geom.bottom)
        primaryObj.x = snap.x
        primaryObj.y = snap.y
        primaryObj.width = snap.width
        primaryObj.height = snap.height
        canvasSnapGuides.guides = snap.guides
    }

    function endCanvasDrag() {
        root.canvasMoveDrag = null
        root.dragSnapshotTaken = false
        canvasSnapGuides.guides = []
        canvasHistory.commit()
    }

    function nudgeSelectedCanvasObjects(dx, dy) {
        canvasHistory.push(qsTr("Nudge"))
        root.selectedCanvasObjects.forEach((k) => {
            const o = root.canvasObjectByKey(k)
            if (o) {
                o.x += dx
                o.y += dy
            }
        })
    }

    function removeCanvasItems(keys) {
        canvasHistory.push(qsTr("Delete"))
        showSession.removeItems(keys)   // the engine deletes them first
        slideStore.removeItems(keys)
        root.selectedCanvasObjects = root.selectedCanvasObjects.filter((k) => keys.indexOf(k) < 0)
    }

    function deleteSelectedCanvasObjects() {
        root.removeCanvasItems(root.selectedCanvasObjects)
        root.selectedCanvasObjects = []
    }

    function moveCanvasItem(key, direction) {
        const items = slideStore.current.items
        let from = -1
        for (let i = 0; i < items.length; ++i)
            if (items[i].key === key)
                from = i
        const to = from + direction
        if (from < 0 || to < 0 || to >= items.length)
            return
        root.pushUndoSnapshot()
        const next = items.slice()
        next.splice(to, 0, next.splice(from, 1)[0])
        slideStore.current.items = next
        showSession.scheduleFlush()
        mCanvas.forceActiveFocus()
    }

    // ---- The slide's filter set (SLIDE tab · Filters) ----------------------
    function setSlideFilter(key, value) {
        if (!root.hasActiveSlide)
            return
        slideStore.setFilter(key, value)
        showSession.scheduleFlush()
    }

    function resetSlideFilters() {
        canvasHistory.push(qsTr("Reset filters"))
        slideStore.current.filters = slideStore.identityFilters()
        showSession.scheduleFlush()
        mCanvas.forceActiveFocus()
    }

    function addCanvasItem(kind, copyFrom, insertAt) {
        canvasHistory.push(copyFrom ? qsTr("Duplicate item") : qsTr("Add item"))
        const n = slideStore.current.items.length
        const isCamera = kind === "camera"
        showSession.ensureDocument()
        const item = showSession.addItem(
            kind,
            40 + (n % 6) * 20,
            40 + (n % 6) * 20,
            isCamera ? 112 : 220,
            isCamera ? 84 : 44,
            copyFrom,
            insertAt)
        if (!item)
            return null
        root.handleCanvasSelect(item.key, 0)
        return item
    }

    function addTextItem(insertAt) {
        const item = root.addCanvasItem("text", null, insertAt)
        if (item)
            item.meta = Object.assign({}, item.meta, { fontFamily: FontService.defaultContentFamily })
        return item
    }

    // Adds a whole-screen treatment (overlay designs): a vignette tinting the edges or corners cut round. It covers the
    // stage; its colour is the item's background (edited on the Items tab like any item's) and meta.inset is how far in it reaches.
    function addScreenTreatment(kind) {
        const item = root.addCanvasItem(kind)
        if (!item)
            return null
        item.x = 0
        item.y = 0
        item.width = mCanvas.width
        item.height = mCanvas.height
        item.style.backgroundColor = kind === "vignette" ? "#ffffff" : "#000000"
        item.meta = { inset: kind === "vignette" ? 100 : 20 }
        return item
    }

    function addMediaItem(kind, file) {
        if (!file || !file.path)
            return null
        const item = root.addCanvasItem(kind)
        if (!item)
            return null
        item.text = String(file.name !== undefined ? file.name : "")
        // A 16:9 box — the shape most pictures and every video already are,
        // so the common case lands ready to use instead of needing a resize.
        item.width = 320
        item.height = 180
        item.meta = Object.assign({}, item.meta, { src: String(file.path), mediaKind: kind })
        return item
    }

    property string mediaReplaceKey: ""
    onMediaModalOpenChanged: {
        if (!root.mediaModalOpen)
            root.mediaReplaceKey = ""
    }

    function replaceItemMedia(key, file) {
        if (!file || !file.path)
            return false
        const target = root.canvasObjectByKey(key)
        if (!target)
            return false
        root.pushUndoSnapshot()
        target.text = String(file.name !== undefined ? file.name : "")
        target.meta = Object.assign({}, target.meta, {
                                            src: String(file.path),
                                            mediaKind: String(target.kind || "")
                                        })
        return true
    }

    // ---- MEDIA tab: writing a media item's own values -----------------------
    function setMediaMeta(patch) {
        const item = root.primaryMediaItem
        if (!item)
            return
        const media = Object.assign({}, item.meta && item.meta.media ? item.meta.media : {}, patch)
        item.meta = Object.assign({}, item.meta ? item.meta : {}, { media: media })
    }

    function applyMediaFit(fit) {
        root.selectedCanvasObjects.forEach((key) => {
            const item = root.canvasObjectByKey(key)
            if (!item || !item.style)
                return
            item.style.mediaFit = fit
            const media = Object.assign({}, item.meta && item.meta.media ? item.meta.media : {},
                                        { fit: fit })
            item.meta = Object.assign({}, item.meta ? item.meta : {}, { media: media })
        })
    }

    function setMediaFlag(key, on) {
        root.setMediaMeta({ [key]: on === true })
    }

    // ---- A WINDOW'S OWN BUS (a canvas item of kind "bus") -------------------
    readonly property bool primaryIsWindow: root.primarySelectedItem !== null
        && root.primarySelectedItem.kind === "bus"
    // The bus a window names (0 = none).
    function windowBusId() {
        const item = root.primarySelectedItem
        return item !== null && item.meta && item.meta.busId !== undefined
            ? Number(item.meta.busId) : 0
    }
    // The bus's roster ROW for that id (-1 = it is gone).
    function windowBusRow(id) {
        if (!(id > 0))
            return -1
        for (let i = 0; i < BusListModel.rowCount(); ++i) {
            const bus = BusListModel.getBus(i)
            if (bus && Number(bus.id) === id)
                return i
        }
        return -1
    }
    function windowBusName() {
        const row = root.windowBusRow(root.windowBusId())
        if (row < 0)
            return qsTr("No bus")
        const bus = BusListModel.getBus(row)
        return bus.name !== undefined ? String(bus.name) : qsTr("No bus")
    }
    function assignWindowBus(bus) {
        const item = root.primarySelectedItem
        if (!item || item.kind !== "bus" || !bus)
            return
        const pickedRow = bus.row !== undefined ? Number(bus.row) : root.windowBusRow(Number(bus.id))
        if (pickedRow < 0)
            return
        const info = BusListModel.getBus(pickedRow)
        if (!info || info.id === undefined)
            return
        root.pushUndoSnapshot()
        item.meta = Object.assign({}, item.meta ? item.meta : {}, {
            busId: Number(info.id),
            busName: info.name !== undefined ? String(info.name) : "",
            busType: info.type !== undefined ? String(info.type) : ""
        })
        item.text = info.name !== undefined ? String(info.name) : ""
    }

    function setMediaCrop(side, percent) {
        const item = root.primaryMediaItem
        if (!item)
            return
        const media = item.meta && item.meta.media ? item.meta.media : {}
        const crop = Object.assign({ top: 0, right: 0, bottom: 0, left: 0 }, media.crop || {})
        crop[side] = Math.max(0, Math.min(90, percent))
        root.setMediaMeta({ crop: crop })
    }

    function resetMediaCrop() {
        root.setMediaMeta({ crop: { top: 0, right: 0, bottom: 0, left: 0 } })
    }

    function setMediaFilter(key, value) {
        const item = root.primaryMediaItem
        if (!item)
            return
        const media = item.meta && item.meta.media ? item.meta.media : {}
        const filter = Object.assign({}, media.filter || {})
        filter[key] = value
        root.setMediaMeta({ filter: filter })
    }

    function resetMediaFilters() {
        root.setMediaMeta({ filter: ({}) })
    }

    function clearItemFile() {
        const item = root.primaryMediaItem
        if (!item)
            return
        root.pushUndoSnapshot()
        const meta = Object.assign({}, item.meta ? item.meta : {})
        delete meta.src
        delete meta.mediaKind
        item.text = ""
        item.meta = meta
    }

    function pickReplacementMedia() {
        if (!root.primaryIsMedia)
            return
        root.mediaReplaceKey = String(root.primaryMediaItem.key || "")
        root.mediaPickKind = String(root.primaryMediaItem.kind || "")
        root.mediaModalOpen = true
    }

    function mediaPictureUrl(path) {
        if (!path)
            return ""
        return "image://mediathumb/500/-1/" + encodeURIComponent(path)
    }

    function delegateForKey(key) {
        for (let i = 0; i < canvasItemsRepeater.count; ++i) {
            const d = canvasItemsRepeater.itemAt(i)
            if (d && d.modelData && d.modelData.key === key)
                return d
        }
        return null
    }

    // Add-content chip + its popover menu (bottom of the canvas).
    property bool addMenuOpen: false
    property bool mediaMenuOpen: false
    // Which media kind the picker is asking for ("image" | "video" | "audio"),
    // and whether it is up. A picked file becomes an item of that kind.
    property string mediaPickKind: ""
    property bool mediaModalOpen: false
    onAddMenuOpenChanged: {
        if (!root.addMenuOpen)
            root.mediaMenuOpen = false
    }
    property bool timerModalOpen: false
    property bool clockModalOpen: false
    property bool shapeModalOpen: false
    property bool busModalOpen: false
    // The WINDOW's own bus picker (see primaryIsWindow / assignWindowBus below):
    // editing which bus an EXISTING bus item shows, rather than making a new one.
    property bool windowBusModalOpen: false
    property bool metadataPaletteOpen: false
    // The hovered field's own sentence (MetadataFields), shown at the foot of
    // the palette so the list explains itself.
    property string metadataHint: ""

    function addMetadataItem(token, at) {
        if (!token)
            return null
        const field = MetadataFields.fieldForToken(token)
        const item = root.addCanvasItem("text")
        if (!item)
            return null
        item.text = token
        const fontSize = 40
        item.bind = String(field && field.bind ? field.bind : "")
        item.meta = Object.assign({}, item.meta, {
            color: "#ffffff",
            fontFamily: FontService.defaultContentFamily,
            fontSize: fontSize,
            bold: false,
            italic: false,
            align: "left",
            autoSize: "shrinkToFit",
            textCase: "none",
            verticalAlign: "center"
        })
        const wantW = Math.round(String(token).length * fontSize * 0.5) + 24
        const wantH = Math.round(fontSize * 1.4)
        item.width = Math.min(wantW, Math.max(160, mCanvas.width - 80))
        item.height = wantH
        if (at) {
            // Centred on the drop, then pulled inside the canvas so a drop at
            // the very edge cannot land the item half off the slide.
            item.x = Math.max(0, Math.min(mCanvas.width - item.width,
                                           Math.round(at.x - item.width / 2)))
            item.y = Math.max(0, Math.min(mCanvas.height - item.height,
                                           Math.round(at.y - item.height / 2)))
        }
        return item
    }
    readonly property var baseContentTypes: [
        { kind: "text",   icon: "text",   label: "Text" },
        { kind: "bus",    icon: "activity", label: "Bus" },
        { kind: "media",  icon: "media",  label: "Media" },
        { kind: "shape",  icon: "shape",  label: "Shape" },        { kind: "timer",  icon: "timer",  label: "Timer" },
        { kind: "clock",  icon: "clock",  label: "Clock" },
        { kind: "metadata", icon: "textLines", label: "Metadata" }
    ]
    readonly property var mediaKinds: [
        { kind: "image", icon: "image",  label: "Picture" },
        { kind: "video", icon: "play",   label: "Video" },
        { kind: "audio", icon: "music",  label: "Audio" }
    ]
    readonly property var shownContentTypes: root.mediaMenuOpen
        ? root.mediaKinds.concat([{ kind: "mediaBack", icon: "arrowLeft", label: "Back" }])
        : root.contentTypes
    readonly property var itemKindLook: ({
        image: { icon: "image", label: "Picture" },
        video: { icon: "play",  label: "Video" },
        audio: { icon: "music", label: "Audio" }
    })
    function kindInfo(kind) {
        if (root.itemKindLook[kind] !== undefined)
            return root.itemKindLook[kind]
        for (let i = 0; i < root.contentTypes.length; ++i) {
            if (root.contentTypes[i].kind === kind)
                return root.contentTypes[i]
        }
        return { icon: "?", label: kind }
    }

    // How the extra block kinds a library declares are shown in the Add menu (which kinds exist is the ENGINE's call -
    // DesignLibraryConfig::extraBlockKinds, via the service; this is only their icon and label).
    readonly property var extraKindLook: ({
        vignette: { icon: "layers",         label: "Vignette" },
        corners:  { icon: "layoutTemplate", label: "Corners" }
    })
    // What the Add menu offers: the usual kinds, plus whatever the open design's library allows on top of them.
    readonly property var contentTypes: {
        const service = root.designMode ? showSession.designService : null
        if (!service)
            return root.baseContentTypes
        const extra = service.extraBlockKinds.map((k) => Object.assign({ kind: k, icon: "shape", label: k }, root.extraKindLook[k] ?? {}))
        return root.baseContentTypes.concat(extra)
    }

    // ---- Middle toolbar ----
    Rectangle {
        id: mHdr
        x: root.slidePanelWidth
        y: 48
        height: 48
        // Everything between the two side panels (the window is no longer a fixed 1440 wide).
        width: root.centerRegionWidth
        color: Theme.surface

        Row {
            id: mHdrLeft
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14

            Rectangle {
                id: mBack
                anchors.verticalCenter: parent.verticalCenter
                height: 28
                width: 28
                color: Theme.activeBg
                radius: 7

                Text {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    text: "‹"
                }
                // In a design (overlay / template) this is Done: back to the library it came from.
                MouseArea {
                    anchors.fill: parent
                    enabled: root.designMode
                    cursorShape: root.designMode ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        root.closeDesign()
                        root.designClosed()
                    }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.Medium
                // (the design's name follows the library: DesignBackend re-reads it whenever it changes)
                text: root.designMode ? (showSession.designBackend.currentShow.name ?? "") : qsTr("Sunday Service")
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 104
                border.color: Qt.alpha(Theme.accent, 0.30)
                border.width: 1
                color: Qt.alpha(Theme.accent, 0.14)
                radius: 6

                Text {
                    anchors.centerIn: parent
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    text: root.designMode ? (showSession.designKind === "template" ? qsTr("Template") : qsTr("Overlay"))
                                          : qsTr("Template · Worship")
                }
            }
        }

        Row {
            id: mHdrRight
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 16

            Text {
                id: saveLabelText
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10
                // A design (overlay / template) has no Save: every settled edit is written to its library.
                visible: !root.designMode
                text: qsTr("Not saved yet")
                function applySave(autosave) { text = autosave ? qsTr("Autosaved ✓") : qsTr("Saved ✓") }
                Component.onCompleted: {
                    const recent = EngineBridge.recentEngineEvents(200)
                    for (let i = recent.length - 1; i >= 0; --i)
                        if (recent[i].topic === "project.saved") {
                            applySave(recent[i].autosave)
                            break
                        }
                }
            }
            Text {
                visible: root.designMode
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10
                text: qsTr("Saves as you edit")
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: fitArea.containsMouse ? Theme.textSecondary : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10
                text: qsTr("Fit")
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    id: fitArea
                    anchors.fill: parent
                    anchors.margins: -6
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = 1
                }
            }
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Rectangle {
                    height: 28
                    width: 28
                    radius: 7
                    enabled: canvasHistory.canUndo
                    opacity: enabled ? 1 : 0.35
                    color: undoArea.containsMouse ? Theme.hoverBg : Theme.inset
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text { anchors.centerIn: parent; color: Theme.textSecondary; font.pixelSize: 14; text: "↺" }

                    MouseArea {
                        id: undoArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.undo()
                    }
                }
                Rectangle {
                    height: 28
                    width: 28
                    radius: 7
                    enabled: canvasHistory.canRedo
                    opacity: enabled ? 1 : 0.35
                    color: redoArea.containsMouse ? Theme.hoverBg : Theme.inset
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text { anchors.centerIn: parent; color: Theme.textSecondary; font.pixelSize: 14; text: "↻" }

                    MouseArea {
                        id: redoArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.redo()
                    }
                }
                Rectangle {
                    height: 28
                    width: 34
                    radius: 7
                    color: zoomResetArea.containsMouse ? Theme.hoverBg : Theme.inset
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        text: Math.round(root.canvasZoom * 100) + "%"
                    }

                    MouseArea {
                        id: zoomResetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.canvasZoom = 1
                    }
                }
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: !root.designMode
            Repeater {
                model: [
                    { id: "slide", label: qsTr("Slide") },
                    { id: "text", label: qsTr("Text edit") }
                ]
                delegate: Rectangle {
                    required property var modelData
                    width: modelData.id === "slide" ? 76 : 94
                    height: 30
                    radius: 6
                    color: root.editWorkspaceMode === modelData.id ? Theme.activeBg : "transparent"
                    border.color: root.editWorkspaceMode === modelData.id ? Theme.accent : Theme.borderSubtle
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: root.editWorkspaceMode === modelData.id ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: modelData.id === "text" ? root.enterTextEditMode() : root.leaveTextEditMode()
                    }
                }
            }
        }

        Connections {
            target: EngineBridge
            function onEngineEvent(topic, payload) {
                if (topic === "project.saved")
                    saveLabelText.applySave(payload && payload.autosave === true)
            }
        }
    }
    Rectangle {
        x: root.slidePanelWidth
        y: 96
        height: 1
        width: root.centerRegionWidth
        color: Theme.border
    }

    Rectangle {
        id: textEditWorkspace
        x: root.slidePanelWidth
        y: 97
        width: root.centerRegionWidth
        height: root.height - y
        visible: root.editWorkspaceMode === "text" && root.hasActiveSlide
        color: Theme.panelBg
        border.color: Theme.border
        clip: true
        Flickable {
            anchors.fill: parent
            contentWidth: width
            contentHeight: Math.max(height, textEditArea.implicitHeight + 48)
            clip: true
            TextEdit {
                id: textEditArea
                x: 24
                y: 20
                width: Math.max(0, parent.width - 48)
                text: root.textEditBuffer
                color: Theme.textPrimary
                selectionColor: Qt.alpha(Theme.accent, 0.45)
                selectedTextColor: Theme.textPrimary
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                font.family: FontService.monoFamily
                font.pixelSize: 15
                onTextChanged: {
                    if (!root.loadingTextEditBuffer) {
                        root.textEditBuffer = text
                        root.textEditBufferDirty = true
                        textEditApplyTimer.restart()
                    }
                }
            }
        }
        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            text: root.textEditStatus || qsTr("Use [#1], [#2] markers to edit separate text boxes")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 10
            opacity: 0.8
        }
        Timer {
            id: textEditApplyTimer
            interval: 550
            repeat: false
            onTriggered: root.startTextEditApply(false)
        }
        Timer {
            id: textEditWorkTimer
            interval: 50
            repeat: false
            onTriggered: root.finishTextEditApply()
        }
        Connections {
            target: ShowService
            function onDocumentJobFinished(token, kind, ok, error, payload) {
                if (kind !== "textEditBatch" || token !== root.textEditApplyToken)
                    return
                const switchToSlide = root.textEditSwitchAfterApply
                root.textEditApplyToken = 0
                if (ok) {
                    EngineBridge.log("info", "Text edit", "Installing returned slide data for job " + token)
                    showSession.applyTextEditResult(payload ? payload.slides ?? [] : [])
                    root.textEditStatus = qsTr("Text items updated")
                    EngineBridge.log("info", "Text edit",
                                     "Job " + token + " completed; returned slides="
                                     + ((payload && payload.slides) ? payload.slides.length : 0)
                                     + ", new items requested="
                                     + (payload ? payload.requestedNewItems ?? 0 : 0)
                                     + ", created=" + (payload ? payload.createdNewItems ?? 0 : 0))
                    if (root.textEditBuffer === root.textEditSubmittedBuffer)
                        root.textEditBufferDirty = false
                    if (switchToSlide)
                        root.editWorkspaceMode = "slide"
                } else {
                    root.textEditStatus = qsTr("Couldn't apply the text edits. Your text is still here.")
                    EngineBridge.log("warning", "Text edit", "Job " + token + " failed: " + String(error))
                    if (error)
                        EngineBridge.log("warning", "Text edit", String(error))
                    root.textEditBufferDirty = true
                    if (switchToSlide)
                        root.editWorkspaceMode = "slide"
                }
                root.textEditApplying = false
                root.textEditSwitchAfterApply = false
                if (root.textEditBuffer !== root.textEditSubmittedBuffer)
                    textEditApplyTimer.restart()
            }
            function onDocumentJobCancelled(token, kind) {
                if (kind !== "textEditBatch" || token !== root.textEditApplyToken)
                    return
                const switchToSlide = root.textEditSwitchAfterApply
                root.textEditApplyToken = 0
                root.textEditApplying = false
                root.textEditSwitchAfterApply = false
                root.textEditStatus = qsTr("Text update cancelled. Your text is still here.")
                root.textEditBufferDirty = true
                if (switchToSlide)
                    root.editWorkspaceMode = "slide"
            }
        }
        ModalScrim {
            visible: root.textEditApplying
            z: 20
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 320)
                height: 92
                radius: Theme.radiusLg
                color: Theme.surface
                border.color: Theme.border

                BusySpinner {
                    x: 22
                    anchors.verticalCenter: parent.verticalCenter
                    running: root.textEditApplying
                    color: Theme.accent
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 64
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Updating show text…")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    // ---- Canvas ----
    // The stage keeps its native editing coordinates; its display scale fits
    // the available center region without changing saved item geometry.
    Rectangle {
        id: mCanvas
        visible: root.editWorkspaceMode === "slide"

        DropArea {
            id: metadataDrop
            anchors.fill: parent
            keys: ["app-drag"]
            onEntered: (drag) => {
                const p = drag.source ? drag.source.payload : null
                drag.accepted = !!p && p.kind === "metadata"
            }
            onDropped: (drop) => {
                const p = drop.source ? drop.source.payload : null
                if (p && p.kind === "metadata" && p.items && p.items.length > 0)
                    root.addMetadataItem(p.items[0].ref, { x: drop.x, y: drop.y })
            }
        }
        objectName: "selfTestCanvas"
        x: root.slidePanelWidth + (root.centerRegionWidth - width * scale) / 2
        y: (root.height - height * scale) / 2 + 72
        height: StageGrid.h
        width: StageGrid.w
        clip: true
        transformOrigin: Item.TopLeft
        scale: root.canvasZoom * root.canvasFitScale
        enabled: root.hasActiveSlide
        color: "#141519"

        Checkerboard {
            id: checkerGrid
            anchors.fill: parent
            // Cell size is the board's own (13px, FreeShow's) — stated once in
            // Checkerboard.qml so no surface can drift to its own value.
            hostScale: root.canvasZoom * root.canvasFitScale
        }

        Repeater {
            model: 13 * 8
            delegate: Rectangle {
                required property int index
                x: (index % 13) * 56 + 2
                y: Math.floor(index / 13) * 56 + 14
                width: 2
                height: 2
                radius: 1
                color: Theme.border
            }
        }

        // The slide background layer is declared INSIDE slideFilterHost below (with
        // every canvas item), because it is part of the slide the filter set paints.

        focus: true
        Keys.onPressed: (event) => {
            if (root.selectedCanvasObjects.length === 0)
                return

            if (event.key === Qt.Key_Escape) {
                root.selectedCanvasObjects = []
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                root.deleteSelectedCanvasObjects()
                event.accepted = true
                return
            }

            const step = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) ? 10 : 1
            let dx = 0, dy = 0
            if (event.key === Qt.Key_Left) dx = -step
            else if (event.key === Qt.Key_Right) dx = step
            else if (event.key === Qt.Key_Up) dy = -step
            else if (event.key === Qt.Key_Down) dy = step
            else return

            root.nudgeSelectedCanvasObjects(dx, dy)
            event.accepted = true
        }

        MouseArea {
            id: canvasBackgroundArea
            anchors.fill: parent
            z: -1
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            function syncCursor() {
                if (visible && enabled && AppCursor.hovered(canvasBackgroundArea))
                    AppCursor.push(Qt.ArrowCursor, canvasBackgroundArea, AppCursor.priorityFallback)
                else
                    AppCursor.pop(canvasBackgroundArea)
            }
            // Recompute from the pointer-position stream (every move) instead
            // of onContainsMouseChanged — same mechanism as PositionHoverArea.
            Connections {
                target: AppCursor
                function onPointerMoved() { canvasBackgroundArea.syncCursor() }
            }
            onEnabledChanged: syncCursor()
            Component.onDestruction: AppCursor.pop(canvasBackgroundArea)
            onClicked: {
                // A press on empty canvas is "done choosing" — the metadata palette
                // closes with it, the same way the + menu does.
                root.metadataPaletteOpen = false
                mCanvas.forceActiveFocus()
                root.selectedCanvasObjects = []
            }
        }

        SnapGuideLines {
            id: canvasSnapGuides
        }

        // ---- the slide itself, as ONE filtered picture ----------------------
        Item {
            id: slideFilterHost
            anchors.fill: parent
            readonly property bool canShader: GraphicsInfo.api !== GraphicsInfo.Software
            layer.enabled: root.canvasFiltersActive && slideFilterHost.canShader
            layer.effect: SlideFilterEffect {
                filters: slideStore.current.filters
                stageWidth: StageGrid.w
                stageHeight: StageGrid.h
            }
            Rectangle {
                id: slideBgLayer
                anchors.fill: parent
                color: slideStore.current.background
            }
            Repeater {
                id: canvasItemsRepeater
                model: slideStore.current.items
                delegate: DraggableCanvasText {
                    id: canvasItemObject
                    required property var modelData
                    objectName: "canvasItem_" + modelData.key
                    property alias textLabel: itemTextLabel
                    BusMonitor {
                        id: busMonitor
                        busRow: canvasItemObject.modelData.kind === "bus"
                            ? busMonitor.rowForBusId(canvasItemObject.modelData.meta
                                                     ? (canvasItemObject.modelData.meta.busId ?? 0) : 0)
                            : -1
                    }
                    x: modelData.x
                    y: modelData.y
                    width: modelData.width
                    height: modelData.height
                    style: modelData.style
                    // A circle / line / glyph shape, a vignette and screen corners draw their own body (see below); the box behind
                    // every other kind is the item's fill and border.
                    drawsOwnBody: modelData.kind === "vignette" || modelData.kind === "corners" || modelData.kind === "bus"
                        || modelData.kind === "image" || modelData.kind === "video"
                        || (modelData.kind === "shape" && ["circle", "line", "triangle", "arrow", "star", "hexagon"].includes(modelData.meta.shapeType ?? "rectangle"))
                    selected: root.isCanvasObjectSelected(modelData.key)
                    contentHovered: modelData.kind === "text" ? itemTextLabel.hovered
                        : modelData.kind === "camera" ? camDragArea.containsMouse
                        : modelData.kind === "clock" ? clockDragArea.containsMouse
                        : modelData.kind === "timer" ? timerDragArea.containsMouse
                        : modelData.kind === "shape" ? shapeDragArea.containsMouse
                        : modelData.kind === "bus" ? busDragArea.containsMouse
                        : modelData.kind === "image" || modelData.kind === "video" ? mediaDragArea.containsMouse
                        : placeholderDragArea.containsMouse
                    onSelectedRequested: (mods) => root.handleCanvasSelect(modelData.key, mods)
                    onResizing: (geom) => root.applyCanvasResize(modelData.key, geom)
                    onMoving: (dx, dy, snapDisabled) => root.applyCanvasMove(modelData.key, dx, dy, snapDisabled)
                    onDragEnded: root.endCanvasDrag()

                    EditableCanvasLabel {
                        id: itemTextLabel
                        selected: canvasItemObject.selected
                        readonly property var tmeta: canvasItemObject.modelData.meta
                        bandColor: String(itemTextLabel.tmeta.lineBg ?? "")
                        bandCase: itemTextLabel.tmeta.textCase ?? "none"
                        // ...and the band's corner radius (the Lines card's "Line
                        // radius"): 0 keeps Qt's per-fragment rich-text bands.
                        bandRadius: MetadataFields.lineRadius(itemTextLabel.tmeta)
                        Text {
                            id: labelLineBox
                            visible: false
                            text: "Xg"
                            font: itemTextLabel.font
                            lineHeight: 1
                            lineHeightMode: Text.ProportionalHeight
                        }
                        readonly property string autoSizeMode: {
                            const v = itemTextLabel.tmeta.autoSize
                            return v === "shrink" ? "shrinkToFit"
                                : v === "grow" ? "growToFit"
                                : v === "shrinkToFit" || v === "growToFit" || v === "none" ? v
                                : "none"
                        }

                        // ---- the font size actually rendered: an explicit solver ----
                        clip: MetadataFields.isToken(itemTextLabel.shownText)

                        readonly property real baseFontSize: itemTextLabel.tmeta.fontSize ?? 16
                        property real measureSize: baseFontSize     // the twin's current probe size
                        property real fittedSize: baseFontSize      // the solver's answer
                        property bool solving: false
                        readonly property var fitInputs: [
                            itemTextLabel.shownText, itemTextLabel.width,
                            canvasItemObject.modelData.height, canvasItemObject.modelData.width,
                            autoSizeMode, baseFontSize,
                            itemTextLabel.tmeta.fontFamily, itemTextLabel.tmeta.fontWeight, itemTextLabel.tmeta.bold,
                            itemTextLabel.tmeta.italic, itemTextLabel.tmeta.textCase,
                            MetadataFields.letterSpacing(itemTextLabel.tmeta),
                            MetadataFields.wordSpacing(itemTextLabel.tmeta),
                            MetadataFields.oneLine(itemTextLabel.tmeta),
                            MetadataFields.lineHeight(itemTextLabel.tmeta),
                            MetadataFields.lineSpacing(itemTextLabel.tmeta)
                        ]
                        onFitInputsChanged: if (!solving)
                            Qt.callLater(itemTextLabel.recomputeFit)
                        Component.onCompleted: itemTextLabel.recomputeFit()
                        function recomputeFit() {
                            if (solving)
                                return
                            solving = true
                            const pad = canvasItemObject.modelData.style ? (canvasItemObject.modelData.style.padding ?? 0) : 0
                            const bw = canvasItemObject.modelData.width - pad * 2
                            const bh = canvasItemObject.modelData.height - pad * 2
                            if (bw <= 0 || bh <= 0) {
                                fittedSize = baseFontSize
                            } else if (MetadataFields.isToken(itemTextLabel.shownText)
                                       && autoSizeMode !== "growToFit") {
                                fittedSize = baseFontSize
                                measureSize = baseFontSize
                            } else {
                                let lo = 3, hi = 400
                                for (let i = 0; i < 11; ++i) {
                                    const mid = (lo + hi) / 2
                                    measureSize = mid
                                    if (fitMeasure.contentWidth <= bw && fitMeasure.contentHeight <= bh)
                                        lo = mid
                                    else
                                        hi = mid
                                }
                                // growToFit fills the box; shrinkToFit/none cap at the set size.
                                fittedSize = autoSizeMode === "growToFit" ? lo : Math.min(baseFontSize, lo)
                                measureSize = fittedSize
                            }
                            solving = false
                        }

                        Text {
                            id: fitMeasure
                            visible: false
                            width: itemTextLabel.width
                            text: itemTextLabel.shownText          // what is on screen, list markers included
                            font: MetadataFields.textFont(itemTextLabel.tmeta, itemTextLabel.measureSize)
                            lineHeight: MetadataFields.lineHeightWithGap(itemTextLabel.tmeta, fitLineBox.contentHeight)
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: MetadataFields.oneLine(itemTextLabel.tmeta) ? Text.NoWrap : Text.WordWrap

                            // The probe size's own line box — same measurement as
                            // labelLineBox, at this twin's pixelSize.
                            Text {
                                id: fitLineBox
                                visible: false
                                text: "Xg"
                                font: fitMeasure.font
                                lineHeight: 1
                                lineHeightMode: Text.ProportionalHeight
                            }
                        }

                        visible: canvasItemObject.modelData.kind === "text"
                        color: itemTextLabel.tmeta.color ?? "#f2f4fa"
                        font: MetadataFields.textFont(itemTextLabel.tmeta, fittedSize)
                        fontSizeMode: Text.FixedSize
                        wrapMode: MetadataFields.oneLine(itemTextLabel.tmeta) ? Text.NoWrap : Text.WordWrap
                        lineHeight: MetadataFields.lineHeightWithGap(itemTextLabel.tmeta, labelLineBox.contentHeight)
                        horizontalAlignment: {
                            const a = itemTextLabel.tmeta.align ?? "center"
                            return a === "left" ? Text.AlignLeft
                                : a === "right" ? Text.AlignRight
                                : a === "justify" ? Text.AlignJustify
                                : Text.AlignHCenter
                        }
                        verticalAlignment: {
                            const a = itemTextLabel.tmeta.verticalAlign ?? "center"
                            return a === "top" ? Text.AlignTop
                                : a === "bottom" ? Text.AlignBottom
                                : Text.AlignVCenter
                        }
                        text: canvasItemObject.modelData.text
                        // The list style (Text tab): every line a list item, marked by the ENGINE's formatting - display only.
                        listStyle: itemTextLabel.tmeta.list ?? ""
                        onEdited: (value) => {
                            root.beginTextEdit(canvasItemObject.modelData.key)
                            canvasItemObject.modelData.text = value
                        }
                        onCommitted: (value) => {
                            canvasItemObject.modelData.text = value
                            root.endTextEdit()
                        }
                        onEditingChanged: {
                            root.anyTextEditing = itemTextLabel.editing
                            if (!itemTextLabel.editing)
                                root.endTextEdit()
                        }
                        onSelectRequested: (mods, x, y) => root.handleCanvasSelect(canvasItemObject.modelData.key, mods,
                                                                  itemTextLabel.mapToItem(mCanvas, x, y))
                        onMoveRequested: (dx, dy, snapDisabled) => {
                            if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                            root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                        }
                        onDragEnded: root.endCanvasDrag()
                        onUndoRedoRequested: (undo) => Qt.callLater(undo ? root.undo
                                                                         : root.redo)
                    }

                    Item {
                        id: camContent
                        visible: canvasItemObject.modelData.kind === "camera"
                        anchors.fill: parent

                        Shape {
                            anchors.fill: parent
                            ShapePath {
                                fillGradient: LinearGradient {
                                    x1: camContent.width * 0.5; x2: camContent.width * 0.5
                                    y1: 0; y2: camContent.height
                                    GradientStop { color: "#ff123326"; position: 0 }
                                    GradientStop { color: "#ff07130e"; position: 1 }
                                }
                                strokeColor: "#000"
                                strokeWidth: 0
                                PathRectangle { width: camContent.width; height: camContent.height; radius: 8 }
                            }
                        }
                        Rectangle { x: 8; y: 8; width: 6; height: 6; radius: 3; color: Theme.danger }   // the LIVE dot (red = live; the accent sweep only recolored SELECTIONS)
                        Text {
                            x: 18; y: 7
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 8
                            font.weight: Font.Medium
                            text: qsTr("LIVE")
                        }
                        Rectangle { x: 10; y: 24; height: 34; width: 36; color: "#14503a"; radius: 4 }
                        Rectangle { x: 30; y: 24; height: 34; width: 24; color: "#0f3a2c"; radius: 4 }
                        Rectangle { x: 96; y: 10; width: 8; height: 8; radius: 4; color: "#0b1f17"; border.width: 1; border.color: "#1d5c46" }   // the lens
                        Text {
                            x: 10; y: 68
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.Medium
                            text: canvasItemObject.modelData.text.length > 0 ? canvasItemObject.modelData.text : qsTr("Camera")
                        }
                        Text {
                            x: 68; y: 68
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            text: qsTr("10:24:07")
                        }

                        CanvasDragArea {
                            id: camDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    Item {
                        id: clockContent
                        readonly property var cfg: canvasItemObject.modelData.meta
                        readonly property bool hour12: clockContent.cfg.format !== "24"
                        readonly property bool showSeconds: clockContent.cfg.showSeconds !== false
                        readonly property bool showDate: clockContent.cfg.showDate === true
                        readonly property bool analog: clockContent.cfg.style === "analog"
                        // The colour picked in Add Clock (digits and hands); unset keeps the original accent look.
                        readonly property bool tinted: clockContent.cfg.color !== undefined
                        // No background = a bare clock: the analog face loses its dark disc too.
                        readonly property bool bare: canvasItemObject.style !== null && canvasItemObject.style.backgroundColor.a === 0
                        visible: canvasItemObject.modelData.kind === "clock"
                        anchors.fill: parent

                        LiveClock {
                            id: clockTicker
                            running: clockContent.visible
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: canvasItemObject.style ? canvasItemObject.style.cornerRadius : 8
                            // Exactly the item's own background/border: a clock with a transparent background is just its digits
                            // (the Add Clock picker offers a dark box or none; the palette changes it afterwards).
                            color: canvasItemObject.style ? canvasItemObject.style.backgroundColor : "#12131a"
                            border.color: canvasItemObject.style ? canvasItemObject.style.borderColor : Theme.borderSubtle
                            border.width: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                                ? canvasItemObject.style.borderWidth : 0
                        }

                        Column {
                            visible: !clockContent.analog
                            anchors.centerIn: parent
                            spacing: 2

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                color: clockContent.tinted ? clockContent.cfg.color : Theme.accentLight
                                font.family: Theme.fontFamily
                                font.weight: Font.DemiBold
                                font.pixelSize: Math.max(10, Math.min(clockContent.width, clockContent.height) * 0.22)
                                text: clockTicker.formatClock(clockTicker.now, clockContent.hour12, clockContent.showSeconds)
                            }
                            Text {
                                visible: clockContent.showDate
                                anchors.horizontalCenter: parent.horizontalCenter
                                color: clockContent.tinted ? Qt.alpha(clockContent.cfg.color, 0.7) : Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                text: Qt.formatDate(clockTicker.now, "dddd, MMMM d")
                            }
                        }

                        Item {
                            visible: clockContent.analog
                            anchors.centerIn: parent
                            width: Math.min(clockContent.width, clockContent.height) * 0.8
                            height: width

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: clockContent.bare ? "transparent" : "#12131a"
                                border.color: clockContent.tinted ? clockContent.cfg.color : Theme.borderSubtle
                                border.width: 2
                            }
                            Rectangle {
                                width: 4
                                height: parent.height * 0.24
                                radius: 2
                                color: clockContent.tinted ? clockContent.cfg.color : Theme.textPrimary
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.verticalCenter
                                transformOrigin: Item.Bottom
                                rotation: (clockTicker.now.getHours() % 12 + clockTicker.now.getMinutes() / 60) * 30
                            }
                            Rectangle {
                                width: 3
                                height: parent.height * 0.34
                                radius: 1.5
                                color: clockContent.tinted ? clockContent.cfg.color : Theme.textSecondary
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.verticalCenter
                                transformOrigin: Item.Bottom
                                rotation: (clockTicker.now.getMinutes() + clockTicker.now.getSeconds() / 60) * 6
                            }
                            Rectangle {
                                visible: clockContent.showSeconds
                                width: 1.5
                                height: parent.height * 0.4
                                color: clockContent.tinted ? clockContent.cfg.color : Theme.accent
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.verticalCenter
                                transformOrigin: Item.Bottom
                                rotation: clockTicker.now.getSeconds() * 6
                            }
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 3
                                color: clockContent.tinted ? clockContent.cfg.color : Theme.accent
                            }
                        }

                        CanvasDragArea {
                            id: clockDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    Item {
                        id: timerContent
                        readonly property var cfg: canvasItemObject.modelData.meta
                        readonly property string mode: timerContent.cfg.mode ?? "countdown"
                        readonly property real durationSeconds: timerContent.cfg.durationSeconds ?? 300
                        readonly property real startedAt: timerContent.cfg.startedAt ?? Date.now()
                        visible: canvasItemObject.modelData.kind === "timer"
                        anchors.fill: parent

                        LiveClock {
                            id: timerTicker
                            running: timerContent.visible
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: canvasItemObject.style ? canvasItemObject.style.cornerRadius : 8
                            color: (canvasItemObject.style && canvasItemObject.style.backgroundColor.a > 0)
                                ? canvasItemObject.style.backgroundColor : "#12131a"
                            border.color: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                                ? canvasItemObject.style.borderColor : Theme.borderSubtle
                            border.width: (canvasItemObject.style && canvasItemObject.style.borderEnabled)
                                ? canvasItemObject.style.borderWidth : 1
                        }

                        Text {
                            anchors.centerIn: parent
                            color: Theme.accentLight
                            font.family: Theme.fontFamily
                            font.weight: Font.DemiBold
                            font.pixelSize: Math.max(10, Math.min(timerContent.width, timerContent.height) * 0.28)
                            text: timerTicker.formatDuration(
                                timerTicker.timerSeconds(timerTicker.now, timerContent.mode, timerContent.durationSeconds, timerContent.startedAt))
                        }

                        CanvasDragArea {
                            id: timerDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    Item {
                        id: shapeContent
                        readonly property var cfg: canvasItemObject.modelData.meta
                        readonly property string shapeType: shapeContent.cfg.shapeType ?? "rectangle"
                        readonly property var st: canvasItemObject.style
                        // The fill is exactly the item's background: "transparent" (picked in the colour palette) draws nothing,
                        // so outline-only frames work. A NEW shape is given a visible fill when it is added (shapeSourceModal).
                        readonly property color fillColor: shapeContent.st ? shapeContent.st.backgroundColor : "#3a3f55"
                        readonly property color strokeColor: shapeContent.st ? shapeContent.st.borderColor : Theme.accent
                        readonly property real strokeWidth: (shapeContent.st && shapeContent.st.borderEnabled) ? shapeContent.st.borderWidth : 0
                        readonly property bool isCircle: shapeContent.shapeType === "circle"
                        readonly property bool isLine: shapeContent.shapeType === "line"
                        readonly property bool isGlyph: ["triangle", "arrow", "star", "hexagon"].includes(shapeContent.shapeType)
                        readonly property bool isBox: !shapeContent.isCircle && !shapeContent.isLine && !shapeContent.isGlyph
                        visible: canvasItemObject.modelData.kind === "shape"
                        anchors.fill: parent

                        Rectangle {
                            visible: shapeContent.isBox
                            anchors.fill: parent
                            radius: shapeContent.st ? shapeContent.st.cornerRadius : 0
                            color: shapeContent.fillColor
                            border.color: shapeContent.strokeColor
                            border.width: shapeContent.strokeWidth
                        }

                        Rectangle {
                            visible: shapeContent.isCircle
                            anchors.centerIn: parent
                            width: Math.min(parent.width, parent.height)
                            height: width
                            radius: width / 2
                            color: shapeContent.fillColor
                            border.color: shapeContent.strokeColor
                            border.width: shapeContent.strokeWidth
                        }

                        // line — thin horizontal bar at vertical center.
                        Rectangle {
                            visible: shapeContent.isLine
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                            height: Math.max(2, shapeContent.strokeWidth || 3)
                            color: shapeContent.fillColor
                        }

                        Text {
                            visible: shapeContent.isGlyph
                            anchors.centerIn: parent
                            color: shapeContent.fillColor
                            font.pixelSize: Math.min(parent.width, parent.height) * 0.7
                            text: shapeContent.shapeType === "triangle" ? "▲"
                                : shapeContent.shapeType === "arrow" ? "→"
                                : shapeContent.shapeType === "star" ? "★"
                                : "⬡"
                        }

                        CanvasDragArea {
                            id: shapeDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    // Vignette / corners (overlay designs): drawn by the SAME DesignPreview the library cards use, so the canvas and the
                    // card can never disagree about what they look like.
                    Item {
                        id: screenContent
                        visible: canvasItemObject.modelData.kind === "vignette" || canvasItemObject.modelData.kind === "corners"
                        anchors.fill: parent

                        DesignPreview {
                            anchors.fill: parent
                            blocks: [{
                                kind: canvasItemObject.modelData.kind, x: 0, y: 0, width: StageGrid.w, height: StageGrid.h,
                                meta: canvasItemObject.modelData.meta,
                                style: { backgroundColor: canvasItemObject.style.backgroundColor.toString() }
                            }]
                        }

                        CanvasDragArea {
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    // ---- BUS: the same live tile the picker showed ------------
                    Rectangle {
                        id: busBody
                        visible: canvasItemObject.modelData.kind === "bus"
                        anchors.fill: parent
                        radius: 6
                        color: "#000000"
                        clip: true

                        readonly property var bmeta: canvasItemObject.modelData.meta
                        readonly property var bmedia: busBody.bmeta && busBody.bmeta.media
                                                     ? busBody.bmeta.media : ({})
                        readonly property var bcrop: busBody.bmedia.crop
                                                     || ({ top: 0, right: 0, bottom: 0, left: 0 })
                        readonly property var bfilter: busBody.bmedia.filter || ({})
                        readonly property bool flipH: busBody.bmedia.flipH === true
                        readonly property bool flipV: busBody.bmedia.flipV === true
                        readonly property real cropSum: (busBody.bcrop.top ?? 0)
                            + (busBody.bcrop.right ?? 0) + (busBody.bcrop.bottom ?? 0)
                            + (busBody.bcrop.left ?? 0)
                        readonly property bool cropActive: busBody.cropSum > 0
                        readonly property bool filterActive: {
                            const f = busBody.bfilter
                            return (f.hueRotate ?? 0) !== 0 || (f.invert ?? 0) !== 0 || (f.blur ?? 0) !== 0
                                || (f.grayscale ?? 0) !== 0 || (f.sepia ?? 0) !== 0
                                || (f.brightness ?? 1) !== 1 || (f.contrast ?? 1) !== 1
                                || (f.saturate ?? 1) !== 1 || (f.opacity ?? 1) !== 1
                        }
                        readonly property string fit: canvasItemObject.style
                            ? String(canvasItemObject.style.mediaFit) : "contain"
                        readonly property bool canShader: GraphicsInfo.api !== GraphicsInfo.Software

                        property string heldLabel: ""
                        readonly property string wantedLabel: busBody.visible ? busMonitor.videoLabel : ""
                        onWantedLabelChanged: busBody.syncPreview()
                        function syncPreview() {
                            if (busBody.heldLabel !== "" && busBody.heldLabel !== busBody.wantedLabel) {
                                LiveOutputService.endPreviewInput(busBody.heldLabel)
                                busBody.heldLabel = ""
                            }
                            const want = busBody.wantedLabel
                            if (want === "" || LiveOutputService.cardPreviews.indexOf(want) >= 0)
                                return
                            LiveOutputService.previewInput(want, busMonitor.videoKind,
                                busMonitor.videoSource.mode !== undefined ? busMonitor.videoSource.mode : "")
                            if (LiveOutputService.cardPreviews.indexOf(want) >= 0)
                                busBody.heldLabel = want
                        }
                        Component.onCompleted: busBody.syncPreview()
                        Component.onDestruction: {
                            if (busBody.heldLabel !== "")
                                LiveOutputService.endPreviewInput(busBody.heldLabel)
                        }
                        // A source that was not there (a closed window, an unplugged camera)
                        // is asked for again every couple of seconds until it can be held.
                        Timer {
                            interval: 2000
                            repeat: true
                            running: busBody.wantedLabel !== "" && busBody.heldLabel === ""
                            onTriggered: busBody.syncPreview()
                        }

                        // BLUR FILL: the feed fills the window softened and the
                        // sharp copy sits on top — the picture's own fourth fit.
                        Image {
                            anchors.fill: parent
                            visible: busBody.fit === "blur" && busMonitor.previewing
                            source: busMonitor.previewSource
                            fillMode: Image.PreserveAspectCrop
                            cache: false
                            asynchronous: false
                            layer.enabled: visible && busBody.canShader
                            layer.effect: SlideFilterEffect {
                                filters: ({ blur: 24, brightness: 0.85 })
                                stageWidth: StageGrid.w
                                stageHeight: StageGrid.h
                            }
                        }

                        Item {
                            id: busFeedLayer
                            anchors.fill: parent
                            opacity: busBody.bfilter.opacity ?? 1
                            layer.enabled: busBody.filterActive && busBody.canShader
                            layer.effect: SlideFilterEffect {
                                filters: busBody.bfilter
                                stageWidth: StageGrid.w
                                stageHeight: StageGrid.h
                            }

                            Image {
                                id: busFeed
                                anchors.fill: parent
                                source: busMonitor.previewSource
                                cache: false
                                asynchronous: false
                                visible: busMonitor.previewing
                                // FIT, mapped the way a picture's is (blur keeps the
                                // sharp feed whole over its softened backdrop).
                                fillMode: busBody.fit === "cover" ? Image.PreserveAspectCrop
                                        : busBody.fit === "fill" ? Image.Stretch
                                        : Image.PreserveAspectFit
                                // CROP: Qt crops the source, then fits what is left.
                                sourceClipRect: {
                                    if (!busBody.cropActive || busFeed.sourceSize.width <= 1)
                                        return Qt.rect(0, 0, 0, 0)
                                    const c = busBody.bcrop
                                    const w = busFeed.sourceSize.width
                                    const h = busFeed.sourceSize.height
                                    const l = (c.left ?? 0) / 100
                                    const r = (c.right ?? 0) / 100
                                    const t = (c.top ?? 0) / 100
                                    const b = (c.bottom ?? 0) / 100
                                    return Qt.rect(w * l, h * t, w * (1 - l - r), h * (1 - t - b))
                                }
                                mirror: busBody.flipH
                                transform: Scale {
                                    origin.x: busFeed.width / 2
                                    origin.y: busFeed.height / 2
                                    xScale: 1
                                    yScale: busBody.flipV ? -1 : 1
                                }
                            }
                        }

                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            visible: !busMonitor.previewing

                            IconGlyph {
                                name: "activity"
                                color: Theme.textMuted
                                width: 20; height: 20
                                fit: true
                            }
                            Text {
                                text: busMonitor.busType === "audio" ? qsTr("audio only") : qsTr("No signal")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                            }
                        }

                        // The light: green connected · amber offline/starved · red
                        // nothing routed. Same three colours as the pane's card.
                        Rectangle {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 6
                            width: 9; height: 9; radius: 4.5
                            color: busMonitor.statusColor
                            border.width: 1
                            border.color: Qt.alpha("#000000", 0.45)
                        }

                        // The name rides the picture's bottom edge.
                        Rectangle {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            width: Math.min(parent.width - 12, busNameText.implicitWidth + 12)
                            height: busNameText.implicitHeight + 6
                            radius: 4
                            color: Qt.alpha("#000000", 0.55)

                            Text {
                                id: busNameText
                                anchors.centerIn: parent
                                text: canvasItemObject.modelData.text
                                color: "#ffffff"
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                width: Math.min(implicitWidth, busBody.width - 24)
                            }
                        }

                        CanvasDragArea {
                            id: busDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    Rectangle {
                        id: mediaPictureBody
                        readonly property var mmeta: canvasItemObject.modelData.meta
                        readonly property string src: mediaPictureBody.mmeta ? (mediaPictureBody.mmeta.src ?? "") : ""
                        readonly property var mmedia: mediaPictureBody.mmeta && mediaPictureBody.mmeta.media
                                                     ? mediaPictureBody.mmeta.media : ({})
                        readonly property var mcrop: mediaPictureBody.mmedia.crop
                                                     || ({ top: 0, right: 0, bottom: 0, left: 0 })
                        readonly property var mfilter: mediaPictureBody.mmedia.filter || ({})
                        readonly property bool flipH: mediaPictureBody.mmedia.flipH === true
                        readonly property bool flipV: mediaPictureBody.mmedia.flipV === true
                        readonly property real cropSum: (mediaPictureBody.mcrop.top ?? 0)
                            + (mediaPictureBody.mcrop.right ?? 0) + (mediaPictureBody.mcrop.bottom ?? 0)
                            + (mediaPictureBody.mcrop.left ?? 0)
                        readonly property bool cropActive: mediaPictureBody.cropSum > 0
                        readonly property bool filterActive: {
                            const f = mediaPictureBody.mfilter
                            return (f.hueRotate ?? 0) !== 0 || (f.invert ?? 0) !== 0 || (f.blur ?? 0) !== 0
                                || (f.grayscale ?? 0) !== 0 || (f.sepia ?? 0) !== 0
                                || (f.brightness ?? 1) !== 1 || (f.contrast ?? 1) !== 1
                                || (f.saturate ?? 1) !== 1 || (f.opacity ?? 1) !== 1
                        }
                        readonly property string fit: canvasItemObject.style ? canvasItemObject.style.mediaFit : "contain"
                        readonly property bool canShader: GraphicsInfo.api !== GraphicsInfo.Software
                        visible: canvasItemObject.modelData.kind === "image" || canvasItemObject.modelData.kind === "video"
                        anchors.fill: parent
                        color: "transparent"
                        clip: true

                        Image {
                            anchors.fill: parent
                            visible: mediaPictureBody.fit === "blur" && mediaPicture.status === Image.Ready
                            asynchronous: true
                            cache: false
                            fillMode: Image.PreserveAspectCrop
                            source: root.mediaPictureUrl(mediaPictureBody.src)
                            layer.enabled: visible && mediaPictureBody.canShader
                            layer.effect: SlideFilterEffect {
                                filters: ({ blur: 24, brightness: 0.85 })
                                stageWidth: StageGrid.w
                                stageHeight: StageGrid.h
                            }
                        }

                        Item {
                            id: mediaPictureLayer
                            anchors.fill: parent
                            opacity: mediaPictureBody.mfilter.opacity ?? 1
                            layer.enabled: mediaPictureBody.filterActive && mediaPictureBody.canShader
                            layer.effect: SlideFilterEffect {
                                filters: mediaPictureBody.mfilter
                                stageWidth: StageGrid.w
                                stageHeight: StageGrid.h
                            }

                            Image {
                                id: mediaPicture
                                anchors.fill: parent
                                asynchronous: true
                                cache: false
                                fillMode: mediaPictureBody.fit === "cover" ? Image.PreserveAspectCrop
                                        : mediaPictureBody.fit === "fill" ? Image.Stretch
                                        : Image.PreserveAspectFit
                                sourceClipRect: {
                                    if (!mediaPictureBody.cropActive || mediaPicture.sourceSize.width <= 1)
                                        return Qt.rect(0, 0, 0, 0)
                                    const c = mediaPictureBody.mcrop
                                    const w = mediaPicture.sourceSize.width
                                    const h = mediaPicture.sourceSize.height
                                    const l = (c.left ?? 0) / 100
                                    const r = (c.right ?? 0) / 100
                                    const t = (c.top ?? 0) / 100
                                    const b = (c.bottom ?? 0) / 100
                                    return Qt.rect(w * l, h * t, w * (1 - l - r), h * (1 - t - b))
                                }
                                mirror: mediaPictureBody.flipH
                                transform: Scale {
                                    origin.x: mediaPicture.width / 2
                                    origin.y: mediaPicture.height / 2
                                    xScale: 1
                                    yScale: mediaPictureBody.flipV ? -1 : 1
                                }
                                source: root.mediaPictureUrl(mediaPictureBody.src)
                                visible: status === Image.Ready && sourceSize.width > 1
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            width: Math.min(parent.width - 12, mediaNameText.implicitWidth + 12)
                            height: mediaNameText.implicitHeight + 6
                            radius: 4
                            color: Qt.alpha("#000000", 0.55)
                            visible: canvasItemObject.modelData.text.length > 0

                            Text {
                                id: mediaNameText
                                anchors.centerIn: parent
                                text: canvasItemObject.modelData.text
                                color: "#ffffff"
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                width: Math.min(implicitWidth, mediaPictureBody.width - 24)
                            }
                        }

                        IconGlyph {
                            anchors.centerIn: parent
                            visible: canvasItemObject.modelData.kind === "video"
                                     && mediaPicture.status !== Image.Ready
                            name: "play"
                            color: Theme.accentLight
                            width: 26
                            height: 26
                        }

                        CanvasDragArea {
                            id: mediaDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }

                    Rectangle {
                        id: genericPlaceholder
                        readonly property var typeInfo: root.kindInfo(canvasItemObject.modelData.kind)
                        visible: !["text", "camera", "clock", "timer", "shape", "vignette", "corners", "bus",
                                   "image", "video"].includes(canvasItemObject.modelData.kind)
                        anchors.fill: parent
                        color: Theme.inset
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: 6

                        Column {
                            anchors.centerIn: parent
                            spacing: 4

                        Item {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 16
                            width: 20
                            height: 20

                            Text {
                                anchors.fill: parent
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                visible: genericPlaceholder.typeInfo.icon === "text"
                                color: Theme.accentLight
                                font.pixelSize: 18
                                text: "Aa"
                            }
                            IconGlyph {
                                anchors.centerIn: parent
                                visible: genericPlaceholder.typeInfo.icon !== "text"
                                name: genericPlaceholder.typeInfo.icon
                                color: Theme.accentLight
                                width: 16
                                height: 16
                            }
                        }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                text: genericPlaceholder.typeInfo.label.toUpperCase()
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                visible: canvasItemObject.modelData.text.length > 0
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                text: canvasItemObject.modelData.text
                            }
                        }

                        CanvasDragArea {
                            id: placeholderDragArea
                            anchors.fill: parent
                            onMoved: (dx, dy, snapDisabled) => {
                                if (!root.isCanvasObjectSelected(canvasItemObject.modelData.key))
                                    root.handleCanvasSelect(canvasItemObject.modelData.key, 0)
                                root.applyCanvasMove(canvasItemObject.modelData.key, dx, dy, snapDisabled)
                            }
                            onDragFinished: root.endCanvasDrag()
                            onTapped: (mouse) => root.handleCanvasSelect(canvasItemObject.modelData.key, mouse.modifiers,
                                                         canvasItemObject.mapToItem(mCanvas, mouse.x, mouse.y))
                        }
                    }
                }
            }
        }

        // ---- Right-click opens the canvas menu ----------------------------
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onPressed: (mouse) => {
                root.openCanvasMenu(mouse.x, mouse.y, Qt.point(mouse.x, mouse.y))
                mouse.accepted = true
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: "#ffffff"
            border.width: 2
        }
    }

    Rectangle {
        x: mCanvas.x
        y: mCanvas.y
        width: mCanvas.width * mCanvas.scale
        height: mCanvas.height * mCanvas.scale
        visible: !root.hasActiveSlide
        color: Qt.alpha(Theme.panelBg, 0.69)

        Column {
            anchors.centerIn: parent
            spacing: 12

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 15
                text: qsTr("No slides yet")
            }
            Text {
                width: 300
                anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
                wrapMode: Text.Wrap
                text: qsTr("Add a slide to start building — then place text, cameras and more on the canvas")
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                height: 36
                width: 140
                radius: 18
                color: emptyAddArea.containsMouse ? Qt.lighter(Theme.accent, 1.08) : Theme.accent
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    color: "#ffffff"
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    text: qsTr("+ Add slide")
                }

                MouseArea {
                    id: emptyAddArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: showSession.addSlide()
                }
            }
        }
    }

    Rectangle {
        x: rightPanel.x - 158
        y: root.height - 59
        height: 32
        width: 124
        border.color: Theme.border
        border.width: 1
        color: Theme.inset
        radius: 8

        Row {
            anchors.fill: parent
            Text {
                width: 41; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomOutArea.containsMouse ? Theme.textSecondary : Theme.textSecondary
                font.pixelSize: 14
                text: "−"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                MouseArea {
                    id: zoomOutArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = Math.max(0.5, Math.round((root.canvasZoom - 0.1) * 10) / 10)
                }
            }
            Text {
                width: 42; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomLabelArea.containsMouse ? Theme.textSecondary : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                text: Math.round(root.canvasZoom * 100) + "%"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                MouseArea {
                    id: zoomLabelArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = 1
                }
            }
            Text {
                width: 41; height: parent.height
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: zoomInArea.containsMouse ? Theme.textSecondary : Theme.textSecondary
                font.pixelSize: 14
                text: "+"
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                MouseArea {
                    id: zoomInArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.canvasZoom = Math.min(2, Math.round((root.canvasZoom + 0.1) * 10) / 10)
                }
            }
        }
    }

    Rectangle {
        id: addContentChip
        objectName: "selfTestAddChip"
        x: root.slidePanelWidth + 6
        y: root.height - 62
        height: 46
        width: 46
        radius: 23
        color: Theme.accent
        // Nothing to add content TO without an active slide — dim and inert.
        enabled: root.hasActiveSlide
        opacity: root.hasActiveSlide ? 1 : 0.35

        // A "+" rotated 45 degrees reads as an "X": the open state is the same glyph turned, not a different character.
        PlusGlyph {
            anchors.centerIn: parent
            size: 18; thickness: 2.2; color: "#ffffff"
            rotation: root.addMenuOpen ? 45 : 0
            Behavior on rotation { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                root.addMenuOpen = !root.addMenuOpen
                root.metadataPaletteOpen = false   // the + chip owns both panels
            }
        }
    }

    Rectangle {
        id: addContentMenu
        objectName: "selfTestAddMenu"
        x: 344
        y: root.height - 63
        height: 46
        // 7 chips x 48 + 6 gaps x 4 = 360 plus 14 of padding a side; an overlay has two more chips.
        width: contentTypeRow.width + 28
        border.color: Theme.border
        border.width: 1
        color: Theme.surface
        radius: 14
        visible: root.addMenuOpen

        Row {
            id: contentTypeRow
            anchors.centerIn: parent
            spacing: 4

            Repeater {
                // The ring's CURRENT step: the usual kinds, or the three Media
                // kinds with a way back (see shownContentTypes).
                model: root.shownContentTypes
                delegate: Rectangle {
                    id: typeChip
                    required property var modelData
                    required property int index
                    readonly property bool active: chipHover.hoveredIndex === index
                    objectName: "selfTestAddKindChip_" + modelData.kind

                    height: 34
                    width: 48
                    radius: 8
                    border.width: typeChip.active ? 1 : 0
                    border.color: Qt.alpha(Theme.accent, 0.25)
                    color: typeChip.active ? Qt.alpha(Theme.accent, 0.13) : Theme.inset

                    Text {
                        y: 3
                        height: 14
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: typeChip.active ? Theme.accentLight : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        // "text" keeps its Aa letters; every other kind
                        // renders its real IconGlyph (see contentTypes above).
                        text: typeChip.modelData.icon === "text" ? "Aa"
                            : typeChip.modelData.icon
                        visible: typeChip.modelData.icon === "text"
                    }
                    IconGlyph {
                        y: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        name: typeChip.modelData.icon
                        color: typeChip.active ? Theme.accentLight : Theme.textSecondary
                        visible: typeChip.modelData.icon !== "text"
                        width: 14
                        height: 14
                        transformOrigin: Item.Center
                    }
                    Text {
                        y: 19
                        height: 12
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: typeChip.active ? Theme.textPrimary : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 9
                        text: typeChip.modelData.label
                    }
                }
            }
        }

        ZoneHoverArea {
            id: chipHover
            anchors.fill: parent
            container: contentTypeRow
            zoneRow: contentTypeRow
            cursorShape: Qt.PointingHandCursor
            onClicked: (mouse) => {
                if (mouse.y < 6 || mouse.y > 40)
                    return
                if (hoveredIndex < 0)
                    return
                const kind = root.shownContentTypes[hoveredIndex].kind
                if (kind === "media") {
                    root.mediaMenuOpen = true
                    return
                }
                if (kind === "mediaBack") {
                    root.mediaMenuOpen = false
                    return
                }
                if (kind === "image" || kind === "video" || kind === "audio") {
                    // The picker is what makes the item: it needs a FILE, and
                    // a media item without one is a card with no content.
                    root.addMenuOpen = false
                    // The ring ADDS: never inherit a replace key from an earlier Change.
                    root.mediaReplaceKey = ""
                    root.mediaPickKind = kind
                    root.mediaModalOpen = true
                    return
                }
                root.addMenuOpen = false
                if (kind === "bus")
                    root.busModalOpen = true
                else if (kind === "timer")
                    root.timerModalOpen = true
                else if (kind === "clock")
                    root.clockModalOpen = true
                else if (kind === "shape")
                    root.shapeModalOpen = true
                else if (kind === "vignette" || kind === "corners")
                    root.addScreenTreatment(kind)
                else if (kind === "metadata") {
                    root.metadataPaletteOpen = true
                    return
                } else if (kind === "text")
                    root.addTextItem()
                else
                    root.addCanvasItem(kind)
            }
        }
    }

    // ---- While the metadata palette is open, it OWNS the pointer ------------
    function syncMetadataBlocker() {
        if (root.metadataPaletteOpen)
            AppCursor.pushBlocker(metadataPalette)
        else
            AppCursor.popBlocker(metadataPalette)
    }
    onMetadataPaletteOpenChanged: root.syncMetadataBlocker()
    Component.onDestruction: {
        AppCursor.popBlocker(metadataPalette)
        Hotkeys.close(root.panelHotkeyScope)
    }

    MouseArea {
        id: metadataCatcher
        objectName: "selfTestMetadataCatcher"
        anchors.fill: parent
        z: metadataPalette.z - 1
        visible: root.metadataPaletteOpen
        enabled: visible
        acceptedButtons: Qt.AllButtons
        hoverEnabled: false
        onPressed: root.metadataPaletteOpen = false
        // A wheel outside the panel dismisses it too, the way the app's menus
        // behave: scrolling the canvas under an open popup is never intended.
        onWheel: (wheel) => {
            wheel.accepted = true
            root.metadataPaletteOpen = false
        }
    }

    // ---- The metadata palette (the + menu's Metadata chip) ----------------
    Rectangle {
        id: metadataPalette
        objectName: "selfTestMetadataPalette"
        x: 344
        y: root.height - 63 - height - 8
        width: 560
        height: paletteCol.height + 24
        z: 1200
        radius: Theme.radiusXl
        color: Theme.card
        border.color: Qt.alpha(Theme.accent, 0.22)
        border.width: 1
        visible: root.metadataPaletteOpen

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 3
            border.color: Qt.alpha(Theme.accent, 0.10)
            z: -1
        }
        // The lit top edge every raised card in this app wears.
        Rectangle {
            x: parent.radius
            y: 0
            width: parent.width - parent.radius * 2
            height: 1
            color: Qt.alpha(Theme.accentLight, 0.35)
        }

        MouseArea {
            id: paletteBackdrop
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.ArrowCursor
        }

        Column {
            id: paletteCol
            x: 14
            y: 12
            width: metadataPalette.width - 28
            spacing: 10

            Item {
                width: paletteCol.width
                height: 15

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 7

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 7
                        radius: 3.5
                        color: Theme.accent
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("METADATA")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        font.letterSpacing: 1.2
                        font.bold: true
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("click to add · drag onto the canvas")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                }
            }
            Rectangle {
                width: paletteCol.width
                height: 1
                color: Qt.alpha(Theme.border, 0.8)
            }

            Repeater {
                model: MetadataFields.groups
                delegate: Column {
                    id: metaGroup
                    required property var modelData
                    readonly property var groupFields: modelData.fields
                    width: paletteCol.width
                    spacing: 6

                    Row {
                        spacing: 7

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 3; height: 10
                            radius: 1.5
                            color: Qt.alpha(Theme.accent, 0.85)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: metaGroup.modelData.name
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.letterSpacing: 1.0
                            font.bold: true
                        }
                    }

                    Flow {
                        width: parent.width
                        spacing: 7

                        Repeater {
                            model: groupFields
                            delegate: Rectangle {
                                id: metaChip
                                required property var modelData
                                readonly property string label: String(modelData.label)
                                readonly property string token: String(modelData.token)
                                objectName: "selfTestMetaChip_" + metaChip.token.replace(/[{}]/g, "")

                                width: Math.max(metaLabelText.implicitWidth, metaTokenPill.width) + 22
                                height: 40
                                radius: Theme.radiusMd
                                color: metaHover.hovered ? Qt.alpha(Theme.accent, 0.16)
                                                         : Qt.alpha(Theme.chip, 0.45)
                                border.width: 1
                                border.color: metaHover.hovered ? Qt.alpha(Theme.accent, 0.55)
                                                                : Qt.alpha(Theme.borderSubtle, 0.85)
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                                Rectangle {
                                    visible: metaHover.hovered
                                    x: 0
                                    y: 9
                                    width: 3
                                    height: metaChip.height - 18
                                    radius: 1.5
                                    color: Theme.accent
                                }

                                Text {
                                    id: metaLabelText
                                    x: 11
                                    y: 5
                                    text: metaChip.label
                                    color: metaHover.hovered ? Theme.accentLight : Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }
                                Rectangle {
                                    id: metaTokenPill
                                    x: 11
                                    y: 21
                                    height: 14
                                    width: metaTokenText.implicitWidth + 10
                                    radius: 4
                                    color: Qt.alpha(Theme.windowBg, 0.6)

                                    Text {
                                        id: metaTokenText
                                        anchors.centerIn: parent
                                        text: metaChip.token
                                        color: Theme.textMuted
                                        font.family: FontService.monoFamily
                                        font.pixelSize: 9
                                    }
                                }

                                DragSource {
                                    anchors.fill: parent
                                    label: metaChip.label
                                    payload: ({
                                        kind: "metadata",
                                        items: [ { ref: metaChip.token, name: metaChip.label } ]
                                    })
                                    onActivated: root.addMetadataItem(metaChip.token)
                                }
                                PositionHoverArea {
                                    id: metaHover
                                    anchors.fill: parent
                                    showCursor: true
                                    cursorShape: Qt.PointingHandCursor
                                    clickEnabled: false
                                    onEntered: root.metadataHint = metaChip.modelData.hint
                                    onExited: if (root.metadataHint === metaChip.modelData.hint) root.metadataHint = ""
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: paletteCol.width
                height: 28          // two lines of 10px, reserved whether used or not
                radius: Theme.radiusMd
                color: Qt.alpha(Theme.inset, 0.85)

                Row {
                    x: 9
                    width: parent.width - 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 7

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 5; height: 5
                        radius: 2.5
                        color: Theme.textMuted
                    }
                    Text {
                        width: parent.width - 12
                        text: root.metadataHint
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // ---- Left panel: slide list ----
    Rectangle {
        id: leftPanel
        y: 48
        height: root.height - 48
        width: root.slidePanelWidth
        border.color: Theme.border
        border.width: 1
        color: Theme.panelBg

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.ArrowCursor
        }

        Text {
            x: 12
            y: 12
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 12
            text: qsTr("SLIDES · %1").arg(slideListRepeater.count)
        }

        Flickable {
            id: slideListFlick
            x: 12
            y: 30
            width: leftPanel.width - 24
            height: leftPanel.height - 30 - 12
            clip: true
            contentWidth: width
            contentHeight: slideListColumn.height
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true

        Column {
            id: slideListColumn
            width: parent.width
            spacing: 8

            Repeater {
                id: slideListRepeater
                model: slideModel
                delegate: SlideListItem {
                    width: slideListFlick.width
                    previewItems: slideStore.items(slideId)
                    previewBackground: slideStore.backgroundOf(slideId)
                    onSelected: {
                        slideListFlick.forceActiveFocus()
                        slideModel.selectSlide(index)
                    }
                    onDuplicateRequested: showSession.duplicateSlide(index)
                    onDeleteRequested: showSession.removeSlide(index)
                    onContextMenuRequested: (mx, my) => {
                        // The menu belongs to this row, so the row is what the
                        // keys act on: same focus hand-off as a left click.
                        slideListFlick.forceActiveFocus()
                        root.openSlideMenu(index, this, mx, my)
                    }
                }
            }

            Rectangle {
                id: addSlideButton
                // A design (overlay / template) is one canvas: there is nothing to add.
                visible: !root.designMode
                height: 36
                width: slideListFlick.width
                border.color: Theme.borderSubtle
                border.width: 1
                color: addSlideArea.containsMouse ? Theme.hoverBg : Theme.inset
                radius: 8
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    Text { color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.Medium; text: "+" }
                    Text { color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13; text: qsTr("Add slide") }
                }

                MouseArea {
                    id: addSlideArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: showSession.addSlide()
                }
            }
        }
        }

        AppScrollBar {
            x: leftPanel.width - width - 2
            y: slideListFlick.y
            height: slideListFlick.height
            flickable: slideListFlick
        }
    }

    // ---- Right panel ----
    Rectangle {
        id: rightPanel
        x: root.width - root.inspectorPanelWidth
        y: 48
        height: root.height - 48
        width: root.inspectorPanelWidth
        color: Theme.windowBg
        // Greyed with the canvas in the empty state — Background/Size & Style
        // target the active slide, which doesn't exist yet.
        enabled: root.hasActiveSlide
        opacity: root.hasActiveSlide ? 1 : 0.35

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.ArrowCursor
        }

        Item {
            id: rightTabRow
            x: 0
            width: parent.width
            // One label's metrics; all four share them (same font, same caps),
            // so the strip's height is the items label's.
            height: tabItemsLabel.implicitHeight
            readonly property bool textShown: root.textTabAvailable
            readonly property bool mediaShown: root.mediaTabAvailable
            readonly property int visibleCount: 2 + (textShown ? 1 : 0) + (mediaShown ? 1 : 0)
            readonly property real cellWidth: width / visibleCount
            readonly property int textSlot: 1
            readonly property int mediaSlot: 1 + (textShown ? 1 : 0)
            readonly property int slideSlot: 1 + (textShown ? 1 : 0) + (mediaShown ? 1 : 0)
            y: rightPanel.tabRowTop

            Text {
                id: tabItemsLabel
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "items" ? Theme.accent : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
                text: qsTr("ITEMS")
                // ITEMS always leads the strip, so its slot is fixed.
                x: 0
                width: rightTabRow.cellWidth
                Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.rightPanelTab = "items"; root.rightTabPinned = true }
                }
            }
            Text {
                id: tabTextLabel
                visible: opacity > 0
                opacity: rightTabRow.textShown ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                x: rightTabRow.textSlot * rightTabRow.cellWidth
                width: rightTabRow.cellWidth
                Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "text" ? Theme.accent : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium // constant — see tabItemsLabel above
                text: qsTr("TEXT")
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    enabled: rightTabRow.textShown
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.rightPanelTab = "text"; root.rightTabPinned = false }
                }
            }
            Text {
                id: tabMediaLabel
                // Offered only for a selected picture / video / audio item —
                // faded in and out exactly like TEXT.
                visible: opacity > 0
                opacity: rightTabRow.mediaShown ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                x: rightTabRow.mediaSlot * rightTabRow.cellWidth
                width: rightTabRow.cellWidth
                Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "media" ? Theme.accent : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium // constant — see tabItemsLabel above
                text: qsTr("MEDIA")
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    // See the TEXT label: no clicks on a fading-out cell.
                    enabled: rightTabRow.mediaShown
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.rightPanelTab = "media"; root.rightTabPinned = false }
                }
            }
            Text {
                id: tabSlideLabel
                x: rightTabRow.slideSlot * rightTabRow.cellWidth
                width: rightTabRow.cellWidth
                Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                horizontalAlignment: Text.AlignHCenter
                color: root.rightPanelTab === "slide" ? Theme.accent : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium // constant — see tabItemsLabel above
                text: qsTr("SLIDE")
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.rightPanelTab = "slide"; root.rightTabPinned = true }
                }
            }
        }
        // ---- THE PANEL'S ONE VERTICAL CHAIN ---------------------------------
        readonly property real itemsWallTop: 12
        // Air between the previews and the strip: enough to read as two bands,
        // small enough that the previews still own the top of the panel.
        readonly property real itemsWallGap: 16
        // Air between the strip's hairline and the first card.
        readonly property real tabContentGap: 12
        readonly property real tabUnderlineY: rightTabRow.y + rightTabRow.height + 5
        readonly property real tabRuleY: tabUnderlineY + tabUnderline.height
        readonly property real tabContentTop: Math.round(tabRuleY + rightPanel.tabContentGap)
        // The strip's top, derived from the wall's SCALED height (see
        // itemsWallHeight for why scaling is not the same as height).
        readonly property real tabRowTop: rightPanel.itemsWallTop + rightPanel.itemsWallHeight + rightPanel.itemsWallGap

        // ---- WHAT IS LEFT FOR THE WALL AND THE CARDS -------------------------
        readonly property real itemsSectionMinHeight: 150
        readonly property real itemsWallScaleFloor: 0.55
        readonly property real itemsWallScale: {
            const natural = itemsMonitorWall.height
            if (natural <= 0)
                return 1
            // The strip's own height, measured (row → 5px gap → hairline →
            // gap), so a larger UI scale cannot silently eat the floor below.
            const stripHeight = rightTabRow.height + 5 + tabUnderline.height + rightPanel.tabContentGap
            const budget = rightPanel.height - rightPanel.itemsWallTop - rightPanel.itemsWallGap
                - stripHeight - rightPanel.itemsSectionMinHeight - 12
            if (budget >= natural)
                return 1
            return Math.max(rightPanel.itemsWallScaleFloor, budget / natural)
        }
        readonly property real itemsWallHeight: itemsMonitorWall.height * rightPanel.itemsWallScale

        Rectangle {
            id: tabUnderline
            y: rightPanel.tabUnderlineY
            height: 3
            radius: 1.50
            color: Theme.accent
            readonly property var activeLabel: root.rightPanelTab === "text" && root.textTabAvailable ? tabTextLabel
                : root.rightPanelTab === "media" && root.mediaTabAvailable ? tabMediaLabel
                : root.rightPanelTab === "slide" ? tabSlideLabel : tabItemsLabel
            x: rightTabRow.x + activeLabel.x + activeLabel.width / 2 - width / 2
            width: activeLabel.implicitWidth
            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
            Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
        }
        Rectangle { y: rightPanel.tabRuleY; height: 1; width: parent.width; color: Theme.border }

        MonitorWall {
            id: itemsMonitorWall
            x: 12
            y: rightPanel.itemsWallTop
            width: rightPanel.width - 24
            transformOrigin: Item.Top
            scale: rightPanel.itemsWallScale
            showClearControls: false
        }

        // The tab's sections scroll UNDER the strip.
        Flickable {
            id: itemsTabFlick
            x: 0
            // Below the strip (which is itself below the wall), so the cards
            // follow the previews instead of being pinned to a guess.
            y: rightPanel.tabContentTop
            width: parent.width
            height: Math.max(0, rightPanel.height - y - 12)
            visible: root.rightPanelTab === "items"
            clip: true
            contentWidth: width
            contentHeight: itemsTabContent.height
            boundsBehavior: Flickable.StopAtBounds

            Item {
                id: itemsTabContent
                width: itemsTabFlick.width

                readonly property int inset: 12
                readonly property int rowWidth: width - inset * 2

        Text {
            id: slideSectionLabel
            x: itemsTabContent.inset + 4
            y: 8
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            text: qsTr("SLIDE")
        }
        Rectangle {
            id: backgroundRow
            x: itemsTabContent.inset
            y: slideSectionLabel.y + slideSectionLabel.height + 8
            height: 46
            width: itemsTabContent.rowWidth
            color: Theme.card
            radius: 8
            enabled: root.primarySelectedSupportsFill
            opacity: root.primarySelectedSupportsFill ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 14
                text: root.primarySelectedItemStyle ? qsTr("Item Background") : qsTr("Background")
            }
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 104
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 5
                color: root.primarySelectedItemStyle ? root.primarySelectedItemStyle.backgroundColor : slideStore.current.background
            }
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 26
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                border.color: Theme.borderSubtle
                border.width: 1
                color: changeArea.containsMouse ? Theme.hoverBg : Theme.inset
                radius: 12
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    text: qsTr("Change")
                }

                MouseArea {
                    id: changeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openBackgroundPicker(root.primarySelectedItemStyle ? "itemBackground" : "background")
                }
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 16
                text: "›"
            }
        }

        // ---- WINDOW BUS lives in the MEDIA tab -----------------------------

        TransformCard {
            id: transformCard
            x: itemsTabContent.inset
            // The geometry cards follow the Background row (the window's bus row
            // is not part of this column any more — it is the MEDIA tab's).
            y: backgroundRow.y + backgroundRow.height + 10
            width: itemsTabContent.rowWidth
            targets: root.selectedCanvasObjects.map((k) => root.canvasObjectByKey(k)?.style).filter((s) => s !== null && s !== undefined)
            undoHook: root.pushUndoSnapshot
            focusHook: function () { mCanvas.forceActiveFocus() }
        }

        SizeStyleCard {
            id: sizeStyleCard
            x: itemsTabContent.inset
            y: transformCard.y + transformCard.height + 10
            width: itemsTabContent.rowWidth
            targets: transformCard.targets
            fillSupported: root.primarySelectedSupportsFill
            undoHook: root.pushUndoSnapshot
            focusHook: function () { mCanvas.forceActiveFocus() }
            onChangeBorderRequested: root.openBackgroundPicker("border")
            onChangeBorderStyleRequested: {
                const field = sizeStyleCard.borderStyleFieldItem
                borderStyleMenu.openAt(field, 0, field.height + 2, root)
            }
        }

        FiltersCard {
            id: filtersCard
            x: itemsTabContent.inset
            y: sizeStyleCard.y + sizeStyleCard.height + 10
            width: itemsTabContent.rowWidth
            targets: sizeStyleCard.targets
            mediaFitSupported: root.primarySelectedItem !== null
                && (root.primarySelectedItem.kind === "image" || root.primarySelectedItem.kind === "video")
            fitHook: (fit) => root.applyMediaFit(fit)
            undoHook: root.pushUndoSnapshot
            focusHook: function () { mCanvas.forceActiveFocus() }
        }

                height: filtersCard.y + filtersCard.height + itemsTabContent.inset
            }
        }

        AppScrollBar {
            id: itemsTabScrollBar
            // Sits in the tab's 12px right gutter (the content columns stop at
            // 12 short of the panel width, so the bar never lies over a row).
            x: itemsTabFlick.width - 10
            y: itemsTabFlick.y
            height: itemsTabFlick.height
            flickable: itemsTabFlick
            visible: itemsTabFlick.visible && itemsTabFlick.contentHeight > itemsTabFlick.height
        }

        Flickable {
            id: textPanelFlick
            x: 8
            y: rightPanel.tabContentTop
            width: parent.width - 16
            height: rightPanel.height - rightPanel.tabContentTop - 12
            visible: root.rightPanelTab === "text" && root.primaryIsText
            clip: true
            contentWidth: width
            contentHeight: textItemPanel.height
            boundsBehavior: Flickable.StopAtBounds

            TextItemPanel {
                id: textItemPanel
                width: textPanelFlick.width - 12
                target: textPanelFlick.visible ? root.primarySelectedItem : null
                undoHook: root.pushUndoSnapshot
                focusHook: function () { mCanvas.forceActiveFocus() }
                onChangeColorRequested: {
                    root.openBackgroundPicker("textColor")
                }
                onChangeFontRequested: {
                    const field = textItemPanel.weightFieldItem
                    textFontMenu.openAt(field, 0, field.height + 2, root)
                }
                onChangeFontFamilyRequested: {
                    const field = textItemPanel.fontFamilyFieldItem
                    textFontFamilyMenu.openAt(field, 0, field.height + 2, root)
                }
                onChangeAutoSizeRequested: {
                    const field = textItemPanel.autoSizeFieldItem
                    textAutoSizeMenu.openAt(field, 0, field.height + 2, root)
                }
                onChangeTextCaseRequested: {
                    const field = textItemPanel.textTransformFieldItem
                    textCaseMenu.openAt(field, 0, field.height + 2, root)
                }
                // The Lines card's "Background color" row: the SAME shared picker,
                // aimed at the per-LINE band instead of the item's box.
                onChangeLineBackgroundRequested: {
                    root.openBackgroundPicker("lineBackground")
                }
            }
        }

        AppScrollBar {
            id: textPanelScrollBar
            x: textPanelFlick.x + textPanelFlick.width - 4
            y: textPanelFlick.y
            height: textPanelFlick.height
            flickable: textPanelFlick
            visible: textPanelFlick.visible && textPanelFlick.contentHeight > textPanelFlick.height
        }

        Text {
            x: 8
            y: rightPanel.tabContentTop
            width: parent.width - 16
            wrapMode: Text.Wrap
            visible: root.rightPanelTab === "text" && !(root.primarySelectedItem !== null && root.primarySelectedItem.kind === "text")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
            text: qsTr("Select a text item on the canvas to edit its formatting.")
        }

        Flickable {
            id: mediaItemFlick
            x: 8
            y: rightPanel.tabContentTop
            width: parent.width - 16
            height: rightPanel.height - rightPanel.tabContentTop - 12
            visible: root.rightPanelTab === "media" && root.primaryMediaItem !== null
            clip: true
            contentWidth: width
            contentHeight: mediaItemPanel.height
            boundsBehavior: Flickable.StopAtBounds

            MediaItemPanel {
                id: mediaItemPanel
                width: mediaItemFlick.width - 12
                item: mediaItemFlick.visible ? root.primaryMediaItem : null
                busName: root.primaryIsWindow ? root.windowBusName() : ""
                undoHook: root.pushUndoSnapshot
                focusHook: function () { mCanvas.forceActiveFocus() }
                onChangeBusRequested: root.windowBusModalOpen = true
                onChangeFileRequested: root.pickReplacementMedia()
                onFitFieldTapped: mediaFitMenu.openAt(mediaItemPanel.fitFieldItem, 0,
                                                      mediaItemPanel.fitFieldItem.height + 2)
                onBlendFieldTapped: mediaBlendMenu.openAt(mediaItemPanel.blendFieldItem, 0,
                                                          mediaItemPanel.blendFieldItem.height + 2)
                onBlendPicked: (blend) => root.setMediaMeta({ blend: blend })
                onClearFileRequested: root.clearItemFile()
                onFitPicked: (fit) => root.applyMediaFit(fit)
                onFlagChanged: (key, on) => root.setMediaFlag(key, on)
                onCropEdited: (side, percent) => root.setMediaCrop(side, percent)
                onResetCropRequested: root.resetMediaCrop()
                onFilterEdited: (key, value) => root.setMediaFilter(key, value)
                onResetFiltersRequested: root.resetMediaFilters()
            }
        }

        AppScrollBar {
            id: mediaItemScrollBar
            x: mediaItemFlick.x + mediaItemFlick.width - 4
            y: mediaItemFlick.y
            height: mediaItemFlick.height
            flickable: mediaItemFlick
            visible: mediaItemFlick.visible && mediaItemFlick.contentHeight > mediaItemFlick.height
        }

        Text {
            x: 8
            y: rightPanel.tabContentTop
            width: parent.width - 16
            wrapMode: Text.Wrap
            visible: root.rightPanelTab === "media" && !root.primaryIsMedia
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
            text: qsTr("Select a picture, video, audio or bus window on the canvas to edit it.")
        }

        Flickable {
            id: slidePanelFlick
            x: 8
            y: rightPanel.tabContentTop
            width: parent.width - 16
            height: rightPanel.height - rightPanel.tabContentTop - 12
            visible: root.rightPanelTab === "slide"
            clip: true
            contentWidth: width
            contentHeight: slidePanel.height
            boundsBehavior: Flickable.StopAtBounds

            SlidePanel {
                id: slidePanel
                width: slidePanelFlick.width - 12
                items: slideStore.current.items
                selectedKeys: root.selectedCanvasObjects
                kinds: root.contentTypes
                backgroundColor: slideStore.current.background
                onBackgroundPickRequested: root.openBackgroundPicker("background")
                onRowClicked: (key, modifiers) => root.handleCanvasSelect(key, modifiers)
                onMoveRequested: (key, direction) => root.moveCanvasItem(key, direction)
                // The slide's filter set — the same map the canvas host and the
                // engine's on-air pass read.
                filters: slideStore.current.filters
                undoHook: root.pushUndoSnapshot
                onFilterChanged: (key, value) => root.setSlideFilter(key, value)
                onFiltersResetRequested: root.resetSlideFilters()
            }
        }

        AppScrollBar {
            x: slidePanelFlick.x + slidePanelFlick.width - 4
            y: slidePanelFlick.y
            height: slidePanelFlick.height
            flickable: slidePanelFlick
            visible: slidePanelFlick.visible && slidePanelFlick.contentHeight > slidePanelFlick.height
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: slideContextMenu.visible || textFontMenu.visible || textFontFamilyMenu.visible || textAutoSizeMenu.visible || textCaseMenu.visible || borderStyleMenu.visible
        onClicked: {
            slideContextMenu.close()
            textFontMenu.visible = false
            textFontFamilyMenu.visible = false
            textAutoSizeMenu.visible = false
            textCaseMenu.visible = false
            borderStyleMenu.visible = false
        }
    }

    SlideContextMenu {
        id: canvasMenu
        onChosen: (id, arg) => root.canvasMenuPicked(id)
    }

    NameDialog {
        id: itemTimerDialog
        property string itemKey: ""
        property string action: ""
        title: qsTr("Seconds")
        placeholder: qsTr("0 = off")
        confirmLabel: qsTr("Set")
        allowEmpty: true
        onAccepted: (text) => {
            const seconds = Math.max(0, Math.min(3600, Number(text) || 0))
            root.setItemAction(itemTimerDialog.itemKey, itemTimerDialog.action, seconds)
        }
    }

    // The seconds a slide stays up before the show moves on by itself.
    NameDialog {
        id: slideTimerDialog
        parent: Window.window ? Window.window.contentItem : null
        z: 30000
        property string slideId: ""
        title: qsTr("Next timer, in seconds")
        placeholder: qsTr("0 = wait for the operator")
        confirmLabel: qsTr("Set")
        allowEmpty: true
        onAccepted: (text) => {
            const seconds = Math.max(0, Math.min(3600, Number(text) || 0))
            if (slideTimerDialog.slideId !== "")
                ShowService.updateSlide(slideTimerDialog.slideId, { nextTimer: seconds })
        }
    }

    SlideContextMenu {
        id: slideContextMenu
        model: [
            { id: "edit", label: qsTr("Edit"), icon: "edit", color: "#97c7ff" },
            { id: "group", label: qsTr("Change group…"), icon: "groups", color: "#ff97d9" },
            { id: "nextTimer", label: qsTr("Next timer…"), icon: "clock", color: "#fca4ff" },
            { divider: true },
            { id: "duplicate", label: qsTr("Duplicate"), icon: "duplicate", color: "#97c7ff", shortcut: Hotkeys.sequence("duplicateSelected") },
            { id: "delete", label: qsTr("Delete"), icon: "delete", color: "#ff5454", shortcut: Hotkeys.sequence("deleteSelected") }
        ]
        onChosen: (id, arg) => root.slideMenuPicked(id)
    }

    DropdownPanel {
        id: groupPickerMenu
        property string slideId: ""
        visible: false
        model: {
            void GroupListModel.rosterChanged
            const rows = []
            for (let i = 0; i < GroupListModel.rowCount(); ++i)
                rows.push({ label: GroupListModel.getGroup(i).name })
            rows.push({ divider: true })
            rows.push({ label: "None" })
            return rows
        }
        onItemActivated: (label) => {
            if (groupPickerMenu.slideId !== "")
                ShowService.updateSlide(groupPickerMenu.slideId, { tag: label === "None" ? "" : label })
            groupPickerMenu.visible = false
            groupPickerMenu.slideId = ""
        }
    }

    // Font-weight picker for TextItemPanel's weight pill (see its changeFontRequested signal) - the short, fixed presets its
    // rendering already understands (see itemTextLabel's font.weight ternary above).
    DropdownPanel {
        id: textFontMenu
        visible: false
        // Matches the Weight field's own width — same reasoning as the
        // font-family menu below (see its own comment).
        matchAnchorWidth: true
        model: [
            { label: "Regular" },
            { label: "Medium" },
            { label: "SemiBold" },
            { label: "Bold" }
        ]
        onItemActivated: (label) => {
            if (root.primarySelectedItem) {
                canvasHistory.push(qsTr("Change font weight"))
                root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { fontWeight: label })
            }
            textFontMenu.visible = false
        }
    }

    DropdownPanel {
        id: textAutoSizeMenu
        visible: false
        matchAnchorWidth: true
        model: [
            { label: qsTr("None"), key: "none" },
            { label: qsTr("Shrink"), key: "shrinkToFit" },
            { label: qsTr("Grow"), key: "growToFit" }
        ]
        onItemActivated: (label, item) => {
            if (root.primarySelectedItem) {
                canvasHistory.push(qsTr("Change auto size"))
                root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { autoSize: item.key })
            }
            textAutoSizeMenu.visible = false
        }
    }

    DropdownPanel {
        id: borderStyleMenu
        visible: false
        matchAnchorWidth: true
        model: [
            { label: qsTr("Line"), key: "line" },
            { label: qsTr("Dotted"), key: "dotted" },
            { label: qsTr("Dashed"), key: "dashed" }
        ]
        onItemActivated: (label, item) => {
            root.pushUndoSnapshot()
            sizeStyleCard.targets.forEach((t) => t.borderStyle = item.key)
            borderStyleMenu.visible = false
        }
    }

    DropdownPanel {
        id: mediaFitMenu
        visible: false
        matchAnchorWidth: true
        model: [
            { label: qsTr("Contain"), key: "contain" },
            { label: qsTr("Cover"), key: "cover" },
            { label: qsTr("Fill"), key: "fill" },
            { label: qsTr("Blur fill"), key: "blur" }
        ]
        onItemActivated: (label, item) => {
            root.pushUndoSnapshot()
            root.applyMediaFit(item.key)
            mediaFitMenu.visible = false
        }
    }

    DropdownPanel {
        id: mediaBlendMenu
        visible: false
        matchAnchorWidth: true
        maxHeight: 300
        model: mediaItemPanel ? mediaItemPanel.blendOptions : []
        onItemActivated: (label, item) => {
            root.pushUndoSnapshot()
            root.setMediaMeta({ blend: item.key })
            mediaBlendMenu.visible = false
        }
    }

    DropdownPanel {
        id: textCaseMenu
        visible: false
        matchAnchorWidth: true
        model: [
            { label: qsTr("As typed"), key: "none" },
            { label: qsTr("UPPERCASE"), key: "upper" },
            { label: qsTr("lowercase"), key: "lower" },
            { label: qsTr("Title Case"), key: "capitalize" }
        ]
        onItemActivated: (label, item) => {
            if (root.primarySelectedItem) {
                canvasHistory.push(qsTr("Change text transform"))
                root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { textCase: item.key })
            }
            textCaseMenu.visible = false
        }
    }

    DropdownPanel {
        id: textFontFamilyMenu
        visible: false
        maxHeight: 320
        headerTitle: qsTr("Font family")
        headerSubtitle: root.primarySelectedItem ? (root.primarySelectedItem.meta.fontFamily ?? FontService.defaultContentFamily) : ""
        rowInset: 8
        rowHeight: 30
        matchAnchorWidth: true
        model: {
            const current = root.primarySelectedItem ? (root.primarySelectedItem.meta.fontFamily ?? FontService.defaultContentFamily) : ""
            return Qt.fontFamilies().slice().sort().map((f) => ({ label: f, trailing: f === current ? "✓" : "", fontFamily: f, accent: f === current }))
        }
        onItemActivated: (label) => {
            if (root.primarySelectedItem) {
                canvasHistory.push(qsTr("Change font"))
                root.primarySelectedItem.meta = Object.assign({}, root.primarySelectedItem.meta, { fontFamily: label })
            }
            textFontFamilyMenu.visible = false
        }
    }

    BackgroundColorModal {
        id: bgColorModal
        objectName: "selfTestBgPicker"
        open: root.bgModalOpen
        onOpenChanged: if (open) {
            const item = root.bgPickerPrimaryItem()
            const style = item ? item.style : null
            bgColorModal.seedWith(root.bgModalTarget === "itemBackground" || root.bgModalTarget === "border"
                ? (style ? (root.bgModalTarget === "border" ? style.borderColor : style.backgroundColor) : "")
                : root.bgModalTarget === "textColor"
                    ? (item ? String(item.meta?.color ?? "") : "")
                    : root.bgModalTarget === "lineBackground"
                        ? (item ? String(item.meta?.lineBg ?? "") : "")
                        : slideStore.current.background)
        }
        title: root.bgModalTarget === "border" ? qsTr("Border Color")
            : root.bgModalTarget === "textColor" ? qsTr("Text Color")
            : root.bgModalTarget === "lineBackground" ? qsTr("Line Background")
            : qsTr("Background Color")
        onApplied: (selection) => {
            canvasHistory.push(qsTr("Change color"))
            const hex = selection.kind === "color" ? selection.color : selection.from
            if (root.bgModalTarget === "border" || root.bgModalTarget === "itemBackground") {
                let touched = 0
                root.bgPickerKeys().forEach((k) => {
                    if (root.applyItemColor(root.canvasObjectByKey(k), root.bgModalTarget, hex))
                        ++touched
                })
                if (touched === 0)
                    EventBus.notify(qsTr("Colour not applied — the item it was opened for is no longer selected. Select it and try again."),
                                    "warning", qsTr("Edit"), "ui.edit.color.noTarget")
            } else if (root.bgModalTarget === "textColor") {
                // Solid only, same reasoning as border/itemBackground above.
                const item = root.bgPickerPrimaryItem() ?? root.primarySelectedItem
                if (item)
                    item.meta = Object.assign({}, item.meta, { color: hex })
            } else if (root.bgModalTarget === "lineBackground") {
                let touched = 0
                root.bgPickerKeys().forEach((k) => {
                    const item = root.canvasObjectByKey(k)
                    if (item && item.kind === "text") {
                        item.meta = Object.assign({}, item.meta, { lineBg: hex })
                        ++touched
                    }
                })
                if (touched === 0)
                    EventBus.notify(qsTr("Line colour not applied — the item it was opened for is no longer selected. Select it and try again."),
                                    "warning", qsTr("Edit"), "ui.edit.lineBg.noTarget")
            } else {
                slideStore.current.background = hex
            }
            root.bgModalTargetKeys = []
            root.bgModalOpen = false
        }
        onCancelled: {
            root.bgModalTargetKeys = []
            root.bgModalOpen = false
        }
    }

    BusSourceModal {
        id: busSourceModal
        objectName: "editBusPicker"
        mode: "bus"
        open: root.busModalOpen
        onApplied: (bus) => {
            root.busModalOpen = false
            const pickedRow = bus && bus.row !== undefined ? Number(bus.row)
                            : (bus ? root.windowBusRow(Number(bus.id)) : -1)
            if (pickedRow < 0)
                return
            // Its name and type are read from the model HERE, so the item is
            // named what the bus is called now and not what a stale list said.
            const info = BusListModel.getBus(pickedRow)
            if (!info || info.id === undefined)
                return
            const item = root.addCanvasItem("bus")
            if (!item)
                return   // the engine refused the item (no show, or no slide open)
            item.text = info.name !== undefined ? String(info.name) : ""
            // A 16:9 tile — the window is the bus's feed, so it is sized
            // like a picture rather than like a line of text.
            item.width = 240
            item.height = 135
            item.meta = {
                busId: Number(info.id),
                busName: item.text,
                busType: info.type !== undefined ? String(info.type) : ""
            }
        }
        onCancelled: root.busModalOpen = false
    }

    BusSourceModal {
        id: windowBusPicker
        objectName: "editWindowBusPicker"
        mode: "bus"
        open: root.windowBusModalOpen
        onApplied: (bus) => {
            root.windowBusModalOpen = false
            root.assignWindowBus(bus)
        }
        onCancelled: root.windowBusModalOpen = false
    }

    TimerSourceModal {
        id: timerSourceModal
        open: root.timerModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("timer")
            item.meta = Object.assign({}, config, { startedAt: Date.now() })
            root.timerModalOpen = false
        }
        onCancelled: root.timerModalOpen = false
    }

    ClockSourceModal {
        id: clockSourceModal
        open: root.clockModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("clock")
            const meta = Object.assign({}, config)
            delete meta.background            // the box is the item's own background (palette-editable), not clock config
            item.meta = meta
            item.style.backgroundColor = config.background === "none" ? "transparent" : "#12131a"
            root.clockModalOpen = false
        }
        onCancelled: root.clockModalOpen = false
    }

    ShapeSourceModal {
        id: shapeSourceModal
        open: root.shapeModalOpen
        onApplied: (config) => {
            const item = root.addCanvasItem("shape")
            item.meta = config
            item.style.backgroundColor = "#3a3f55"   // visible from the start; the palette's Transparent makes it an outline
            root.shapeModalOpen = false
        }
        onCancelled: root.shapeModalOpen = false
    }

    // ---- The Media chip's picker -----------------------------------------
    MediaSourceModal {
        id: mediaSourceModal
        open: root.mediaModalOpen
        fixedKind: root.mediaPickKind
        onApplied: (item) => {
            // Two outcomes from one picker: the MEDIA tab's Change replaces the
            // file on the item it was opened for, the "+" ring adds a new item.
            if (root.mediaReplaceKey !== "") {
                if (!root.replaceItemMedia(root.mediaReplaceKey, item))
                    EventBus.notify(qsTr("That item is no longer on the slide — nothing was replaced."),
                                    "warning", qsTr("Media"), "edit.mediaReplace")
                root.mediaReplaceKey = ""
            } else if (!root.addMediaItem(root.mediaPickKind, item)) {
                EventBus.notify(qsTr("Add a slide first — an item needs a slide to sit on."),
                                "warning", qsTr("Media"), "edit.mediaItem")
            }
            root.mediaModalOpen = false
        }
        onCancelled: {
            root.mediaModalOpen = false
            root.mediaReplaceKey = ""
        }
        onBrowseRequested: MediaLibraryService.addFolder()
    }

    // ---- Esc closes the Edit screen's own transient panels -----------------
    property int panelHotkeyScope: 0
    readonly property bool anyPanelOpen: root.addMenuOpen || root.metadataPaletteOpen
    onAnyPanelOpenChanged: {
        if (root.anyPanelOpen && root.panelHotkeyScope === 0) {
            root.panelHotkeyScope = Hotkeys.open({
                name: qsTr("Add content"),
                cancel: function () {
                    root.addMenuOpen = false
                    root.metadataPaletteOpen = false
                }
            })
        } else if (!root.anyPanelOpen && root.panelHotkeyScope !== 0) {
            Hotkeys.close(root.panelHotkeyScope)
            root.panelHotkeyScope = 0
        }
    }

    // ---- Keyboard shortcuts ---------------------------------------------
    Shortcut {
        action: "undo"
        enabled: mCanvas.activeFocus || root.anyTextEditing || slideListFlick.activeFocus
        onActivated: root.undo()
    }
    Shortcut {
        action: "redo"
        enabled: mCanvas.activeFocus || root.anyTextEditing || slideListFlick.activeFocus
        onActivated: root.redo()
    }
    Shortcut {
        action: "redoAlt"
        enabled: mCanvas.activeFocus || root.anyTextEditing || slideListFlick.activeFocus
        onActivated: root.redo()
    }
    Shortcut {
        action: "copy"
        enabled: mCanvas.activeFocus && root.selectedCanvasObjects.length > 0
        onActivated: root.copySelectedCanvasObjects()
    }
    Shortcut {
        action: "paste"
        enabled: mCanvas.activeFocus && root.clipboardItems.length > 0
        onActivated: root.pasteClipboardItems()
    }
    Shortcut {
        action: "duplicateSelected"
        enabled: mCanvas.activeFocus && root.selectedCanvasObjects.length > 0
        onActivated: root.duplicateSelectedCanvasObjects()
    }
    // Ctrl+A — the canvas menu's "Select all" row, on the keyboard.
    Shortcut {
        action: "selectAll"
        enabled: mCanvas.activeFocus && root.hasActiveSlide
        onActivated: root.selectAllCanvasObjects()
    }
    Shortcut {
        action: "addText"
        enabled: mCanvas.activeFocus && root.hasActiveSlide
        onActivated: root.addTextItem()
    }

    // ---- Text style from the keyboard (Ctrl+B / Ctrl+I / Ctrl+U) --------
    readonly property bool textStyleKeysLive:
        (mCanvas.activeFocus || root.anyTextEditing) && root.primarySelectedItem !== null
    Shortcut {
        action: "boldText"
        enabled: root.textStyleKeysLive
        onActivated: root.toggleTextStyle("bold")
    }
    Shortcut {
        action: "italicText"
        enabled: root.textStyleKeysLive
        onActivated: root.toggleTextStyle("italic")
    }
    Shortcut {
        action: "underlineText"
        enabled: root.textStyleKeysLive
        onActivated: root.toggleTextStyle("underline")
    }

    // ---- The slide LIST's own two keys --------------------------------
    Shortcut {
        action: "duplicateSelected"
        enabled: slideListFlick.activeFocus && root.hasActiveSlide
        onActivated: {
            const row = slideModel.activeIndex()
            if (row >= 0)
                showSession.duplicateSlide(row)
        }
    }
    Shortcut {
        action: "deleteSelected"
        enabled: slideListFlick.activeFocus && root.hasActiveSlide
        onActivated: {
            const row = slideModel.activeIndex()
            if (row >= 0)
                showSession.removeSlide(row)
        }
    }
}