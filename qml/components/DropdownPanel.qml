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
// Every color/spacing/font value below is a literal, not a Theme.* reference:
// this file is instantiated from AppMenuBar.qml, itself nested two documents
// deep (Main -> VGRPresenterMainScreen -> AppMenuBar -> DropdownPanel). At
// that depth Qt 6.11.1's AOT compiler cannot resolve the Theme singleton at
// all — every property bound to it here logged "Unable to assign [undefined]"
// and rendered wrong permanently (confirmed, not just a transient warning).
// Each literal below is commented with the Theme token it must stay in sync
// with if that token's value ever changes.
Rectangle {
    id: root

    property var model: []
    property string headerTitle: ""
    property string headerSubtitle: ""
    // 0 = size to content; a positive value caps the height and turns the
    // item list into a scrollable Flickable (see header comment).
    property int maxHeight: 0

    signal itemActivated(string label)

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
    // Hover in/out of a non-divider row — `rowItem` is the row's own
    // Rectangle, in THIS panel's coordinate space, so a consumer that wants
    // to open a flyout submenu next to it can map from a known-good item
    // instead of re-deriving the row's position from the model index.
    signal itemHovered(string label, bool hovering, var rowItem)

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
        root.anchorItem = sourceItem
        root.anchorLocalX = x
        root.anchorLocalY = y
        const contentItem = sourceItem.Window.contentItem
        if (contentItem && root.parent !== contentItem)
            root.parent = contentItem
        root.z = 10000
        _placeAt(sourceItem.mapToItem(root.parent, x, y),
                 bounds ? bounds : root.Window.contentItem)
        root.visible = true
    }

    function _placeAt(parentPos, bounds) {
        if (bounds && root.parent) {
            const bp = root.parent.mapFromItem(bounds, 0, 0)
            root.x = Math.max(bp.x, Math.min(parentPos.x, bp.x + bounds.width - root.width))
            root.y = Math.max(bp.y, Math.min(parentPos.y, bp.y + bounds.height - root.height))
        } else {
            root.x = parentPos.x
            root.y = parentPos.y
        }
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
            const p = root.anchorItem.mapToItem(root.parent, root.anchorLocalX, root.anchorLocalY)
            root._placeAt(p, root.Window.contentItem)
        }
    }

    Connections {
        enabled: root.visible && root.anchorItem !== null
        target: root.anchorItem
        function onXChanged() { anchorFollow.restart() }
        function onYChanged() { anchorFollow.restart() }
    }

    // Matches Theme.space4 — see x/y note below.
    readonly property int insetPad: 16

    width: 240
    height: root.maxHeight > 0
            ? Math.min(headerCol.height + itemList.height + 16, root.maxHeight)
            : headerCol.height + itemList.height + 16
    // The viewport the item list scrolls within — everything inside the
    // panel that isn't the 8px top/bottom padding or the header.
    readonly property real listViewport: height - 16 - headerCol.height
    radius: 10 // Theme.radiusLg
    color: "#16171e" // Theme.rowBg
    border.color: "#232530" // Theme.border
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
                color: "#6C5CE7" // Theme.accent
                font.family: "Inter" // Theme.fontFamily
                font.pixelSize: 13 // Theme.textMd
                font.weight: Font.Bold
            }
            Text {
                x: root.insetPad
                bottomPadding: 8 // Theme.space2
                text: root.headerSubtitle
                color: "#5c6475" // Theme.textMuted
                font.family: "Inter" // Theme.fontFamily
                font.pixelSize: 9 // Theme.textXs
            }
            Rectangle { width: parent.width; height: 1; color: "#232530" /* Theme.border */ }
        }

        // The item list — a Flickable whenever maxHeight caps it, so the cap
        // scrolls instead of clipping. interactive only when it actually
        // overflows, so short lists keep their native click feel.
        Flickable {
            id: itemFlick
            width: content.width
            height: root.maxHeight > 0 ? root.listViewport : itemList.height
            contentWidth: width
            contentHeight: itemList.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: root.maxHeight > 0 && contentHeight > height

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
                        width: itemList.width
                        sourceComponent: modelData.divider === true ? dividerC : itemC

                        Component {
                            id: dividerC
                            Item {
                                width: itemList.width
                                height: 12 // Theme.space3
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width - 32 // Theme.space4 * 2
                                    height: 1
                                    color: "#232530" // Theme.border
                                }
                            }
                        }

                        Component {
                            id: itemC
                            Rectangle {
                                id: itemRow
                                width: itemList.width
                                height: 34
                                // Disabled entries (e.g. capture modes a
                                // device's max fps can't reach) sit greyed
                                // with no hover wash and swallow activation.
                                color: !modelData.disabled && itemArea.hovered
                                       ? (modelData.danger ? "#24ff4d3d" /* Theme.danger @ 14% */
                                                           : "#232530" /* Theme.border */)
                                       : "transparent"
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    x: root.insetPad
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.label
                                    color: modelData.disabled ? "#5c6475" /* Theme.textMuted */
                                        : modelData.danger ? "#ff6b61" /* Theme.dangerLight */ : "#e2e8f0" /* Theme.textPrimary */
                                    font.family: "Inter" // Theme.fontFamily
                                    font.pixelSize: 13 // Theme.textMd
                                    font.weight: Font.Medium
                                }

                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16 // Theme.space4
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.trailing || ""
                                    color: "#5c6475" // Theme.textMuted
                                    font.family: "Inter" // Theme.fontFamily
                                    font.pixelSize: 9 // Theme.textXs
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
                                    onClicked: if (!modelData.disabled) root.itemActivated(modelData.label)
                                    onEntered: root.itemHovered(modelData.label, true, itemRow)
                                    onExited: root.itemHovered(modelData.label, false, itemRow)
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
}
