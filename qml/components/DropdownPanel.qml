import QtQuick
import VGRPresenterUI

// A menu dropdown panel: optional header (title + subtitle), then a list of
// items built from a plain data model. Each entry in `model` is one of:
//   { divider: true }
//   { label, trailing, danger }   // trailing = shortcut text, a submenu
//                                    chevron ("›"), or any right-aligned hint
//
// `danger` items get red text always (a destructive action, e.g. "Emergency
// Stop") — every other item is neutral text that only tints on hover, so no
// row looks permanently "selected" the way the raw export baked in.
//
// SCROLLING: `maxHeight` (default 0 = size to content, the historical
// behavior every existing menu relies on) caps the panel's height — past
// the cap the item list becomes a Flickable with an AppScrollBar, so a long
// option list (a device picker) scrolls instead of running off-screen.
//
// WINDOW-RESPONSIVE: the cap is only an UPPER bound — the panel also fits
// itself to the room it actually has. `openAt` measures the `bounds` rect
// (the window, or the dialog a field lives in) around the anchor and
// subtracts the space above the anchor's TOP edge, and additionally caps at the
// caller's own `maxHeight`. A menu opened from a row near the window's bottom
// used to size to content, land mostly off-screen, and clamp its own y so its
// top hid under the anchor; now it shrinks and SCROLLS instead. Every
// maxHeight: 0 consumer (menu bar, SelectField, …) gets this for free.
//
// FLYOUT (`openBeside`): a second open mode for a row that owns a long list
// — the panel opens beside the row (right, or left when there is no room)
// with the same fit-and-scroll behavior, and closes with its parent. Used by
// the design library's "Move to …" row so a library with many categories
// scrolls in a submenu instead of growing the parent menu past the window.
//
// Colors read the Theme singleton (panel/row ground, borders, text tones), so
// a Light choice recolours every menu and dropdown with the app. This file
// carried literal hex for a while because Theme resolved to undefined at the
// time — the real cause was Theme being registered as an ORDINARY type rather
// than a singleton, so `Theme.*` was undefined (fixed via
// QT_QML_SINGLETON_TYPE in CMakeLists.txt), not any nesting/AOT limit.
Rectangle {
    id: root

    property var model: []
    property string headerTitle: ""
    property string headerSubtitle: ""
    // 0 = size to content; a positive value caps the height and turns the
    // item list into a scrollable Flickable (see header comment). The room
    // the panel was opened into caps it further (effMaxHeight).
    property int maxHeight: 0

    // ---- measured open geometry (see _reposition) ---------------------------
    // The rect the panel was opened into (the window, or a dialog a field
    // lives in) and what the measurement found: the tallest/widest the panel
    // may draw, and whether it flipped above its anchor.
    property Item openBounds: null
    property int roomCap: 0        // 0 = not measured
    property int widthCap: 0       // 0 = not measured
    property bool flipUp: false
    // True for a panel opened as a FLYOUT beside a row rather than at a
    // point (openBeside) — it aligns to the row and flips to the row's left
    // instead of the anchor's right when the bounds have no room there.
    property bool isFlyout: false
    // Panel padding against the bounds' edges — a menu never touches the
    // window frame.
    readonly property int edgePad: 8

    signal itemActivated(string label)

    // --- autocomplete mode (opt-in) ------------------------------------------
    // An inline suggestion list for a text field (the tab search box's live
    // reference completions): picks carry an arbitrary payload, keyboard
    // Up/Down moves a highlight, Enter/click accepts, and — when
    // `dismissOnOutsideClick` is set — a click anywhere else closes it
    // (AppMenuBar's menus close via the menu bar's own catcher, so the flag
    // is off there and the behavior is unchanged for every existing menu).
    property bool dismissOnOutsideClick: false
    property int highlightedIndex: -1
    signal itemPicked(int index, var payload)

    // The field's own click is the toggle: without this the catcher would eat
    // the very press that should re-open a closed popup.
    function outsidePressed() {
        if (root.dismissOnOutsideClick && root.visible)
            root.visible = false
    }

    // While open, CLAIM the pointer (AppCursor.blocked): a PositionHoverArea
    // beneath the panel — a design card, a list row — must not light up as
    // though the pointer were on it. Hover is derived from pure geometry, so
    // without this an open menu washed the surface behind it (the card's
    // border lit and its action row appeared while the pointer was merely on
    // the menu). The panel's OWN rows are descendants and stay exempt, so the
    // menu keeps its row hover. Emits blockersChanged so the surfaces it
    // covers settle at once, even under a stationary pointer.
    onVisibleChanged: {
        if (root.visible)
            AppCursor.pushBlocker(root)
        else
            AppCursor.popBlocker(root)
    }
    Component.onDestruction: AppCursor.popBlocker(root)

    // Scroll the item list by `step` px (clamped) — the catcher calls this
    // for wheels over the anchor control, so a wheel on the open combobox's
    // box scrolls the MENU's options, never the dialog/page behind it.
    // Sign convention (both callers): pass the RAW wheel delta per notch
    // (angleDelta.y / 3, pixelDelta.y when the device gives pixels) —
    // scrolling DOWN arrives negative and must move contentY toward the
    // bottom, same as the standard `contentY -= angleDelta` idiom. The old
    // extra negation here inverted every dropdown's scroll direction.
    function scrollList(step) {
        if (!itemFlick || itemFlick.contentHeight <= itemFlick.height)
            return
        const maxY = itemFlick.contentHeight - itemFlick.height
        itemFlick.contentY = Math.max(0, Math.min(maxY, itemFlick.contentY - step))
    }

    // Called by a FLYOUT's parent panel when the parent closes: a submenu
    // must never outlive the menu that spawned it. (A binding on `visible`
    // does the same thing; this is the imperative twin for hosts that show
    // the flyout by a method call.)
    function closeFlyout() {
        if (root.visible)
            root.visible = false
    }
    // Hover in/out of a non-divider row — `rowItem` is the row's own
    // Rectangle, in THIS panel's coordinate space, so a consumer that wants
    // to open a flyout submenu next to it can map from a known-good item
    // instead of re-deriving the row's position from the model index.
    // `index` is the row's model position (the autocomplete consumers relay it
    // so the owner can find the row's payload; menu consumers ignore it).
    signal itemHovered(string label, bool hovering, var rowItem, int index)

    // Opens the panel at (x, y) — coordinates in `sourceItem`'s local space —
    // clamped to stay fully inside `bounds` (any common ancestor, usually
    // the window root), so a menu opened near an edge can't land partly or
    // fully off-screen.
    //
    // POSITIONING SPACE: the point is mapped into THIS PANEL'S PARENT's
    // coordinate space (where root.x/root.y actually apply), NOT into
    // `bounds` space. An earlier version mapped into bounds-space and
    // assigned the result directly — correct only when the panel's parent
    // sat at the window origin, and silently offset down-right by the
    // parent's position otherwise (the AV board's context menu landed far
    // from the cursor; the menu sits inside the settings modal there).
    // Mapping to root.parent + converting the clamp rect with mapFromItem
    // is correct for ANY parent, so no call site needs to know where its
    // menu is declared. One implementation of the map + clamp dance
    // instead of a copy per menu site.
    // Remembered anchor so the open menu FOLLOWS its field: scrolling the
    // page moves the box, and the menu tracks it (native combobox behavior)
    // instead of floating detached — or being dismissed outright.
    property Item anchorItem: null
    property real anchorLocalX: 0
    property real anchorLocalY: 0

    function openAt(sourceItem, x, y, bounds) {
        // Window-root reparenting: the panel lifts OUT of its declaration
        // context (a clipped, scrolling dialog container) to the window's
        // contentItem. Parented in-place it was clipped by ancestor `clip`
        // rects and scrolled with the dialog's Flickable — panels read as
        // transparent/overdrawn garbage and fought the dialog's own
        // scroller. At the root, the panel floats above everything (z is
        // relative to window-level siblings) and nothing clips it.
        root.isFlyout = false
        root.anchorItem = sourceItem
        root.anchorLocalX = x
        root.anchorLocalY = y
        root.openBounds = bounds ? bounds : sourceItem.Window.contentItem
        const contentItem = sourceItem.Window.contentItem
        if (contentItem && root.parent !== contentItem)
            root.parent = contentItem
        root.z = 40000
        _reposition()
        root.visible = true
        // The measurement above sizes the panel, but the FIRST pass reads the
        // width/height it had BEFORE that sizing — so the clamp math (and a
        // flip-up's y) is one layout late. Re-place once the panel has laid
        // out, exactly as the old call site used to do by hand.
        Qt.callLater(function () { if (root.visible) root._reposition() })
    }

    // Opens this panel as a FLYOUT beside `rowItem` — a row of another,
    // already-open panel that owns this list (the design library's
    // "Move to …" row). It sits at the row's right edge, or to its LEFT when
    // the bounds have no room there, and fits/scrols exactly like openAt. It
    // is a submenu, not an independent menu: the host binds its `visible` to
    // the parent panel's, so the pair always comes and goes together.
    function openBeside(rowItem, bounds) {
        root.isFlyout = true
        root.anchorItem = rowItem
        root.anchorLocalX = 0     // the ROW's own top-left, not a click point
        root.anchorLocalY = 0
        root.openBounds = bounds ? bounds : rowItem.Window.contentItem
        const contentItem = rowItem.Window.contentItem
        if (contentItem && root.parent !== contentItem)
            root.parent = contentItem
        root.z = 40000
        _reposition()
        root.visible = true
        Qt.callLater(function () { if (root.visible) root._reposition() })
    }

    // Sizes the panel to the room it has and places it at its anchor —
    // called when it opens and again whenever the anchor moves or the window
    // resizes (see the resize Connections below). A panel that doesn't fit
    // below its anchor SCROLLS (the cap turns the list into a Flickable) and
    // flips ABOVE the anchor when that side has more room, instead of being
    // clamped into a position where part of it hangs off the window — the
    // "dropdown isn't window responsive" bug.
    function _reposition() {
        const item = root.anchorItem
        if (!item || !root.parent)
            return
        const pad = root.edgePad
        const pt = item.mapToItem(root.parent, root.anchorLocalX, root.anchorLocalY)
        let x = pt.x
        let y = pt.y
        const b = root.openBounds

        if (!b) {
            root.roomCap = 0
            root.widthCap = 0
            root.flipUp = false
            root.x = x
            root.y = y
            return
        }

        const bp = root.parent.mapFromItem(b, 0, 0)
        const innerTop = bp.y + pad
        const innerBottom = bp.y + b.height - pad
        const innerLeft = bp.x + pad
        const innerRight = bp.x + b.width - pad
        const roomBelow = innerBottom - pt.y
        const roomAbove = pt.y - innerTop
        // The room the panel may draw in: the roomier side of the anchor. A
        // menu (a click point for an anchor) flips ABOVE when there is no
        // usable room below; a flyout (a ROW for an anchor) keeps its top at
        // the row and shifts up instead, so it still reads as that row's
        // submenu rather than detaching from it.
        if (root.isFlyout) {
            root.flipUp = false
            root.roomCap = Math.floor(Math.max(roomBelow, roomAbove))
        } else if (roomBelow < 240 && roomAbove > roomBelow) {
            root.flipUp = true
            root.roomCap = Math.floor(roomAbove)
        } else {
            root.flipUp = false
            root.roomCap = Math.floor(roomBelow)
        }
        // Never taller/wider than the bounds itself: a window shorter than
        // the menu's content is exactly the case this exists for.
        root.roomCap = Math.max(0, Math.min(root.roomCap, Math.floor(b.height - pad * 2)))
        root.widthCap = Math.max(0, Math.floor(b.width - pad * 2))

        // Horizontal: a flyout prefers the row's right edge and flips to its
        // left when the bounds have no room there.
        if (root.isFlyout) {
            const rowRight = item.mapToItem(root.parent, item.width + 4, 0).x
            const rowLeft = item.mapToItem(root.parent, 0, 0).x
            x = (rowRight + root.width <= innerRight) ? rowRight : rowLeft - root.width - 4
        }
        x = Math.max(innerLeft, Math.min(x, innerRight - root.width))

        // Vertical.
        if (root.isFlyout)
            y = Math.min(pt.y, innerBottom - root.height)
        else if (root.flipUp)
            y = pt.y - root.height
        y = Math.max(innerTop, Math.min(y, innerBottom - root.height))

        root.x = x
        root.y = y
    }

    // Coalesced follow: when the anchor (or an ancestor scroller) moves,
    // re-derive the window-space anchor point and re-place, clamped as at
    // open. Throttled to the next frame — one wheel batch can move the
    // anchor many times.
    Timer {
        id: anchorFollow
        interval: 0
        repeat: false
        onTriggered: {
            if (!root.visible || !root.anchorItem) return
            root._reposition()
        }
    }

    Connections {
        enabled: root.visible && root.anchorItem !== null
        target: root.anchorItem
        function onXChanged() { anchorFollow.restart() }
        function onYChanged() { anchorFollow.restart() }
    }

    // The window resized under an open menu: re-measure and re-place, so a
    // menu that was inside the old window can't be left hanging over (or
    // past) the new edge — no menu open, no cost.
    Connections {
        enabled: root.visible
        target: root.Window.contentItem
        function onWidthChanged() { anchorFollow.restart() }
        function onHeightChanged() { anchorFollow.restart() }
    }

    // Matches Theme.space4 — see x/y note below.
    readonly property int insetPad: 16

    // Content-fit width (opt-in): the panel hugs its widest row — how a
    // native menu sizes itself — instead of the fixed 480 slab the menu
    // bar's dropdowns used to render as (a "Save  Ctrl+S" row floating in
    // 480 px of mostly empty panel). Off by default, so every existing
    // consumer keeps 480; the AppMenuBar menus turn it on.
    property bool fitContentWidth: false
    // Floor for a fitted panel — narrower than this a menu reads as a chip.
    property real minFitWidth: 140

    // Invisible measurement twins. The real rows ELIDE to the panel's width,
    // so the needed width cannot be read off them; these render the same
    // strings in the same fonts unelided, and fittedWidth reads their
    // implicitWidths. A model rebuild recreates the Repeater's delegates
    // (its children list is a binding dependency), so a menu whose contents
    // change re-fits without anyone remembering to ask.
    Column {
        id: sizerCol
        visible: false
        width: 0; height: 0
        Repeater {
            model: root.model
            Item {
                required property var modelData
                readonly property bool isDivider: modelData === undefined || modelData === null
                                                  || modelData.divider === true
                readonly property bool hasTrailing: !isDivider && modelData.trailing !== undefined
                                                    && modelData.trailing !== ""
                // What the row needs to show unelided: 16 left pad + label,
                // then either 16 right pad (no trailing) or the trailing
                // text + its own right pad + the row's fixed label↔trailing
                // gap (8 + 26 = 34 from itemC's width formula, +4 slack).
                readonly property real sizerWidth: isDivider ? 0 : Math.ceil(
                    labelSizer.implicitWidth
                    + (hasTrailing ? trailingSizer.implicitWidth + 54 : 32))
                Text {
                    id: labelSizer
                    visible: false
                    font.family: "Segoe UI" // Theme.fontFamily
                    font.pixelSize: 15 // Theme.textMd
                    font.weight: Font.Medium
                    text: parent.isDivider || parent.modelData.label === undefined
                          ? "" : parent.modelData.label
                }
                Text {
                    id: trailingSizer
                    visible: false
                    font.family: "Segoe UI" // Theme.fontFamily
                    font.pixelSize: 10 // Theme.textXs
                    text: parent.hasTrailing ? parent.modelData.trailing : ""
                }
            }
        }
    }
    // Header twins — a fitted panel must fit the title/version lines too.
    Text {
        id: headerTitleSizer
        visible: false
        font.family: "Segoe UI" // Theme.fontFamily
        font.pixelSize: 15 // Theme.textMd
        font.weight: Font.Bold
        text: root.headerTitle
    }
    Text {
        id: headerSubSizer
        visible: false
        font.family: "Segoe UI" // Theme.fontFamily
        font.pixelSize: 10 // Theme.textXs
        text: root.headerSubtitle
    }
    readonly property real fittedWidth: {
        let w = root.minFitWidth
        const kids = sizerCol.children
        for (let i = 0; i < kids.length; ++i)
            if (kids[i].sizerWidth !== undefined)
                w = Math.max(w, kids[i].sizerWidth)
        if (root.headerTitle !== "")
            w = Math.max(w, 16 + headerTitleSizer.implicitWidth + 16,
                            16 + headerSubSizer.implicitWidth + 16)
        return Math.ceil(w)
    }
    // The width the contents want, before the window's own limit is applied.
    readonly property real naturalWidth: fitContentWidth ? fittedWidth : 480
    width: root.widthCap > 0 ? Math.min(root.naturalWidth, root.widthCap) : root.naturalWidth
    // What the panel wants to be, and the cap actually in force: the room
    // `openAt`/`openBeside` measured and the caller's own `maxHeight`,
    // whichever is smaller. A panel that was never measured (the menu bar's
    // declaratively-positioned menus) falls back to the window itself, so no
    // menu can ever be taller than the window it opens in.
    readonly property real contentHeight: headerCol.height + itemList.height + 16
    readonly property int effMaxHeight: {
        let cap = root.roomCap
        if (root.maxHeight > 0)
            cap = cap > 0 ? Math.min(cap, root.maxHeight) : root.maxHeight
        if (cap <= 0) {
            const w = root.Window.contentItem
            cap = w ? Math.floor(w.height - root.edgePad * 2) : 0
        }
        return cap
    }
    height: root.effMaxHeight > 0 ? Math.min(root.contentHeight, root.effMaxHeight)
                                  : root.contentHeight
    // The viewport the item list scrolls within — everything inside the
    // panel that isn't the 8px top/bottom padding or the header. Floored at
    // 0: a window so short that the header alone fills the cap must not
    // produce a negative Flickable height.
    readonly property real listViewport: Math.max(0, height - 16 - headerCol.height)
    radius: 10 // Theme.radiusLg
    color: Theme.rowBg
    border.color: Theme.border
    border.width: 1
    clip: true

    Column {
        id: content
        y: 8
        width: parent.width

        Column {
            id: headerCol
            visible: root.headerTitle !== ""
            width: parent.width
            spacing: 2

            Text {
                x: root.insetPad
                topPadding: 4 // Theme.space1
                text: root.headerTitle
                color: Theme.accent
                font.family: "Segoe UI" // Theme.fontFamily
                font.pixelSize: 15 // Theme.textMd
                font.weight: Font.Bold
            }
            Text {
                x: root.insetPad
                bottomPadding: 8 // Theme.space2
                text: root.headerSubtitle
                color: Theme.textMuted
                font.family: "Segoe UI" // Theme.fontFamily
                font.pixelSize: 10 // Theme.textXs
            }
            Rectangle { width: parent.width; height: 1; color: Theme.border }
        }

        // The item list — a Flickable whenever maxHeight caps it, so the cap
        // scrolls instead of clipping. interactive only when it actually
        // overflows, so short lists keep their native click feel.
        Flickable {
            id: itemFlick
            width: content.width
            height: root.effMaxHeight > 0 ? root.listViewport : itemList.height
            contentWidth: width
            contentHeight: itemList.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: root.effMaxHeight > 0 && contentHeight > height

            // Contained wheels: wheels over the list scroll THE LIST —
            // anything the list can't take (fits without scrolling, or at
            // its end) is absorbed here too, never forwarded to the
            // dialog/page behind the popup. (The catcher keeps wheels that
            // miss the menu from reaching the page; it must not inherit
            // these.)
            WheelHandler {
                enabled: root.visible
                onWheel: (ev) => {
                    ev.accepted = true
                    // Raw delta — wheel-down is negative and scrolls the
                    // list downward (see scrollList's sign convention).
                    root.scrollList(ev.angleDelta.y / 3)
                }
            }

            Column {
                id: itemList
                width: parent.width

                Repeater {
                    model: root.model
                    delegate: Loader {
                        required property var modelData
                        required property int index
                        width: itemList.width
                        // Stale-row guard: the model rebuilds live (the
                        // autocomplete box repopulates on every keystroke),
                        // and a mouse release can land on a row whose model
                        // entry is already gone — bare derefs threw
                        // "Cannot read property 'payload' of undefined" live.
                        // A detached row falls through to itemC and its own
                        // alive guard renders/swallows it.
                        sourceComponent: modelData && modelData.divider === true ? dividerC : itemC

                        Component {
                            id: dividerC
                            Item {
                                width: itemList.width
                                height: 12 // Theme.space3
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width - 32 // Theme.space4 * 2
                                    height: 1
                                    color: Theme.border
                                }
                            }
                        }

                        Component {
                            id: itemC
                            Rectangle {
                                id: itemRow
                                width: itemList.width
                                height: 34
                                // STALE-ROW GUARD — the click fix: a model
                                // rebuild (autocomplete retyping, menu
                                // re-opening) can leave a delegate clickable
                                // for a beat after its data is gone, and the
                                // click used to throw live ("Cannot read
                                // property 'payload' of undefined", or
                                // "root is not defined" when the creation
                                // context was already torn down). A detached
                                // row must render blank and swallow its
                                // click. typeof (not truthiness) is the one
                                // reference that cannot itself throw.
                                readonly property bool alive: typeof modelData !== "undefined"
                                                              && modelData !== null
                                // Disabled entries (e.g. capture modes a
                                // device's max fps can't reach) sit greyed
                                // with no hover wash and swallow activation.
                                // Autocomplete mode draws the keyboard highlight (the row
                                // Up/Down will accept) under the mouse hover, same wash.
                                color: alive && !modelData.disabled && (itemArea.hovered || root.highlightedIndex === index)
                                       ? (modelData.danger ? Qt.alpha(Theme.danger, 0.14)
                                                           : Theme.border)
                                       : "transparent"
                                // Slightly quicker fade than the header pills
                                // (180 ms there): the cursor sweeps rows fast,
                                // and a longer wash here smears the trail.
                                Behavior on color {
                                    ColorAnimation { duration: 150; easing.type: Easing.OutQuad }
                                }

                                Text {
                                    id: rowTrailing
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16 // Theme.space4
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: alive ? modelData.trailing || "" : ""
                                    color: Theme.textMuted
                                    font.family: "Segoe UI" // Theme.fontFamily
                                    font.pixelSize: 10 // Theme.textXs
                                }

                                Text {
                                    x: root.insetPad
                                    anchors.verticalCenter: parent.verticalCenter
                                    // Autocomplete labels elide, but only when they genuinely
                                    // don't fit: the label reserves the ACTUAL trailing-code
                                    // width ("51-0501"), not a flat 40% of the row — the flat
                                    // reserve truncated short titles long before the row edge
                                    // (user call: the dropdown must show the full name).
                                    width: alive ? parent.width - root.insetPad
                                            - (modelData.trailing ? rowTrailing.implicitWidth + 26 : 0) - 8
                                                 : parent.width
                                    elide: Text.ElideRight
                                    text: alive ? modelData.label : ""
                                    color: alive ? (modelData.disabled ? Theme.textMuted
                                        : modelData.danger ? Theme.dangerLight : Theme.textPrimary)
                                        : Theme.textMuted
                                    font.family: "Segoe UI" // Theme.fontFamily
                                    font.pixelSize: 15 // Theme.textMd
                                    font.weight: Font.Medium
                                }

                                // Position truth, not containsMouse — hover-exit
                                // never delivers to MouseAreas in this build
                                // (KNOWN_ISSUES.md), which is exactly why these
                                // rows' hover effects felt dead. shownChain
                                // (inside PositionHoverArea) is a binding over
                                // every ancestor's visible/enabled, so a panel
                                // closing also clears its rows' hover.
                                PositionHoverArea {
                                    id: itemArea
                                    anchors.fill: parent
                                    onClicked: {
                                        // THE fix for the reported TypeError:
                                        // itemActivated's consumer may rebuild
                                        // root.model SYNCHRONOUSLY (retagging,
                                        // switching content) — the Repeater then
                                        // rebinds this delegate's modelData to
                                        // undefined while this handler is still
                                        // on the stack, and the old code read
                                        // modelData.payload AFTER that ("Cannot
                                        // read property 'payload' of undefined";
                                        // a consumer that closed the panel
                                        // entirely tore the context down instead
                                        // — "root is not defined"). Capture the
                                        // row's data FIRST (the user picked THIS
                                        // row — itemPicked must carry it even if
                                        // the model just changed under us), and
                                        // typeof-guard both lookups (the one
                                        // reference that cannot itself throw) so
                                        // a fully dead row does nothing.
                                        if (typeof modelData === "undefined" || !modelData || modelData.disabled
                                            || typeof root === "undefined")
                                            return
                                        const pickedLabel = modelData.label
                                        const pickedPayload = modelData.payload
                                        const pickedIndex = index
                                        root.itemActivated(pickedLabel)
                                        root.itemPicked(pickedIndex, pickedPayload)
                                    }
                                    // Same stale-row guards as the click: a
                                    // hover recompute can also fire on a row
                                    // whose model entry is gone.
                                    onEntered: if (typeof modelData !== "undefined" && modelData)
                                                   root.itemHovered(modelData.label, true, itemRow, index)
                                    onExited: if (typeof modelData !== "undefined" && modelData)
                                                  root.itemHovered(modelData.label, false, itemRow, index)
                                }
                            }
                        }
                    }
                }
            }
        }

        // Auto-hides when the list doesn't overflow (AppScrollBar's own
        // visible binding); sits in the panel's right padding strip.
        AppScrollBar {
            flickable: itemFlick
            x: root.width - width - 3
            y: content.y + headerCol.height
            height: itemFlick.height
            z: 1
        }
    }

    // The dismissal catcher — only in autocomplete mode (menus are closed by
    // the menu bar's own catcher; a second catcher there would eat the field's
    // toggle click).
    MouseArea {
        enabled: root.visible && root.dismissOnOutsideClick
        parent: root.parent
        anchors.fill: parent
        z: root.z - 1
        onPressed: (mouse) => { root.visible = false; mouse.accepted = false }
    }
}
