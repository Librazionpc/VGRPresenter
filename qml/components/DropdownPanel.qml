import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Rectangle {
    id: root

    property var model: []
    property string headerTitle: ""
    property string headerSubtitle: ""
    property int maxHeight: 0
    // Keep the list's scroll bar on screen whenever the list overflows (see
    // header comment). False restores AppScrollBar's own fade-out-on-idle.
    property bool scrollBarPinned: true

    // ---- measured open geometry (see _reposition) ---------------------------
    property Item openBounds: null
    property int roomCap: 0        // 0 = not measured
    property int widthCap: 0       // 0 = not measured
    property bool flipUp: false
    property bool isFlyout: false
    // Panel padding against the bounds' edges — a menu never touches the
    // window frame.
    readonly property int edgePad: 8

    scale: AppScale.cardScale(root)
    transformOrigin: Item.TopLeft

    signal itemActivated(string label, var item)

    property bool dismissOnOutsideClick: false
    property int highlightedIndex: -1
    signal itemPicked(int index, var payload)

    onVisibleChanged: {
        if (root.visible) {
            AppCursor.pushBlocker(root)
            itemFlick.contentY = 0
        } else {
            AppCursor.popBlocker(root)
        }
    }
    Component.onDestruction: AppCursor.popBlocker(root)

    function scrollList(step) {
        if (!itemFlick || itemFlick.contentHeight <= itemFlick.height)
            return
        const maxY = itemFlick.contentHeight - itemFlick.height
        itemFlick.contentY = Math.max(0, Math.min(maxY, itemFlick.contentY - step))
    }

    function closeFlyout() {
        if (root.visible)
            root.visible = false
    }
    signal itemHovered(string label, bool hovering, var rowItem, int index)

    property Item anchorItem: null
    property real anchorLocalX: 0
    property real anchorLocalY: 0

    // ---- which layer the panel pops into -----------------------------------
    function _layer(item) {
        const w = item ? item.Window : null
        if (!w)
            return null
        const ov = Overlay.overlay
        return (ov && ov.parent === w.contentItem) ? ov : w.contentItem
    }

    function openAt(sourceItem, x, y, bounds) {
        root.isFlyout = false
        root.anchorItem = sourceItem
        root.anchorLocalX = x
        root.anchorLocalY = y
        root.openBounds = bounds ? bounds : sourceItem.Window.contentItem
        const layer = root._layer(sourceItem)
        if (layer && root.parent !== layer)
            root.parent = layer
        root.z = 40000
        _reposition()
        root.visible = true
        Qt.callLater(function () { if (root.visible) root._reposition() })
    }

    function openBeside(rowItem, bounds) {
        root.isFlyout = true
        root.anchorItem = rowItem
        root.anchorLocalX = 0     // the ROW's own top-left, not a click point
        root.anchorLocalY = 0
        root.openBounds = bounds ? bounds : rowItem.Window.contentItem
        const layer = root._layer(rowItem)
        if (layer && root.parent !== layer)
            root.parent = layer
        root.z = 40000
        _reposition()
        root.visible = true
        Qt.callLater(function () { if (root.visible) root._reposition() })
    }

    function _reposition() {
        const item = root.anchorItem
        if (!item || !root.parent)
            return
        const pad = root.edgePad
        const pt = item.mapToItem(root.parent, root.anchorLocalX, root.anchorLocalY)
        let x = pt.x
        let y = pt.y
        const b = root.openBounds
        const s = root.scale > 0 ? root.scale : 1
        const drawnW = root.width * s
        const drawnH = root.height * s
        const capOf = (roomPx) => Math.floor(roomPx / s)

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
        if (root.isFlyout) {
            root.flipUp = false
            root.roomCap = capOf(Math.max(roomBelow, roomAbove))
        } else if (roomBelow < 240 && roomAbove > roomBelow) {
            root.flipUp = true
            root.roomCap = capOf(roomAbove)
        } else {
            root.flipUp = false
            root.roomCap = capOf(roomBelow)
        }
        // Never taller/wider than the bounds itself: a window shorter than
        // the menu's content is exactly the case this exists for.
        root.roomCap = Math.max(0, Math.min(root.roomCap, capOf(b.height - pad * 2)))
        root.widthCap = Math.max(0, capOf(b.width - pad * 2))

        // Horizontal: a flyout prefers the row's right edge and flips to its
        // left when the bounds have no room there.
        if (root.isFlyout) {
            const rowRight = item.mapToItem(root.parent, item.width + 4, 0).x
            const rowLeft = item.mapToItem(root.parent, 0, 0).x
            x = (rowRight + drawnW <= innerRight) ? rowRight : rowLeft - drawnW - 4
        }
        x = Math.max(innerLeft, Math.min(x, innerRight - drawnW))

        // Vertical.
        if (root.isFlyout)
            y = Math.min(pt.y, innerBottom - drawnH)
        else if (root.flipUp)
            y = pt.y - drawnH
        y = Math.max(innerTop, Math.min(y, innerBottom - drawnH))

        root.x = x
        root.y = y
    }

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

    Connections {
        enabled: root.visible
        target: root.Window.contentItem
        function onWidthChanged() { anchorFollow.restart() }
        function onHeightChanged() { anchorFollow.restart() }
    }

    // Matches Theme.space4 — see x/y note below.
    readonly property int insetPad: 16
    // Menu text: one size down from the old 15 — a menu row is a label, not a
    // headline, and 15px rows read as a big slab next to the 13px fields they drop from.
    readonly property int rowFont: Theme.textSm
    // The rows sit on a 6px gutter inside the panel as soft rounded pills.
    readonly property int rowGutter: 6

    property real rowInset: -1
    property real rowHeight: -1

    property bool fitContentWidth: false
    // Floor for a fitted panel — narrower than this a menu reads as a chip.
    property real minFitWidth: 140

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
                readonly property real sizerWidth: isDivider ? 0 : Math.ceil(
                    labelSizer.implicitWidth
                    + (hasTrailing ? trailingSizer.implicitWidth + 54 : 32))
                Text {
                    id: labelSizer
                    visible: false
                    font.family: Theme.fontFamily
                    font.pixelSize: root.rowFont
                    font.weight: Font.Medium
                    text: parent.isDivider || parent.modelData.label === undefined
                          ? "" : parent.modelData.label
                }
                Text {
                    id: trailingSizer
                    visible: false
                    font.family: Theme.fontFamily
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
        font.family: Theme.fontFamily
        font.pixelSize: 15 // Theme.textMd
        font.weight: Font.Bold
        text: root.headerTitle
    }
    Text {
        id: headerSubSizer
        visible: false
        font.family: Theme.fontFamily
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
    property bool matchAnchorWidth: false
    // The width the contents want, before the window's own limit is applied.
    readonly property real naturalWidth: {
        if (root.matchAnchorWidth && root.anchorItem && root.anchorItem.width > 0)
            return root.anchorItem.width
        return (root.fitContentWidth || root.autoFit) ? root.fittedWidth : 480
    }
    readonly property bool autoFit: {
        if (root.dismissOnOutsideClick)
            return false
        const m = root.model
        if (!m)
            return false
        for (let i = 0; i < m.length; ++i)
            if (m[i] && m[i].fontFamily)
                return false
        return true
    }
    width: root.widthCap > 0 ? Math.min(root.naturalWidth, root.widthCap) : root.naturalWidth
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
    readonly property real listViewport: Math.max(0, height - 16 - headerCol.height)
    radius: 12
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
                font.family: Theme.fontFamily
                font.pixelSize: 15 // Theme.textMd
                font.weight: Font.Bold
            }
            Text {
                x: root.insetPad
                bottomPadding: 8 // Theme.space2
                text: root.headerSubtitle
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10 // Theme.textXs
            }
            Rectangle { width: parent.width; height: 1; color: Theme.border }
        }

        Flickable {
            id: itemFlick
            width: content.width
            height: root.effMaxHeight > 0 ? root.listViewport : itemList.height
            contentWidth: width
            contentHeight: itemList.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: root.effMaxHeight > 0 && contentHeight > height

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
                        sourceComponent: modelData && modelData.divider === true ? dividerC : itemC

                        Component {
                            id: dividerC
                            Item {
                                width: itemList.width
                                height: 11
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width - root.rowGutter * 2 - 8
                                    height: 1
                                    color: Theme.borderSubtle
                                }
                            }
                        }

                        Component {
                            id: itemC
                            Rectangle {
                                id: itemRow
                                objectName: "selfTestDropdownRow_"
                                            + (typeof modelData !== "undefined" && modelData
                                               && modelData.label ? modelData.label : index)
                                width: itemList.width
                                height: root.rowHeight >= 0 ? root.rowHeight : 34
                                readonly property bool alive: typeof modelData !== "undefined"
                                                              && modelData !== null
                                color: "transparent"

                                Rectangle {
                                    id: pill
                                    x: root.rowGutter
                                    y: 1
                                    width: parent.width - root.rowGutter * 2
                                    height: parent.height - 2
                                    radius: Theme.radiusMd - 1
                                    color: alive && !modelData.disabled && root.highlightedIndex === index
                                           ? (modelData.danger ? Qt.alpha(Theme.danger, 0.14)
                                                               : Theme.border)
                                           : alive && modelData.accent === true
                                               ? Qt.alpha(Theme.accent, 0.14)
                                               : "transparent"
                                    Behavior on color {
                                        ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing }
                                    }

                                    HoverGlow {
                                        anchors.fill: parent
                                        radius: parent.radius
                                        hovered: alive && !modelData.disabled && itemArea.hovered
                                        glowColor: modelData.danger ? Qt.alpha(Theme.danger, 0.14) : Theme.border
                                    }
                                }

                                Text {
                                    id: rowTrailing
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16 // Theme.space4
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: alive ? modelData.trailing || "" : ""
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10 // Theme.textXs
                                }

                                Text {
                                    x: root.rowInset >= 0 ? root.rowInset : root.insetPad
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: alive ? parent.width - (root.rowInset >= 0 ? root.rowInset : root.insetPad)
                                            - (modelData.trailing ? rowTrailing.implicitWidth + 26 : 0) - 8
                                                 : parent.width
                                    elide: Text.ElideRight
                                    text: alive ? modelData.label : ""
                                    color: alive ? (modelData.disabled ? Theme.textMuted
                                        : modelData.danger ? Theme.dangerLight
                                        : modelData.accent === true ? Theme.accent : Theme.textPrimary)
                                        : Theme.textMuted
                                    font.family: alive && modelData.fontFamily ? modelData.fontFamily
                                                                              : Theme.fontFamily
                                    font.pixelSize: root.rowFont
                                    font.weight: modelData && modelData.accent === true ? Font.DemiBold : Font.Medium
                                }

                                PositionHoverArea {
                                    id: itemArea
                                    anchors.fill: parent
                                    onClicked: {
                                        if (typeof modelData === "undefined" || !modelData || modelData.disabled
                                            || typeof root === "undefined")
                                            return
                                        const pickedLabel = modelData.label
                                        const pickedPayload = modelData.payload
                                        const pickedRow = modelData
                                        const pickedIndex = index
                                        if (typeof pickedPayload !== "undefined")
                                            root.itemPicked(pickedIndex, pickedPayload)
                                        else
                                            root.itemActivated(pickedLabel, pickedRow)
                                    }
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

        AppScrollBar {
            flickable: itemFlick
            autoHide: !root.scrollBarPinned
            trackWidth: 6
            // No track: on a menu the ground behind the thumb is already the
            // list, and a second surface band down the edge read as a divider.
            trackColor: "transparent"
            thumbColor: Theme.dark ? "#4d5363" : "#b9bfcc"
            x: root.width - width - 4
            y: content.y + headerCol.height + 4
            height: Math.max(0, itemFlick.height - 8)
            z: 1
        }
    }

    MouseArea {
        enabled: root.visible && root.dismissOnOutsideClick
        parent: root.parent
        anchors.fill: parent
        z: root.z - 1
        onPressed: (mouse) => { root.visible = false; mouse.accepted = false }
    }
}