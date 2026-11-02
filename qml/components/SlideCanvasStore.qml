import QtQuick

QtObject {
    id: store

    // The slide whose items `current` holds. Maintained by load(); -1 means
    // no slide is active (empty roster) and save() is a no-op.
    property int activeSlideId: -1

    // slideId -> { items: [CanvasItem], background: string }
    property var slides: ({})

    // The live working set for the active slide.
    readonly property QtObject current: QtObject {
        property var items: []
        property string background: "transparent"
        property var filters: ({ hueRotate: 0.0, invert: 0.0, blur: 0.0, grayscale: 0.0,
                                 brightness: 1.0, contrast: 1.0, saturate: 1.0 })
    }

    function identityFilters() {
        return ({ hueRotate: 0.0, invert: 0.0, blur: 0.0, grayscale: 0.0,
                  brightness: 1.0, contrast: 1.0, saturate: 1.0 })
    }

    // True when the map is every value's identity — the state in which neither
    // the canvas effect nor the engine's filter pass runs at all.
    function filtersAreIdentity(f) {
        if (!f)
            return true
        return f.hueRotate === 0 && f.invert === 0 && f.blur === 0 && f.grayscale === 0
            && f.brightness === 1 && f.contrast === 1 && f.saturate === 1
    }

    // Explicit property, not a bare child — QtObject has no default/children
    // property, so an unqualified Component{} here fails to load.
    property Component canvasItemComponent: Component { CanvasItem {} }

    // ---- Save / load -------------------------------------------------------

    function save() {
        if (store.activeSlideId < 0)
            return
        const items = store.current.items
        if (items.length > 0 || store.slides[store.activeSlideId] !== undefined)
            store.slides = Object.assign({}, store.slides,
                { [store.activeSlideId]: { items: items, background: store.current.background,
                                           filters: store.current.filters } })
    }

    // Makes `slideId` active: re-archives the outgoing slide, then installs
    // the incoming one's archive (or a fresh empty set for a brand-new id).
    function load(slideId) {
        store.save()
        store.activeSlideId = slideId
        const entry = store.slides[slideId]
        store.current.items = entry ? entry.items : []
        store.current.background = entry ? entry.background : store.current.background
        // A slide that has never had a filter dialled keeps the identity set —
        // the incoming slide's own values, never the outgoing one's.
        store.current.filters = entry && entry.filters ? entry.filters : store.identityFilters()
    }

    // ---- Slide lifecycle ---------------------------------------------------

    function dropSlide(slideId) {
        const entry = store.slides[slideId]
        if (!entry)
            return
        // The ACTIVE slide's live items are `current.items`: its archive may still list objects that an undo/redo
        // (restoreItems) has since destroyed and replaced, and destroying those again is a TypeError.
        const live = slideId === store.activeSlideId ? store.current.items : entry.items
        if (slideId === store.activeSlideId)
            store.current.items = []
        live.forEach((it) => it.destroy())
        const copy = Object.assign({}, store.slides)
        delete copy[slideId]
        store.slides = copy
    }

    function cloneSlide(fromSlideId, toSlideId) {
        const entry = store.slides[fromSlideId]
        if (!entry || entry.items.length === 0)
            return
        const clones = entry.items.map((it) => store.cloneItem(it, 0, 0))
        store.slides = Object.assign({}, store.slides,
            { [toSlideId]: { items: clones, background: entry.background,
                             filters: Object.assign({}, entry.filters || store.identityFilters()) } })
    }

    function clear() {
        // Archive what is live first. The active slide's archive can list items an undo/redo has since destroyed and
        // replaced (restoreItems); refreshing it means the loop below only ever destroys objects that still exist.
        store.save()
        store.current.items = []
        store.current.background = "transparent"
        store.current.filters = store.identityFilters()
        for (const id in store.slides)
            store.slides[id].items.forEach((it) => it.destroy())
        store.slides = {}
        // Park the active id so the active-slide transition's save() (load()
        // opens with one) can't re-archive anything under the dying id.
        store.activeSlideId = -1
    }

    // ---- Content API (operates on the active slide) -------------------------

    function addItem(item) {
        store.current.items = store.current.items.concat([item])
    }

    // Removes the keyed items from the working set (destroying their
    // objects). The next save() re-archives the reduced set.
    function removeItems(keys) {
        store.current.items = store.current.items.filter((it) => {
            if (keys.indexOf(it.key) < 0)
                return true
            it.destroy()
            return false
        })
    }

    function createItem(kind, text, x, y, width, height, styleFrom, key) {
        const item = canvasItemComponent.createObject(null, {
            key: key !== undefined && key !== "" ? key : "item-" + (store.m_nextKey++),
            kind: kind,
            text: text,
            x: x, y: y, width: width, height: height
        })
        if (styleFrom) {
            item.style.padding = styleFrom.padding
            item.style.backgroundColor = styleFrom.backgroundColor
            item.style.borderEnabled = styleFrom.borderEnabled
            item.style.borderWidth = styleFrom.borderWidth
            item.style.borderStyle = styleFrom.borderStyle
            item.style.cornerRadius = styleFrom.cornerRadius
            item.style.borderColor = styleFrom.borderColor
            item.style.opacity = styleFrom.opacity
            item.style.mediaFit = styleFrom.mediaFit
        }
        return item
    }

    function cloneItem(src, offsetX, offsetY) {
        const copy = store.createItem(src.kind, src.text, src.x + offsetX, src.y + offsetY,
                                      src.width, src.height, src.style)
        copy.bind = src.bind
        return copy
    }

    // ---- Undo/redo support (EditScreen.qml) -----------------------------

    function snapshotOf(items) {
        return items.map((it) => ({
            key: it.key, kind: it.kind, text: it.text,
            x: it.x, y: it.y, width: it.width, height: it.height,
            bind: it.bind,
            meta: it.meta,
            style: {
                padding: it.style.padding, backgroundColor: it.style.backgroundColor.toString(),
                cornerRadius: it.style.cornerRadius, borderEnabled: it.style.borderEnabled,
                borderWidth: it.style.borderWidth, borderStyle: it.style.borderStyle,
                borderColor: it.style.borderColor.toString(),
                opacity: it.style.opacity, mediaFit: it.style.mediaFit
            }
        }))
    }
    function snapshotItems() {
        return store.snapshotOf(store.current.items)
    }

    function itemFromBlock(d) {
        const item = store.canvasItemComponent.createObject(null, {
            key: d.key, kind: d.kind, text: d.text,
            x: d.x, y: d.y, width: d.width, height: d.height,
            bind: d.bind !== undefined ? d.bind : ""
        })
        item.meta = d.meta !== undefined ? d.meta : ({})
        if (d.style) {
            item.style.padding = d.style.padding
            item.style.backgroundColor = d.style.backgroundColor
            item.style.cornerRadius = d.style.cornerRadius
            item.style.borderEnabled = d.style.borderEnabled
            item.style.borderWidth = d.style.borderWidth
            item.style.borderStyle = d.style.borderStyle
            item.style.borderColor = d.style.borderColor
            // Defensive defaults: a show saved before this field existed has
            // no opacity/mediaFit in its .vgr block data at all.
            item.style.opacity = d.style.opacity !== undefined ? d.style.opacity : 1.0
            const metaFit = d.meta && d.meta.media && d.meta.media.fit ? String(d.meta.media.fit) : ""
            item.style.mediaFit = d.style.mediaFit !== undefined ? d.style.mediaFit
                                : metaFit !== "" ? metaFit : "contain"
        }
        return item
    }
    function itemsFromBlocks(blocks) {
        return blocks.map((d) => store.itemFromBlock(d))
    }

    function restoreItems(snapshot) {
        store.current.items.forEach((it) => it.destroy())
        store.current.items = store.itemsFromBlocks(snapshot)
    }

    // ---- Engine sync ---------------------------------------------------------
    // A slide's content as engine blocks — the active slide reads the live working
    // set, any other its archive.
    function blocksOf(slideId) {
        return store.snapshotOf(store.items(slideId))
    }
    function backgroundOf(slideId) {
        if (slideId === store.activeSlideId)
            return store.current.background
        const entry = store.slides[slideId]
        return entry ? entry.background : "transparent"
    }
    function filtersOf(slideId) {
        if (slideId === store.activeSlideId)
            return store.current.filters
        const entry = store.slides[slideId]
        return entry && entry.filters ? entry.filters : store.identityFilters()
    }
    function setFilter(key, value) {
        if (store.activeSlideId < 0)
            return
        store.current.filters = Object.assign({}, store.current.filters, { [key]: value })
    }
    // Archives a NON-active slide built from engine data (a duplicate, or every slide
    // of an opened show). Replaces any existing archive (destroying its items).
    function archiveSlide(slideId, blocks, background, filters) {
        const old = store.slides[slideId]
        if (old)
            old.items.forEach((it) => it.destroy())
        store.slides = Object.assign({}, store.slides,
            { [slideId]: { items: store.itemsFromBlocks(blocks), background: background || "transparent",
                           filters: filters ? Object.assign({}, filters) : store.identityFilters() } })
    }

    function replaceSlide(slideId, blocks, background, filters) {
        const items = store.itemsFromBlocks(blocks)
        const textItemCount = items.filter((item) => item.kind === "text").length
        const nextBackground = background || "transparent"
        const nextFilters = filters ? Object.assign({}, filters) : store.identityFilters()
        EngineBridge.log("info", "Text edit", "SlideCanvasStore built " + items.length + " item(s), including "
                         + textItemCount + " text item(s), for slide " + slideId)
        if (slideId === store.activeSlideId) {
            store.current.items.forEach((item) => item.destroy())
            store.current.items = items
            store.current.background = nextBackground
            store.current.filters = nextFilters
            return
        }
        const old = store.slides[slideId]
        if (old)
            old.items.forEach((item) => item.destroy())
        store.slides = Object.assign({}, store.slides,
            { [slideId]: { items: items, background: nextBackground, filters: nextFilters } })
    }

    // ---- Live thumbnails -----------------------------------------------------

    function items(slideId) {
        if (slideId === store.activeSlideId)
            return store.current.items
        const entry = store.slides[slideId]
        return entry ? entry.items : []
    }

    // ---- internals -----------------------------------------------------------

    // Key generator for created items — the store owns the namespace so
    // clones (which may outlive their source slide) can never collide.
    property int m_nextKey: 1
}