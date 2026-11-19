import QtQuick
import VGRPresenterUI
import "../shortcuts"

Item {
    id: root

    property var item: null
    property bool slideHotkeysEnabled: false
    signal editRequested(string path)
    signal templateEditRequested(string id)
    signal closeRequested()

    readonly property string type: item ? (item.type ?? "") : ""
    property int reload: 0            // bumped when the show's file changed under us (the next timer), so it is read again
    readonly property var loaded: {
        root.reload
        ShowService.peekRevision
        if (root.type !== "show")
            return null
        const peekT0 = Date.now()
        const peekAnswer = ShowService.peekShow(item.ref)
        const peekMs = Date.now() - peekT0
        if (peekMs >= 40)
            console.log("[PEEKCOST] peekShow " + peekMs + " ms for " + item.ref)
        return peekAnswer
    }
    readonly property string imageFit: {
        const fileFit = root.type === "image" && root.item
            ? MediaLibraryService.itemFit(String(root.item.ref)) : ""
        return fileFit !== "" ? fileFit : LiveOutputService.defaultMediaFit
    }
    readonly property var show: loaded && loaded.ok ? loaded.show : null
    readonly property var slides: show ? show.slides : []
    property int styleRevision: 0
    property int templateCatalogRevision: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() {
            root.styleRevision++
            templateNoticeTimer.restart()
        }
    }
    Connections {
        target: TemplateLibraryService
        function onChanged() {
            root.templateCatalogRevision++
            templateNoticeTimer.restart()
        }
    }
    // The wall's timer reads the show ON SCREEN, not only the on-air set: its icon lights and its bar
    // appears for a timed show before GO LIVE too. Report this page's own timer total whenever it changes.
    function reportShowTimer() { LiveOutputService.setShowTimerTotal(root.timerTotal) }
    Connections {
        target: root
        function onTimerTotalChanged() { root.reportShowTimer() }
    }
    readonly property string outputStyleTemplateId: {
        root.styleRevision
        return OutputListModel.activeStyleTemplateId("shows")
    }
    function categoryTemplatePick(category, picks) {
        const exact = String(picks[category] ?? "")
        if (exact !== "") return exact
        const wanted = String(category).trim().toLowerCase()
        for (const key in picks)
            if (String(key).trim().toLowerCase() === wanted)
                return String(picks[key] ?? "")
        return ""
    }
    readonly property string categoryTemplateOverrideId: {
        const category = String(root.item?.category ?? "")
        const raw = SettingsService.values["session.showsCategoryTemplates"]
        let picks = ({})
        try { picks = raw ? JSON.parse(String(raw)) : ({}) } catch (e) {}
        const categoryTemplate = category !== "" ? root.categoryTemplatePick(category, picks) : ""
        if (categoryTemplate !== "") return categoryTemplate
        return String(SettingsService.values["shows.defaultTemplate"] ?? "")
    }
    readonly property string outputCategoryTemplateId: {
        root.styleRevision
        return root.item && root.type === "show"
            ? OutputListModel.activeStyleShowCategoryTemplateId(String(root.item.category ?? "")) : ""
    }
    readonly property string showTemplateId: root.outputCategoryTemplateId !== ""
        ? root.outputCategoryTemplateId
        : (root.categoryTemplateOverrideId !== "" ? root.categoryTemplateOverrideId : root.outputStyleTemplateId)
    property int selectedSlide: -1


    property bool stageFirstPending: false
    // The (show, template) pairs already warned about THIS SESSION. Once is enough: closing the notice
    // is the answer, and opening the show again (or coming back to it) must not ask again. (It used to
    // remember only the last one, so going A -> B -> A warned about A twice.)
    property var templatePrompted: ({})
    onItemChanged: { selectedSlide = -1; root.refreshWallStepper() }
    onShowChanged: {
        // A (new or reloaded) show fills in batches; an edit only clamps — see builtSlides.
        root.renderSlideCache = ({})
        if (stageFirstPending && show) {
            stageFirstPending = false
            Qt.callLater(() => root.stageFirstSlide())
        }
        templateNoticeTimer.restart()
    }

    function resolvedShow(forTemplate) {
        if (!root.show)
            return null
        if (!forTemplate || forTemplate.id === "")
            return root.show

        // The library design is an external category template. Adapt it to the
        // show-document resolver without saving or changing the show on disk.
        const copy = JSON.parse(JSON.stringify(root.show))
        const templateId = String(forTemplate.id)
        const templates = (copy.templates ?? []).filter((t) => String(t.id) !== templateId)
        templates.push({
            id: templateId,
            name: String(forTemplate.name ?? ""),
            contentType: String(forTemplate.contentType ?? "shows"),
            background: String(forTemplate.background ?? "transparent"),
            blocks: forTemplate.blocks ?? [],
            meta: ({})
        })
        copy.templates = templates

        let categories = copy.categories ?? []
        if (categories.length === 0) {
            categories = [{ id: "__show_center_template", name: "Show", contentType: "shows", templateId: templateId }]
            copy.slides = (copy.slides ?? []).map((slide) => Object.assign({}, slide, {
                categoryId: String(slide.categoryId ?? "") !== "" ? slide.categoryId : "__show_center_template" }))
        } else {
            categories = categories.map((category) => Object.assign({}, category, { templateId: templateId }))
            const fallbackCategoryId = String(categories[0].id ?? "")
            copy.slides = (copy.slides ?? []).map((slide) => Object.assign({}, slide, {
                categoryId: String(slide.categoryId ?? "") !== "" ? slide.categoryId : fallbackCategoryId }))
        }
        copy.categories = categories
        return copy
    }
    readonly property var categoryTemplateDesign: {
        root.templateCatalogRevision
        const id = root.showTemplateId
        if (id === "") return null
        try {
            const design = TemplateLibraryService.design(id)
            return design && design.id ? design : null
        } catch (e) { return null }
    }
    readonly property var renderShow: root.resolvedShow(root.categoryTemplateDesign)
    function blockKind(block) {
        return String(block?.kind ?? "text").toLowerCase()
    }
    function sourceHasContent(block) {
        const kind = root.blockKind(block)
        if (kind === "text") return String(block.text ?? "").trim() !== ""
        if (["image", "video", "audio", "media"].indexOf(kind) >= 0) {
            const meta = block.meta ?? ({})
            const media = meta.media ?? ({})
            return String(meta.src ?? meta.path ?? meta.url ?? media.src ?? "").trim() !== ""
        }
        return kind !== ""
    }
    function templateComposition(slide, resolved, design) {
        const templateDesign = design ?? root.categoryTemplateDesign
        const sources = (slide.blocks ?? []).slice()
        const hasTextItem = sources.some((block) => root.blockKind(block) === "text"
            && root.sourceHasContent(block))
        if (!hasTextItem) {
            for (const slot of (templateDesign?.blocks ?? [])) {
                const bind = String(slot.bind ?? "")
                if (root.blockKind(slot) !== "text" || bind === "" || bind === "title") continue
                const bound = (resolved?.blocks ?? []).find((block) =>
                    String(block.key ?? block.id ?? "") === String(slot.key ?? slot.id ?? ""))
                if (bound && root.sourceHasContent(bound))
                    sources.push(Object.assign({}, bound, { _templateSynthetic: true }))
            }
            const fallbackText = [slide.body, slide.text, slide.line1, slide.line2]
                .map((value) => String(value ?? "")).find((value) => value.trim() !== "") ?? ""
            if (fallbackText.trim() !== ""
                    && !sources.some((block) => root.blockKind(block) === "text" && root.sourceHasContent(block))) {
                sources.push({ key: "@show-body", kind: "text", text: fallbackText, _templateSynthetic: true })
            }
        }
        const slots = templateDesign?.blocks ?? []
        const used = ({})
        const blocks = []
        for (const slot of slots) {
            const slotKey = String(slot.key ?? slot.id ?? "")
            const kind = root.blockKind(slot)
            let sourceIndex = sources.findIndex((candidate, index) => !used[index]
                && root.sourceHasContent(candidate)
                && root.blockKind(candidate) === kind
                && String(candidate.key ?? candidate.id ?? "") === slotKey)
            if (sourceIndex < 0)
                sourceIndex = sources.findIndex((candidate, index) => !used[index]
                    && root.sourceHasContent(candidate)
                    && root.blockKind(candidate) === kind)
            if (sourceIndex < 0) {
                if (kind !== "text" && root.sourceHasContent(slot)) blocks.push(slot)
                continue
            }

            used[sourceIndex] = true
            const source = sources[sourceIndex]
            const composed = Object.assign({}, slot)
            if (kind === "text") {
                composed.text = String(source.text ?? "")
            } else {
                // Keep the template's fit/opacity and geometry while taking
                // the actual media source from the matching show item.
                const templateMeta = slot.meta ?? ({})
                const sourceMeta = source.meta ?? ({})
                const meta = Object.assign({}, templateMeta, sourceMeta)
                if (templateMeta.media || sourceMeta.media)
                    meta.media = Object.assign({}, sourceMeta.media ?? ({}), templateMeta.media ?? ({}))
                if (sourceMeta.src !== undefined) meta.src = sourceMeta.src
                composed.meta = meta
            }
            composed.bind = ""
            blocks.push(composed)
        }
        // OMITTED = what the notice says it is: TEXT or MEDIA the template had no box for. A clock, a
        // shape or any other non-content item is never "omitted text or media", and counting them made
        // the notice come back for a show whose text had long been cleared.
        const countsAsOmitted = (block) => {
            const kind = root.blockKind(block)
            return kind === "text" || ["image", "video", "audio", "media"].indexOf(kind) >= 0
        }
        return {
            blocks: blocks,
            omitted: sources.filter((block, index) => !used[index]
                && block._templateSynthetic !== true && countsAsOmitted(block)
                && root.sourceHasContent(block)).length
        }
    }
    function unmatchedItemCount(slide) {
        if (!slide) return 0
        const resolved = ShowService.resolveSlide(root.renderShow, String(slide.id ?? ""), "")
        if (!resolved || resolved.visible === false) return 0
        return root.templateComposition(slide, resolved).omitted
    }
    readonly property int unmatchedTemplateItemCount: {
        if (!root.renderShow || !root.categoryTemplateDesign) return 0
        return root.slides.reduce((sum, slide) => sum + root.unmatchedItemCount(slide), 0)
    }
    readonly property bool hasUnmatchedTemplateItems: root.unmatchedTemplateItemCount > 0
    function offerTemplateForMissingText() {
        if (!root.visible || root.type !== "show" || !root.show || !root.hasUnmatchedTemplateItems)
            return
        if (root.showTemplateId !== "" && TemplateLibraryService.totalCount === 0)
            return
        const path = String(root.item?.ref ?? "")
        const promptKey = path + "|" + root.showTemplateId
        if (path === "" || root.templatePrompted[promptKey] === true) return
        root.templatePrompted[promptKey] = true
        const actionId = "show.createTemplateFromMissingItems." + promptKey
        NotificationCenter.registerAction(actionId, function() {
            if (String(root.item?.ref ?? "") === path)
                root.createTemplateFromShow(path)
        })
        const templateName = root.categoryTemplateDesign
            ? String(root.categoryTemplateDesign.name ?? qsTr("current Shows template"))
            : qsTr("No Shows template")
        EventBus.notify(
            qsTr("Some text or media items were omitted because “%1” has no matching boxes for them. Add boxes to this template or create a category template for this show.").arg(templateName),
            "warning", qsTr("Show items omitted"), "show.template.missingItems", templateName,
            ({ id: actionId, label: qsTr("Create template") }))
    }
    function createTemplateFromShow(expectedPath) {
        if (!root.show || !root.item || String(root.item.ref ?? "") !== expectedPath) return
        const consumedSourceKeys = ({})
        const outputKeys = ({})
        const templateBlocks = []
        const sources = []
        for (const slide of root.slides)
            for (const block of (slide.blocks ?? [])) sources.push(block)
        const usedSources = ({})
        for (const slot of (root.categoryTemplateDesign?.blocks ?? [])) {
            const slotKey = String(slot.key ?? slot.id ?? "")
            const kind = root.blockKind(slot)
            let sourceIndex = sources.findIndex((source, index) => !usedSources[index]
                && root.sourceHasContent(source)
                && root.blockKind(source) === kind
                && String(source.key ?? source.id ?? "") === slotKey)
            if (sourceIndex < 0)
                sourceIndex = sources.findIndex((source, index) => !usedSources[index]
                    && root.sourceHasContent(source)
                    && root.blockKind(source) === kind)
            if (sourceIndex >= 0) {
                usedSources[sourceIndex] = true
                const sourceKey = String(sources[sourceIndex].key ?? sources[sourceIndex].id ?? "")
                if (sourceKey !== "") consumedSourceKeys[sourceKey] = true
            }
            if (slotKey === "" || outputKeys[slotKey]) continue
            outputKeys[slotKey] = true
            templateBlocks.push(Object.assign({}, slot, { text: "", bind: slot.bind ?? "" }))
        }
        for (const source of sources) {
            const key = String(source.key ?? source.id ?? "")
            if (key === "" || consumedSourceKeys[key] || outputKeys[key]) continue
            outputKeys[key] = true
            templateBlocks.push(Object.assign({}, source, { text: "", bind: "" }))
        }
        if (templateBlocks.length === 0) {
            EventBus.notify(qsTr("There are no slide items to use for a template."), "warning", qsTr("Template"))
            return
        }
        const name = qsTr("%1 Template").arg(String(root.item.name ?? qsTr("Show")))
        const id = TemplateLibraryService.createDesign(name)
        if (id === "") return
        const firstSlide = root.slides.length > 0 ? root.slides[0] : ({})
        let background = String(firstSlide.background ?? "")
        if (background === "") background = String(root.categoryTemplateDesign?.background ?? "transparent")
        if (!TemplateLibraryService.setDesignBlocks(id, background, templateBlocks)) {
            TemplateLibraryService.deleteDesign(id)
            EventBus.notify(qsTr("The template could not be created from this show."), "error", qsTr("Template"))
            return
        }
        TemplateLibraryService.setDesignContentType(id, "shows")
        root.templateEditRequested(id)
    }
    onHasUnmatchedTemplateItemsChanged: templateNoticeTimer.restart()
    onShowTemplateIdChanged: templateNoticeTimer.restart()
    Timer {
        id: templateNoticeTimer
        interval: 350
        repeat: false
        onTriggered: root.offerTemplateForMissingText()
    }
    // The composition for one slide is ~19 ms, so a grid of 38 resolved the same slides over and over
    // (open, reload, edit, view switch). The cache is keyed on everything this answer depends on.
    property var renderSlideCache: ({})
    function renderSlide(slide, forStage) {
        if (!slide || !root.show)
            return slide
        if (forStage)
            return Object.assign({}, slide, { showCategory: String(root.item?.category ?? "") })
        const designId = root.showTemplateId
        const design = designId === "" ? null : TemplateLibraryService.design(designId)
        if (!design || !design.id)
            return Object.assign({}, slide, { showCategory: String(root.item?.category ?? "") })
        const cacheKey = String(slide.id ?? "") + "|" + designId + "|" + String(root.item?.ref ?? "")
                       + "|" + root.styleRevision + "|" + root.templateCatalogRevision
        if (root.renderSlideCache[cacheKey] !== undefined)
            return root.renderSlideCache[cacheKey]
        const resolvedShow = root.resolvedShow(design)
        const resolved = ShowService.resolveSlide(resolvedShow, String(slide.id ?? ""), "")
        if (!resolved || resolved.visible === false)
            return slide
        let blocks = resolved.blocks ?? slide.blocks ?? []
        blocks = root.templateComposition(slide, resolved, design).blocks
        const composed = Object.assign({}, slide, {
            blocks: blocks,
            background: resolved.background ?? slide.background ?? "",
            showCategory: String(root.item?.category ?? ""),
            templateOverride: !!design
        })
        root.renderSlideCache[cacheKey] = composed
        return composed
    }
    function renderedSlides() {
        return root.slides.map((slide) => root.renderSlide(slide, true))
    }

    // Reuse the configurable Left/Right slide hotkeys for the centre preview's
    // slide selector. Do not steal those keys while the user is typing in a field.
    readonly property bool typingInField: {
        const w = Window.window
        const f = w ? w.activeFocusItem : null
        return !!f && f.text !== undefined && f.cursorPosition !== undefined
    }
    Shortcut {
        action: "previousSlide"
        enabled: root.visible && root.slideHotkeysEnabled && root.type === "show"
                 && root.slides.length > 1 && !root.typingInField
        onActivated: root.stepSlide(-1)
    }
    Shortcut {
        action: "nextSlide"
        enabled: root.visible && root.slideHotkeysEnabled && root.type === "show"
                 && root.slides.length > 1 && !root.typingInField
        onActivated: root.stepSlide(1)
    }

    // The view (FreeShow's slidesOptions), kept by the engine's settings: how many slides across, and grid / list / lyrics.
    readonly property int columns: Math.max(2, Math.min(10, Number(SettingsService.values["session.slideColumns"] ?? 4)))
    readonly property string viewMode: String(SettingsService.values["session.slideView"] ?? "grid")
    readonly property real cellWidth: Math.floor((width - 10) / columns)     // (FreeShow: the grid has 5px padding, a cell is 100 / columns %)
    readonly property bool hasRichContent: root.slides.some((slide) => {
        const blocks = slide.blocks ?? []
        const background = String(slide.background ?? "").toLowerCase()
        return blocks.length > 1
            || blocks.some((block) => (block.kind ?? "text") !== "text"
                || Number(block.x ?? 0) !== 0 || Number(block.y ?? 0) !== 0
                || Number(block.width ?? 160) !== 160 || Number(block.height ?? 40) !== 40
                || String(block.style?.backgroundColor ?? "transparent").toLowerCase() !== "transparent")
            || (background !== "" && background !== "transparent" && background !== "#00000000")
    })
    // FreeShow's "dark" theme
    readonly property color cLighter: Theme.border
    readonly property color cDarker: Theme.rowBg
    readonly property color cDarkest: Theme.panelBg
    readonly property color cText: Theme.textPrimary
    // Selection and focus accents follow the user's Appearance setting.
    readonly property color cSecondary: Theme.accent
    readonly property string mono: FontService.monoFamily
    property bool headerMenuOpen: false
    // the colour of a slide's group (verse, chorus...): the bar under its picture
    function groupColor(slide) {
        const map = { break: "#f5255e", bridge: "#f52598", chorus: "#f525d2", intro: "#d525f5", outro: "#a525f5", pre_chorus: "#8825f5", tag: "#7525f5", verse: "#5825f5" }
        const norm = (t) => String(t ?? "").toLowerCase().replace(/[\s-]+/g, "_").replace(/_?\d+$/, "")
        return map[norm(slide.tag)] ?? map[norm(slide.title)] ?? ""
    }
    property bool barOpen: false      // the floating bar's chevron: shows the zoom
    property bool zoomOpen: false     // the zoom's + / - popup

    function escapeBack() {
        if (root.headerMenuOpen) {
            root.headerMenuOpen = false
            return true
        }
        if (root.item !== null) {
            // The one line that says the KEY did this rather than a click —
            // "the page closed" alone cannot tell the two apart afterwards.
            EngineBridge.log("info", "Navigation",
                             "Esc closed the centre page (" + root.type + " “" + (root.item.name ?? "") + "”)")
            root.closeRequested()
            return true
        }
        return false
    }

    function setColumns(n) {
        const next = Math.max(2, Math.min(10, n))
        if (next !== root.columns)
            SettingsService.setValue("session.slideColumns", next)
    }
    // grid -> list -> lyrics -> grid, like FreeShow's view button
    function nextView() {
        if (root.hasRichContent) {
            SettingsService.setValue("session.slideView", root.viewMode === "grid" ? "list" : "grid")
            return
        }
        const order = { grid: "list", list: "lyrics", lyrics: "grid" }
        SettingsService.setValue("session.slideView", order[root.viewMode] ?? "grid")
    }
    // ---- the next timer (FreeShow's clock): every slide moves on by itself after so many seconds ----
    property bool timerOpen: false
    property int timerSeconds: 10
    readonly property real timerTotal: root.slides.reduce((t, s) => t + (s.nextTimer ?? 0), 0)
    // every slide already has exactly the seconds in the box: the button then offers to take it off again
    readonly property bool timerApplied: root.slides.length > 0 && root.timerSeconds > 0 && root.slides.every((s) => Math.round(s.nextTimer ?? 0) === root.timerSeconds)
    function formatTime(t) {
        const n = Math.round(t)
        if (n <= 59) return n + "s"
        const m = Math.floor(n / 60), r = n % 60
        return r === 0 ? m + "m" : m + "m " + r + "s"
    }
    // The token of the next-timer write in flight (0 = none): it rewrites the
    // show's file on the engine worker, so the reload below hangs off its verdict.
    property int _timerToken: 0
    function applyTimer() {
        if (!root.item)
            return
        const seconds = root.timerApplied ? 0 : root.timerSeconds
        root._timerToken = ShowService.setNextTimer(root.item.ref, seconds)
    }
    Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok) {
            if (kind !== "timer" || token !== root._timerToken)
                return
            root._timerToken = 0
            if (!ok)
                return
            root.reload++          // the file changed: the grid reads the show again
            root.timerOpen = false
        }
    }
    // A slide's words, for the list and lyrics views.
    function slideText(slide) {
        return (slide.blocks ?? []).filter((b) => (b.kind ?? "text") === "text" && (b.text ?? "") !== "").map((b) => b.text).join("\n")
    }
    function slideTextItems(slide) {
        const blocks = slide.blocks ?? []
        const items = blocks.filter((block) => (block.kind ?? "text") === "text")
            .map((block) => ({ blockId: String(block.key ?? block.id ?? ""), text: String(block.text ?? "") }))
        if (items.length > 0)
            return items
        return slide.text ? [{ blockId: "@slide_text", text: String(slide.text) }] : []
    }

    // A SINGLE CLICK ON A SLIDE TAKES IT (no double-click per slide: faster under pressure). Live, that moves
    // the OUTPUT, so it only fires while this show is what the output shows; not live, it only STAGES the
    // slide for the monitor preview, so any click may do it.
    readonly property bool singleClickLive: SettingsService.values["preferences.singleClickLive"] !== false
        && root.item !== null && root.item !== undefined
        // NOT LIVE: the click STAGES the slide (takeSlideLive -> stageSlideSet, the monitor preview),
        // which takes nothing away from anyone. Only content that is already ON AIR needs the "this
        // show is the one showing" gate — that is what ownsWallTile() answers.
        && (!LiveOutputService.live || root.ownsWallTile())
    function slideTapped(index, slide, styledPreview) {
        if (root.selectedSlide === index || root.singleClickLive)
            root.takeSlideLive(index, slide, styledPreview)
        else
            root.selectedSlide = index
    }
    property int lastTakenSlide: -1
    property double lastTakenAt: 0
    function takeSlideLive(index, slide, styledPreview) {
        // a double-click is two taps: the second one must not pay for the same slide twice
        const now = Date.now()
        if (index === root.lastTakenSlide && now - root.lastTakenAt < 450)
            return
        root.lastTakenSlide = index
        root.lastTakenAt = now
        root.selectedSlide = index
        const pickT0 = Date.now()
        const wordsPerScreen = Number(SettingsService.value("shows.wordsPerSlide"))
        LiveOutputService.readingWordBudget = wordsPerScreen > 0 ? wordsPerScreen : 40
        const name = root.item ? root.item.name : ""
        const picked = slide ? root.renderSlide(slide, true)
                             : ((index >= 0 && index < root.slides.length) ? root.slides[index] : null)
        if (picked)
            LiveOutputService.stageSlideSet(name, [picked], 0)
        showSetFill.restart()
        const pickMs = Date.now() - pickT0
        if (pickMs >= 80)
            console.log("[PICKCOST] show slide " + index + " staged in " + pickMs + " ms"
                        + (LiveOutputService.scrollMode ? " (scroll)" : " (slide)"))
    }
    function fillShowSet() {
        if (!root.item || !LiveOutputService.scrollMode)
            return
        const at = root.selectedSlide
        if (at < 0 || at >= root.slides.length)
            return
        LiveOutputService.stageSlideSet(root.item.name, root.renderedSlides(), at)
    }
    function stageFirstSlide() {
        if (!root.show) {
            root.stageFirstPending = true
            return
        }
        if (root.slides.length > 0)
            root.takeSlideLive(0, root.slides[0], null)
    }
    Timer {
        id: showSetFill
        interval: 0
        repeat: false
        onTriggered: root.fillShowSet()
    }

    // ---- The wall's ‹ › while a SHOW slide is on the tile -------------------
    function stepSlide(direction) {
        const slides = root.slides
        if (!root.item || slides.length === 0)
            return false
        const at = (root.selectedSlide >= 0 && root.selectedSlide < slides.length)
                 ? root.selectedSlide : 0
        const next = at + direction
        if (next < 0 || next >= slides.length)
            return false               // the show's first/last slide: nothing that way
        takeSlideLive(next, slides[next], null)
        return true
    }

    function ownsWallTile() {
        if (!root.item)
            return false
        // Live, the tile belongs to the SET the show was taken as, whose name is readingSetName —
        // onAirTitle is the SLIDE's title ('Verse 1'), never the show's name, so the old comparison
        // was always false and the wall's › / the auto-advance timer could not step the show.
        return LiveOutputService.stagedIsOurs(root.item.name)
            || (LiveOutputService.live && LiveOutputService.readingSetName === root.item.name)
    }

    function refreshWallStepper() {
        LiveOutputService.setWallStepper("show", root.ownsWallTile() && root.slides.length > 1)
    }
    onSlidesChanged: {
        root.refreshWallStepper()
    }
    onSelectedSlideChanged: root.refreshWallStepper()
    Component.onCompleted: { root.refreshWallStepper(); root.reportShowTimer() }
    Connections {
        target: LiveOutputService
        function onLiveChanged() { root.refreshWallStepper() }
        function onStagedChanged() { root.refreshWallStepper() }
        function onOnAirChanged() { root.refreshWallStepper() }
        // The wall's ‹ › reached either end of the on-air set: if this show's
        // slide is the one on the tile, step the SHOW (stage the neighbour).
        function onPassageStepRequested(direction) {
            if (root.ownsWallTile())
                root.stepSlide(direction)
        }
    }

    // The show on this page can be the one open in the editor: read it again when it changes, and when this page comes back into view.
    Connections {
        target: ShowService
        function onShowChanged() { if (root.visible && root.type === "show") root.reload++ }
    }
    onVisibleChanged: {
        if (visible) root.reportShowTimer()
        if (visible && root.type === "show") {
            root.reload++
            templateNoticeTimer.restart()
        }
    }

    Rectangle { anchors.fill: parent; color: root.cDarkest }

    // ---- the header (FreeShow's ShowHeader): 30px over the slides, the name on the left, the dots on the right --------------------
    Item {
        id: bar
        visible: root.item !== null
        z: 10
        x: 0; y: 0; width: parent.width; height: 30
        clip: true
        Rectangle { y: -10; width: parent.width; height: 40; radius: 10; color: "#ef0b0b14" }
        Text {
            x: 13; anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 60
            text: root.item ? root.item.name : ""
            color: root.cText; elide: Text.ElideRight
            font.family: root.mono; font.pixelSize: 15; font.weight: Font.DemiBold
        }
        Item {
            x: parent.width - width; width: 32; height: parent.height
            Rectangle { anchors.fill: parent; color: headerMoreHover.hovered ? Theme.hoverBg : "transparent" }
            IconGlyph { anchors.centerIn: parent; name: "moreVertical"; color: root.cText; opacity: root.headerMenuOpen ? 1 : 0.8; width: 4; height: 14 }
            HoverHandler { id: headerMoreHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.headerMenuOpen = !root.headerMenuOpen }
        }
    }
    // the dots' menu (FreeShow's .showDropdown)
    MouseArea { anchors.fill: parent; z: 19; enabled: root.headerMenuOpen; onPressed: (mouse) => { root.headerMenuOpen = false; mouse.accepted = true } }
    Rectangle {
        visible: root.headerMenuOpen
        z: 20
        x: root.width - width - 5; y: 31
        width: 200; height: headerMenuColumn.height + 2
        radius: 6; color: root.cDarkest; border.color: root.cLighter
        Column {
            id: headerMenuColumn
            x: 1; y: 1; width: parent.width - 2
            Repeater {
                model: root.type === "show" ? [qsTr("Edit show"), "-", qsTr("Close")] : [qsTr("Close")]
                delegate: Item {
                    id: menuEntry
                    required property var modelData
                    width: headerMenuColumn.width
                    height: modelData === "-" ? 1 : 34
                    Rectangle { visible: menuEntry.modelData === "-"; width: parent.width; height: 1; color: root.cLighter }
                    Rectangle { visible: menuEntry.modelData !== "-"; anchors.fill: parent; color: menuEntryHover.hovered ? Theme.hoverBg : "transparent" }
                    Text { visible: menuEntry.modelData !== "-"; x: 12; anchors.verticalCenter: parent.verticalCenter; text: menuEntry.modelData; color: Theme.controlInk; font.family: root.mono; font.pixelSize: 15; font.weight: Font.Medium }
                    HoverHandler { id: menuEntryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: menuEntry.modelData !== "-"
                        onTapped: {
                            root.headerMenuOpen = false
                            if (menuEntry.modelData === qsTr("Edit show")) root.editRequested(root.item.ref)
                            else root.closeRequested()
                        }
                    }
                }
            }
        }
    }

    // ---- A CELL'S PICTURE IS BUILT WHEN IT IS NEAR THE VIEW ------------------------------------
    // Each cell used to instantiate a whole DesignPreview (every block's items) for all 38 slides at
    // once: measured 2-3.5 s of interface-thread silence opening the show, while composing the slides
    // at the active style costs only 22 ms for all 38. The cells are cheap frames; the picture is
    // loaded for the rows on screen (plus one screen of margin, so scrolling stays ahead).
    function previewOnScreen(cell) {
        if (!grid.visible)
            return false
        // INDEX WINDOW, not the cell's own y: before the first layout every cell still reports y=0, so a
        // geometry test said "all 38 are on screen" and built them all anyway. Cell height follows the
        // column width, so the rows around the scroll offset are known without waiting for layout.
        const perRow = Math.max(1, root.columns)
        const rowH = StageGrid.heightForWidth(root.cellWidth - 4) + 29
        const viewport = grid.height > 0 ? grid.height : 700
        const firstRow = Math.max(0, Math.floor(grid.contentY / rowH))
        const lastRow = Math.ceil((grid.contentY + viewport) / rowH)
        return cell.index >= firstRow * perRow && cell.index < (lastRow + 1) * perRow
    }
    // ONE PICTURE PER TURN OF THE EVENT LOOP. A preview costs ~80 ms to instantiate, so even the cells
    // on screen were a single freeze; metered this way the frames and titles appear at once and the
    // pictures fill in while the window stays responsive.

    Component {
        id: slidePreviewComponent
        DesignPreview {
            width: parent.width
            showBindPlaceholders: false
            blocks: parent.styled ? (parent.styled.blocks ?? []) : []
            background: parent.styled ? (parent.styled.background ?? "transparent") : "transparent"
            showCheckerboard: parent.styled ? parent.styled.frameTransparent === true : false
        }
    }

    // ---- a show: its slides (grid, list or lyrics) ----
    Flickable {
        id: grid
        visible: root.type === "show" && root.show !== null
        x: 0; y: 0
        width: parent.width; height: parent.height
        topMargin: bar.height
        clip: true
        contentHeight: (root.viewMode === "grid" ? gridFlow.height : (root.viewMode === "list" ? listCol.height : lyricsCol.height)) + 76   // room for the bar
        boundsBehavior: Flickable.StopAtBounds

        AppScrollBar {
            flickable: grid
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 2; anchors.bottomMargin: 56   // clear floatingBar
            anchors.right: parent.right; anchors.rightMargin: 4
        }

        // Ctrl + wheel zooms, like FreeShow.
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => root.setColumns(root.columns + (event.angleDelta.y < 0 ? 1 : -1))
        }

        Flow {
            id: gridFlow
            visible: root.viewMode === "grid"
            x: 5; y: 5
            width: parent.width - 10
            Repeater {
                model: root.viewMode === "grid" ? root.slides : []
                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.selectedSlide === index
                    readonly property color group: root.groupColor(modelData) !== "" ? root.groupColor(modelData) : root.cDarkest
                    readonly property var styled: {
                        void root.reload
                        void root.styleRevision
                        void root.templateCatalogRevision
                        void root.showTemplateId
                        // A cell far from the viewport resolves no template and composes nothing: this
                        // binding is the bulk of opening a 38-slide show. The window is the stable index
                        // range from previewOnScreen, so it only re-evaluates when the grid scrolls.
                        if (!root.previewOnScreen(cell))
                            return ({})
                        return LiveOutputService.previewWithActiveStyle(root.renderSlide(cell.modelData))
                    }
                    width: root.cellWidth
                    height: body.height + 4              // (FreeShow: a cell has 2px of padding)

                    Rectangle {
                        id: body
                        x: 2; y: 2; width: parent.width - 4
                        height: thumb.height + labelBar.height
                        color: root.cDarkest

                        // the picture: 16:9
                        Rectangle {
                            id: thumb
                            width: parent.width; height: StageGrid.heightForWidth(width)
                            color: "#000000"; clip: true
                            Loader {
                                anchors.fill: parent
                                active: root.previewOnScreen(cell)
                                sourceComponent: slidePreviewComponent
                                property var styled: cell.styled
                            }
                            Rectangle { anchors.fill: parent; color: Theme.hoverBg; visible: cellHover.hovered }      // the hover veil
                            // the slide's next timer
                            Rectangle {
                                visible: (cell.modelData.nextTimer ?? 0) > 0
                                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
                                width: timerLabel.width + 8; height: 16; radius: 3; color: "#cc0b0b14"
                                Text { id: timerLabel; anchors.centerIn: parent; text: root.formatTime(cell.modelData.nextTimer ?? 0); color: Theme.dangerLight; font.family: root.mono; font.pixelSize: 12 }
                            }
                        }
                        // the label: the number, the group's name, a line in the group's colour under it
                        Rectangle {
                            id: labelBar
                            y: thumb.height; width: parent.width; height: 25
                            color: root.cDarkest
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: cell.group }
                            Text { x: 5; y: 5; text: cell.index + 1; opacity: 0.85; color: root.cText; font.family: root.mono; font.pixelSize: 13 }
                            Text {
                                x: 22; y: 4; width: parent.width - 44
                                horizontalAlignment: Text.AlignHCenter
                                text: (cell.modelData.title ?? "") !== "" ? cell.modelData.title : "—"
                                color: root.cText; elide: Text.ElideRight
                                font.family: root.mono; font.pixelSize: 13; font.weight: Font.Bold
                            }
                        }
                        // the chosen slide: FreeShow's 2px outline in the accent, inside the cell
                        Rectangle { anchors.fill: parent; color: "transparent"; border.width: cell.selected ? 2 : 0; border.color: root.cSecondary }
                    }
                    HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.slideTapped(cell.index, cell.modelData, cell.styled)
                        onDoubleTapped: root.takeSlideLive(cell.index, cell.modelData, cell.styled)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.editRequested(root.item.ref)
                    }
                }
            }
        }

        // one row a slide: a small picture, its group, its words
        Column {
            id: listCol
            visible: root.viewMode === "list"
            width: parent.width
            spacing: 6
            Repeater {
                model: root.viewMode === "list" ? root.slides : []
                delegate: Rectangle {
                    id: listRow
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.selectedSlide === index
                    // Same active-style-template composition as the grid
                    // cell's own `styled` — see its comment.
                    readonly property var styled: {
                        void root.reload
                        void root.styleRevision
                        void root.templateCatalogRevision
                        void root.showTemplateId
                        return LiveOutputService.previewWithActiveStyle(root.renderSlide(listRow.modelData))
                    }
                    property var pendingTextEdits: ({})
                    property var inFlightTextEdits: ({})
                    property int quickEditToken: 0
                    width: listCol.width
                    height: Math.max(80, textItemsColumn.y + textItemsColumn.implicitHeight + 10)
                    radius: 6
                    color: selected ? Theme.activeBg : (listHover.hovered ? Theme.surface : Theme.panelBg)
                    // Purple selection — the app-wide selection accent
                    // (see cSecondary above).
                    border.color: selected ? Theme.accent : Theme.borderSubtle
                    Rectangle {
                        x: 10; y: 10; width: 106; height: StageGrid.heightForWidth(width); color: "#000"; radius: 3; clip: true
                        DesignPreview {
                            width: parent.width
                            showBindPlaceholders: false
                            blocks: listRow.styled.blocks ?? []
                            background: listRow.styled.background ?? "transparent"
                            // Same one answer as the grid cell above.
                            showCheckerboard: listRow.styled.frameTransparent === true
                        }
                    }
                    Text { x: 130; y: 10; text: (listRow.index + 1) + "  " + (listRow.modelData.title ?? "") + ((listRow.modelData.nextTimer ?? 0) > 0 ? "  ·  " + root.formatTime(listRow.modelData.nextTimer) : ""); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
                    // Edit each text item in place. Rebuilding a rich slide from
                    // joined plain text discarded its image and block geometry.
                    Column {
                        id: textItemsColumn
                        x: 130; y: 32; width: parent.width - 146
                        spacing: 4
                        Repeater {
                            model: root.slideTextItems(listRow.modelData)
                            delegate: TextEdit {
                                required property var modelData
                                width: textItemsColumn.width
                                text: modelData.text
                                color: Theme.textSecondary
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                font.family: Theme.fontFamily; font.pixelSize: 14
                                property bool ready: false
                                Component.onCompleted: ready = true
                                onTextChanged: {
                                    if (!ready) return
                                    const edits = Object.assign({}, listRow.pendingTextEdits)
                                    edits[String(modelData.blockId)] = text
                                    listRow.pendingTextEdits = edits
                                    rowSaveTimer.restart()
                                }
                                onActiveFocusChanged: if (!activeFocus && rowSaveTimer.running) {
                                    rowSaveTimer.stop()
                                    listRow.flushEdit()
                                }
                            }
                        }
                    }
                    Timer {
                        id: rowSaveTimer
                        interval: 700
                        onTriggered: listRow.flushEdit()
                    }
                    function flushEdit() {
                        if (!root.item || Object.keys(listRow.pendingTextEdits).length === 0) return
                        listRow.inFlightTextEdits = listRow.pendingTextEdits
                        listRow.quickEditToken = ShowService.updateShowTextItems(
                                    root.item.ref, String(listRow.modelData.id ?? ""),
                                    listRow.inFlightTextEdits)
                        listRow.pendingTextEdits = ({})
                    }
                    Connections {
                        target: ShowService
                        function onDocumentJobFinished(token, kind, ok) {
                            if (kind !== "quickEdit" || token !== listRow.quickEditToken) return
                            listRow.quickEditToken = 0
                            if (ok) root.reload++
                            else listRow.pendingTextEdits = Object.assign(
                                        {}, listRow.inFlightTextEdits, listRow.pendingTextEdits)
                            listRow.inFlightTextEdits = ({})
                            if (Object.keys(listRow.pendingTextEdits).length > 0)
                                rowSaveTimer.restart()
                        }
                    }
                    HoverHandler { id: listHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.slideTapped(listRow.index, listRow.modelData, listRow.styled)
                        onDoubleTapped: root.takeSlideLive(listRow.index, listRow.modelData, listRow.styled)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.editRequested(root.item.ref)
                    }
                }
            }
        }

        Column {
            id: lyricsCol
            visible: root.viewMode === "lyrics" && !root.hasRichContent
            x: Math.max(0, (parent.width - width) / 2)
            width: Math.min(parent.width, 760)

            property string editingPath: ""
            property bool loaded: false

            function flushPending() {
                if (autosaveTimer.running && lyricsCol.editingPath !== "") {
                    autosaveTimer.stop()
                    ShowService.rewriteShowFromText(lyricsCol.editingPath, lyricsEditor.text)
                }
            }
            function loadText() {
                lyricsCol.flushPending()
                lyricsEditor.text = root.slides.map((s) => root.slideText(s)).join("\n\n")
                lyricsCol.editingPath = root.item ? root.item.ref : ""
                lyricsCol.loaded = true
            }
            onVisibleChanged: if (visible) lyricsCol.loadText(); else lyricsCol.flushPending()
            Connections {
                target: root
                function onItemChanged() { if (lyricsCol.visible && root.type === "show") lyricsCol.loadText() }
            }

            TextEdit {
                id: lyricsEditor
                width: lyricsCol.width
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 17
                selectByMouse: true
                onTextChanged: if (lyricsCol.loaded) autosaveTimer.restart()
            }

            Timer {
                id: autosaveTimer
                interval: 700
                onTriggered: {
                    if (lyricsCol.editingPath !== "")
                        ShowService.rewriteShowFromText(lyricsCol.editingPath, lyricsEditor.text)
                }
            }
        }
        Rectangle {
            visible: root.viewMode === "lyrics" && root.hasRichContent
            anchors.centerIn: parent
            width: Math.min(parent.width - 40, 520)
            height: noticeColumn.implicitHeight + 32
            radius: 8
            color: Theme.surface
            border.color: Theme.borderSubtle
            Column {
                id: noticeColumn
                anchors.centerIn: parent
                width: parent.width - 32
                spacing: 10
                Text {
                    width: parent.width
                    text: qsTr("This show contains positioned text and media items. Edit text in List view to preserve its layout.")
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }
                AppButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Open List view")
                    variant: "secondary"
                    onClicked: SettingsService.setValue("session.slideView", "list")
                }
            }
        }
    }

    // ---- the floating bar (FreeShow's FloatingInputs on the slide area): a chevron that shows the zoom, and the view button ----
    Rectangle {
        id: floatingBar
        visible: root.type === "show" && root.show !== null && root.slides.length > 0
        z: 5
        anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.rightMargin: 12; anchors.bottomMargin: 10
        height: 40; radius: 20
        width: barRow.width + 2
        color: "#d9191923"; border.color: root.cLighter

        Row {
            id: barRow
            x: 1; y: 1; height: parent.height - 2

            Item {
                objectName: "selfTestShowPreviousSlide"
                visible: root.type === "show"
                width: visible ? 34 : 0; height: parent.height
                opacity: root.selectedSlide > 0 ? 1 : 0.38
                Rectangle {
                    anchors.centerIn: parent; width: 30; height: 30; radius: 15; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: prevSlideHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "previous"; color: Theme.controlInk; width: 14; height: 14; fit: true }
                HoverHandler { id: prevSlideHover; cursorShape: root.selectedSlide > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler { enabled: root.selectedSlide > 0; onTapped: root.stepSlide(-1) }
            }
            Item {
                objectName: "selfTestShowNextSlide"
                visible: root.type === "show"
                width: visible ? 34 : 0; height: parent.height
                opacity: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1 ? 1 : 0.38
                Rectangle {
                    anchors.centerIn: parent; width: 30; height: 30; radius: 15; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: nextSlideHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "next"; color: Theme.controlInk; width: 14; height: 14; fit: true }
                HoverHandler { id: nextSlideHover; cursorShape: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1 ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler { enabled: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1; onTapped: root.stepSlide(1) }
            }

            // chevron: opens the bar (the zoom hides behind it, like FreeShow's)
            Item {
                objectName: "selfTestBarChevron"
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: chevHover.hovered; glowColor: Theme.hoverBg }
                }
                // a chevron drawn as two bars, so it sits dead centre: > while the bar is open, < while it is shut
                Item {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    opacity: root.barOpen ? 1 : 0.65
                    rotation: root.barOpen ? 0 : 180
                    Behavior on rotation { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                    Rectangle { x: 4.5 - width / 2; y: 3.4 - height / 2; width: 8; height: 2; radius: 1; color: Theme.accent; rotation: 45 }
                    Rectangle { x: 4.5 - width / 2; y: 8.6 - height / 2; width: 8; height: 2; radius: 1; color: Theme.accent; rotation: -45 }
                }
                HoverHandler { id: chevHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { root.barOpen = !root.barOpen; if (!root.barOpen) root.zoomOpen = false } }
            }

            Rectangle { width: 1; height: parent.height; color: root.cLighter }

            // the next timer: red when the slides have one
            Item {
                id: timerButton
                objectName: "selfTestBarClock"
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: timerHover.hovered || root.timerOpen; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "clock"; color: root.timerTotal > 0 ? Theme.accent : Theme.controlInk; fit: true; width: 16; height: 16 }
                HoverHandler { id: timerHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        if (!root.timerOpen) {
                            const first = root.slides.length > 0 ? Math.round(root.slides[0].nextTimer ?? 0) : 0
                            root.timerSeconds = first > 0 ? first : 10
                        }
                        root.timerOpen = !root.timerOpen
                        root.zoomOpen = false
                    }
                }

                // the popover over the button
                Rectangle {
                    visible: root.timerOpen
                    x: parent.width - width + 1; y: -height - 8
                    width: 252; height: timerPanel.height + 28
                    radius: 12; color: "#f018192a"; border.color: root.cLighter
                    MouseArea { anchors.fill: parent; onClicked: (mouse) => { mouse.accepted = true } }
                    Column {
                        id: timerPanel
                        x: 16; y: 14; width: parent.width - 32
                        spacing: 12

                        Text { text: qsTr("NEXT TIMER"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1.2 }

                        // seconds: - [ 10 ] +
                        Rectangle {
                            width: parent.width; height: 40; radius: 8; color: Theme.windowBg; border.color: Theme.border
                            Item {
                                width: 40; height: parent.height
                                opacity: root.timerSeconds > 0 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.max(0, root.timerSeconds - 1) }
                            }
                            Row {
                                anchors.centerIn: parent; spacing: 6
                                TextInput {
                                    id: timerInput
                                    width: 44; horizontalAlignment: TextInput.AlignRight
                                    color: Theme.controlInk; selectByMouse: true
                                    font.family: Theme.fontFamily; font.pixelSize: 18; font.weight: Font.DemiBold
                                    validator: IntValidator { bottom: 0; top: 3600 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    onTextEdited: root.timerSeconds = Number(text) || 0
                                    Component.onCompleted: text = String(root.timerSeconds)
                                    Connections {
                                        target: root
                                        function onTimerSecondsChanged() { if (timerInput.text !== String(root.timerSeconds)) timerInput.text = String(root.timerSeconds) }
                                    }
                                }
                                Text { anchors.baseline: timerInput.baseline; text: qsTr("seconds"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 14 }
                            }
                            Item {
                                anchors.right: parent.right
                                width: 40; height: parent.height
                                opacity: root.timerSeconds < 3600 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                                Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: Theme.controlInk }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.min(3600, root.timerSeconds + 1) }
                            }
                        }

                        // apply it to every slide, or take it off again
                        Rectangle {
                            width: parent.width; height: 40; radius: 20
                            readonly property bool usable: root.timerApplied || root.timerSeconds > 0
                            opacity: usable ? 1 : 0.4
                            color: root.timerApplied ? (applyHover.hovered ? Theme.hoverBg : "transparent") : (applyHover.hovered ? Theme.accentLight : Theme.accent)
                            border.color: root.timerApplied ? Theme.border : Theme.accent
                            Row {
                                anchors.centerIn: parent; spacing: 8
                                Text { text: root.timerApplied ? qsTr("Reset") : qsTr("To all slides"); color: Theme.controlInk; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.DemiBold }
                                Text { visible: !root.timerApplied && root.timerSeconds > 0; text: root.formatTime(root.timerSeconds * root.slides.length); color: Theme.dangerLight; font.family: Theme.fontFamily; font.pixelSize: 14 }
                            }
                            HoverHandler { id: applyHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            TapHandler { enabled: parent.usable; onTapped: root.applyTimer() }
                        }

                        Text {
                            visible: root.timerTotal > 0
                            width: parent.width; horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Duration: %1").arg(root.formatTime(root.timerTotal))
                            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                    }
                }
            }

            Rectangle { visible: root.barOpen; width: 1; height: parent.height; color: root.cLighter }

            // zoom: the percentage, + and -, in a popup over the button
            Item {
                id: zoomButton
                visible: root.barOpen
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: zoomHover.hovered || root.zoomOpen; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "zoomIn"; color: root.zoomOpen ? Theme.controlInk : Theme.navLabelMuted; fit: true; width: 16; height: 16 }
                HoverHandler { id: zoomHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { root.zoomOpen = !root.zoomOpen; root.timerOpen = false } }

                Rectangle {
                    visible: root.zoomOpen
                    x: -1; y: -height - 2; width: parent.width + 2; height: 122
                    radius: 20; color: "#f018192a"; border.color: root.cLighter
                    // (rounded top only: the bottom sits flat on the button)
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 20; color: parent.color }
                    Rectangle { anchors.bottom: parent.bottom; anchors.bottomMargin: -1; x: parent.border.width; width: parent.width - 2; height: 2; color: parent.color }
                    // Same click-through fix as the timer popover: swallow
                    // clicks on the popover's own padding/gaps.
                    MouseArea { anchors.fill: parent; onClicked: (mouse) => { mouse.accepted = true } }
                    Column {
                        anchors.fill: parent; anchors.topMargin: 1
                        Item {
                            width: parent.width; height: 38
                            Text { anchors.centerIn: parent; text: Math.round(100 / root.columns) + "%"; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13 }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(4) }   // the usual size
                        }
                        Rectangle { width: parent.width; height: 1; color: root.cLighter }
                        // + makes the slides bigger (fewer across)
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns > 2 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                            Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: Theme.controlInk }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(root.columns - 1) }
                        }
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns < 10 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(root.columns + 1) }
                        }
                    }
                }
            }

            Rectangle { visible: root.barOpen; width: 1; height: parent.height; color: root.cLighter }

            // the view: grid -> list -> lyrics
            Item {
                width: 44; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 34; height: 34; radius: 17; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: viewHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph {
                    anchors.centerIn: parent
                    name: root.viewMode === "list" ? "listView" : (root.viewMode === "lyrics" ? "textLines" : "gridView")
                    color: root.viewMode === "grid" ? Theme.controlInk : Theme.navLabelMuted
                    fit: true; width: 18; height: 18
                }
                HoverHandler { id: viewHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nextView() }
            }
        }
    }

    // ---- the sliding timer bar: a show that carries timers shows its pace at a glance ----
    // The track is there whenever the show has a next timer on any slide; the fill answers the on-air
    // slide's OWN countdown (LiveOutputService), so it slides as each slide's time runs out. It only
    // fills while THIS show is the one the timer was recorded for (another show on air must not move it).
    Rectangle {
        id: timerBar
        objectName: "selfTestTimerBar"
        visible: root.type === "show" && root.show !== null && root.timerTotal > 0
        z: 3
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 4
        color: "#33000000"
        readonly property bool live: LiveOutputService.autoAdvancing
            && String(LiveOutputService.autoAdvanceShow).toLowerCase() === String(root.show ? root.show.name : "").toLowerCase()
        Rectangle {
            width: timerBar.live ? Math.round(parent.width * LiveOutputService.timerProgress) : 0
            height: parent.height
            color: Theme.accent
            Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.Linear } }
        }
    }

    MouseArea {
        anchors.fill: parent
        z: 4
        enabled: root.timerOpen || root.zoomOpen
        onClicked: { root.timerOpen = false; root.zoomOpen = false }
    }

    Text {
        visible: root.type === "show" && root.show === null
        anchors.centerIn: parent
        text: root.loaded && root.loaded.error ? qsTr("This show could not be read: %1").arg(root.loaded.error) : ""
        color: Theme.dangerLight; font.family: Theme.fontFamily; font.pixelSize: 15
    }

    // ---- a picture ----
    Image {
        visible: root.type === "image"
        // Start below the 30px preview header so its dark surface never covers
        // the image; use the dock divider as the lower edge to avoid a gutter.
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 40
        anchors.bottomMargin: 0
        fillMode: root.imageFit === "cover" ? Image.PreserveAspectCrop
                 : root.imageFit === "fill" ? Image.Stretch : Image.PreserveAspectFit
        asynchronous: true
        source: root.type === "image" && root.item ? "file:///" + String(root.item.ref).replace(/\\/g, "/") : ""

        // A single click previews the picture here; a double-click sends it
        // to the monitor output, matching the media library's take gesture.
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: {
                if (root.item)
                    LiveOutputService.takeMedia(String(root.item.ref), String(root.item.name ?? ""))
            }
        }

        // Still images also need an obvious take-to-monitor control in the
        // centre preview, matching the play/take affordance on video previews.
        Rectangle {
            visible: root.item !== null
                     && !(LiveOutputService.mediaOnAir
                          && LiveOutputService.mediaPath === String(root.item.ref))
            anchors.centerIn: parent
            width: 64; height: 64; radius: 32
            color: "#b0000000"
            border.width: 2
            border.color: Theme.accent
            IconGlyph {
                anchors.centerIn: parent
                name: "play"
                color: Theme.controlInk
                fit: true
                width: 26; height: 26
            }
            PositionHoverArea {
                anchors.fill: parent
                onClicked: {
                    if (root.item)
                        LiveOutputService.takeMedia(String(root.item.ref), String(root.item.name ?? ""))
                }
            }
        }
    }

    // ---- an overlay ----
    Item {
        visible: root.type === "overlay"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        readonly property var design: root.type === "overlay" ? OverlayLibraryService.design(root.item.ref) : ({})
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, StageGrid.widthForHeight(parent.height)); height: StageGrid.heightForWidth(width)
            color: "#000"; clip: true
            DesignPreview {
                anchors.fill: parent
                blocks: parent.parent.design.blocks ?? []
                background: parent.parent.design.background ?? "transparent"
            }
        }
    }

    // ---- a passage: the slide the engine builds for it, with the reference under ----
    Item {
        visible: root.type === "scripture" && scriptureView.ready
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        QtObject {
            id: scriptureView
            readonly property var meta: root.type === "scripture" && root.item ? (root.item.meta ?? {}) : ({})
            readonly property bool ready: meta.bible !== undefined && meta.book !== undefined
            readonly property var design: ready ? ScriptureService.preview(meta.bible, meta.book, meta.chapter, meta.verses ?? []) : ({})
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, StageGrid.widthForHeight(parent.height)); height: StageGrid.heightForWidth(width)
            color: "#000"; clip: true
            DesignPreview {
                anchors.fill: parent
                blocks: scriptureView.design.blocks ?? []
                background: scriptureView.design.background ?? "transparent"
            }
        }
    }

    // ---- a video: the live frame + the on-air transport -----------------
    Item {
        id: mediaView
        visible: root.type === "media" || root.type === "video"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56

        readonly property string filePath: root.item ? String(root.item.ref) : ""
        readonly property bool onAir: LiveOutputService.mediaOnAir
                                      && LiveOutputService.mediaPath === filePath
        readonly property bool isVideoFile: {
            const e = filePath.substring(filePath.lastIndexOf(".") + 1).toLowerCase()
            return ["mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv"].indexOf(e) >= 0
        }
        // On air the shared live-media surface draws the file (see LiveMediaSurface); off air this is
        // the file's poster.
        readonly property url previewSource: onAir
            ? ""
            : "image://mediathumb/500/-1/" + encodeURIComponent(filePath)
        readonly property bool playing: LiveOutputService.mediaState === "playing"

        property int fitPulse: 0
        Connections {
            target: MediaLibraryService
            function onFoldersChanged() { mediaView.fitPulse++ }
        }
        // While the file is on air, use the compositor's effective fit;
        // otherwise use this file's optional override.
        readonly property string storedFit: {
            void mediaView.fitPulse
            return MediaLibraryService.itemFit(mediaView.filePath)
        }
        readonly property string shownFit: mediaView.onAir ? LiveOutputService.mediaFit
            : (mediaView.storedFit !== "" ? mediaView.storedFit : LiveOutputService.defaultMediaFit)
        readonly property int previewFill: mediaView.shownFit === "cover" ? Image.PreserveAspectCrop
                                            : mediaView.shownFit === "fill" ? Image.Stretch
                                                                            : Image.PreserveAspectFit

        // The media preview follows the saved file override or Outputs default.
        Rectangle {
            id: videoBox
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: Math.min(parent.width, parent.height * 16 / 9)
            height: Math.round(width * 9 / 16)
            // Letterboxed media uses the preview surface, not a black box
            // that appears around the image when it is taken to output.
            color: root.cDarkest
            radius: 4
            clip: true

            Image {
                anchors.fill: parent
                visible: !mediaView.onAir
                source: mediaView.previewSource
                // The picture laid out by ITS OWN fill, so the preview answers
                // the same question the outputs do.
                fillMode: mediaView.previewFill
                cache: false
                asynchronous: true
            }

            // On air: the one shared surface — GPU frames from the player that decodes the file
            // (the provider path on a machine with no GPU).
            LiveMediaSurface {
                anchors.fill: parent
                active: mediaView.onAir
                fit: mediaView.shownFit
            }

            // Not on air: the centre overlay IS the take button.
            Rectangle {
                visible: !mediaView.onAir
                anchors.centerIn: parent
                width: 64; height: 64; radius: 32
                color: "#b0000000"
                border.width: 2
                border.color: Theme.accent
                IconGlyph {
                    anchors.centerIn: parent
                    name: "play"
                    color: Theme.controlInk
                    fit: true
                    width: 26; height: 26
                }
                PositionHoverArea {
                    anchors.fill: parent
                    showCursor: false
                    onClicked: {
                        if (mediaView.isVideoFile)
                            LiveOutputService.takeMedia(mediaView.filePath, root.item.name)
                        else
                            root.editRequested(mediaView.filePath)
                    }
                }
            }
        }

        // ---- the transport (videos only; an on-air still shows just OFF AIR)
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: videoBox.bottom
            anchors.topMargin: 10
            width: Math.min(videoBox.width, 560)
            spacing: 8
            visible: mediaView.isVideoFile

            // Seek scrubber + time readout. mediaPosition pulses at the
            // player's rate while playing — the scrubber tracks it.
            Item {
                id: seekBar
                width: parent.width
                height: 22

                readonly property real frac: mediaView.onAir && LiveOutputService.mediaDuration > 0
                    ? Math.max(0, Math.min(1, LiveOutputService.mediaPosition / LiveOutputService.mediaDuration)) : 0

                // Track.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Theme.toggleOffTrack
                }
                // Fill + thumb.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * seekBar.frac
                    height: 4
                    radius: 2
                    color: Theme.accent
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Math.max(0, parent.width * seekBar.frac - 6)
                    width: 12; height: 12; radius: 6
                    color: Theme.controlInk
                    visible: mediaView.onAir
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: mediaView.onAir
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (mouse) => LiveOutputService.mediaSeek(mouse.x / width)
                }
                Text {
                    anchors.right: parent.right
                    anchors.top: parent.bottom
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 11
                    text: mediaView.onAir
                        ? LiveOutputService.formatMediaTime(LiveOutputService.mediaPosition)
                          + " / " + LiveOutputService.formatMediaTime(LiveOutputService.mediaDuration)
                        : ""
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Rectangle {
                    visible: mediaView.onAir
                    width: 34; height: 34; radius: 17
                    color: Theme.chip
                    IconGlyph {
                        anchors.centerIn: parent
                        name: mediaView.playing ? "pause" : "play"
                        color: Theme.textPrimary
                        fit: true; width: 15; height: 15
                    }
                    PositionHoverArea {
                        anchors.fill: parent
                        showCursor: false
                        onClicked: LiveOutputService.mediaTogglePlay()
                    }
                }
                Rectangle {
                    visible: mediaView.onAir
                    width: 34; height: 34; radius: 17
                    color: Theme.chip
                    IconGlyph {
                        anchors.centerIn: parent
                        name: LiveOutputService.mediaMuted ? "close" : "volume2"
                        color: LiveOutputService.mediaMuted ? Theme.dangerLight : Theme.textPrimary
                        fit: true; width: 15; height: 15
                    }
                    PositionHoverArea {
                        anchors.fill: parent
                        showCursor: false
                        onClicked: LiveOutputService.mediaToggleMuted()
                    }
                }
            }
        }
    }

    // ---- everything else: what it is ----
    Column {
        visible: root.item !== null && root.type !== "show" && root.type !== "image" && root.type !== "overlay"
                 && root.type !== "media" && root.type !== "video"
                 && !(root.type === "scripture" && scriptureView.ready)
        anchors.centerIn: parent
        spacing: 10
        IconGlyph {
            anchors.horizontalCenter: parent.horizontalCenter
            name: root.type === "audio" ? "music" : root.type === "video" || root.type === "media" ? "play" : root.type === "scripture" ? "bible"
                : root.type === "camera" ? "camera" : root.type === "pdf" ? "fileText" : "layoutDashboard"
            color: Theme.textMuted; fit: true; width: 40; height: 40
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type === "section"
            text: root.item ? root.item.name : ""
            color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 30; font.weight: Font.DemiBold; font.capitalization: Font.AllUppercase
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type !== "section" && root.item !== null
            width: Math.min(root.width - 60, 520); horizontalAlignment: Text.AlignHCenter
            text: root.item ? String(root.item.ref) : ""
            color: Theme.textMuted; elide: Text.ElideMiddle
            font.family: Theme.fontFamily; font.pixelSize: 14
        }
    }
}