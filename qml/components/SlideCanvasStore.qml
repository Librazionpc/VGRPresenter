import QtQuick

// Per-slide canvas state, keyed by SlideListModel's stable slide id (NOT the
// display `num` — that renumbers whenever slides are inserted/removed).
//
// `current` is the working set EditScreen's canvas renders and edits live;
// `slides` archives each slide's { items, background } so switching slides
// swaps the canvas: load() re-archives the outgoing slide under its id and
// installs the incoming one. The archived item objects are the SAME objects
// that were on the canvas (no copies), so thumbnails can bind directly to
// their properties and update in real time while the slide is being edited.
//
// Slide lifecycle is driven by the consumer (EditScreen) — this store never
// touches SlideListModel itself:
//   add slide     -> nothing needed; the new id simply has no archive yet
//   duplicate     -> cloneSlide(fromId, duplicateSlide()'s returned id)
//   remove        -> dropSlide(id) BEFORE removeSlide(), so the archive and
//                    its item objects die with the slide
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
        // One spelling of "no background" lives in BackgroundColorModal
        // (transparentValue); this is the state-side equivalent for a slide
        // that has never had a background picked — keep the two in sync.
        property string background: "transparent"
    }

    // Explicit property, not a bare child — QtObject has no default/children
    // property, so an unqualified Component{} here fails to load.
    property Component canvasItemComponent: Component { CanvasItem {} }

    // ---- Save / load -------------------------------------------------------

    // Archives the current working set under the active slide's id. A fully
    // empty, never-archived slide is skipped (no entry appears until the
    // slide actually holds something or a background was picked).
    function save() {
        if (store.activeSlideId < 0)
            return
        const items = store.current.items
        if (items.length > 0 || store.slides[store.activeSlideId] !== undefined)
            store.slides = Object.assign({}, store.slides,
                { [store.activeSlideId]: { items: items, background: store.current.background } })
    }

    // Makes `slideId` active: re-archives the outgoing slide, then installs
    // the incoming one's archive (or a fresh empty set for a brand-new id).
    function load(slideId) {
        store.save()
        store.activeSlideId = slideId
        const entry = store.slides[slideId]
        store.current.items = entry ? entry.items : []
        store.current.background = entry ? entry.background : store.current.background
    }

    // ---- Slide lifecycle ---------------------------------------------------

    // Forgets a slide's archive and destroys its item objects. Call BEFORE
    // SlideListModel.removeSlide() — if the removed slide is active, the
    // working set is cleared first so the following load() can't re-archive
    // dying objects.
    function dropSlide(slideId) {
        const entry = store.slides[slideId]
        if (!entry)
            return
        if (slideId === store.activeSlideId)
            store.current.items = []
        entry.items.forEach((it) => it.destroy())
        const copy = Object.assign({}, store.slides)
        delete copy[slideId]
        store.slides = copy
    }

    // Clones a slide's whole archive (items keep their exact positions; each
    // clone gets fresh keys and its own style copy) onto the new id returned
    // by SlideListModel.duplicateSlide().
    function cloneSlide(fromSlideId, toSlideId) {
        const entry = store.slides[fromSlideId]
        if (!entry || entry.items.length === 0)
            return
        const clones = entry.items.map((it) => store.cloneItem(it, 0, 0))
        store.slides = Object.assign({}, store.slides,
            { [toSlideId]: { items: clones, background: entry.background } })
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

    // Creates a CanvasItem owned by the store. When `styleFrom` is given,
    // the new item starts with a copy of that style; otherwise the new item
    // uses CanvasItem's own defaults (border off, radius 0, no padding).
    function createItem(kind, text, x, y, width, height, styleFrom) {
        const item = canvasItemComponent.createObject(null, {
            key: "item-" + (store.m_nextKey++),
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
        }
        return item
    }

    function cloneItem(src, offsetX, offsetY) {
        return store.createItem(src.kind, src.text, src.x + offsetX, src.y + offsetY,
                                src.width, src.height, src.style)
    }

    // ---- Live thumbnails -----------------------------------------------------

    // Every item of any slide, in canvas order — the real CanvasItem
    // objects (not copies), so SlideListItem's preview binds straight to
    // their properties and re-renders the instant the canvas is edited.
    // An inactive slide reads its archive; the active one reads the live
    // set. NO filtering on item contents (text etc.) here: a filter inside
    // this function would evaluate only when the ARRAY changes, so a bound
    // thumbnail would never notice a property edit like typed text. The
    // consumer filters structurally (by kind) and binds to item properties
    // for the live parts.
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
