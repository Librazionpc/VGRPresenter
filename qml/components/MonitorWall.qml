import QtQuick
import VGRPresenterUI

// The output monitor wall — ONE component, TWO hosts: the Show screen's
// right column and the Edit screen's ITEMS tab. Both used to host the
// shared OutputMonitorTile themselves, but the HOSTING diverged: the Show
// side grew hero/paged/swipe behavior (lone output = full-width hero,
// 2×2 pages with snap + dots) while the Edit side kept a plain two-column
// Grid — so "the same tiles" rendered as visibly different monitors per
// surface. The wall logic now lives here once; hosts differ only in
// position/size.
//
// Geometry contract (kept byte-compatible with the old Show-side wall so
// downstream offsets didn't move): `height` is the WALL's height (the page
// dots, when visible, draw below it OUTSIDE bounds — hosts that space
// content under the wall keep their original +22/+11 style constants).
// Hosts set x/y/width; the component does the rest from OutputListModel.
Item {
    id: root

    // Roster-change revision: rowCount() isn't notifyable from QML, so bump
    // a counter on roster changes — every tile's width binding reads it and
    // re-evaluates when outputs come and go (same pattern as ScreenForm's
    // displayRev).
    property int _wallRev: 0
    Connections {
        target: OutputListModel
        function onRowsInserted() { root._wallRev++ }
        function onRowsRemoved() { root._wallRev++ }
    }

    // External contract: hosts position content under the wall and drive
    // the page dots from these.
    readonly property alias pageCount: wall.pageCount
    readonly property alias currentPage: wall.currentPage
    function goTo(page) { wall.goTo(page) }

    height: wall.height

    // Paged wall — max 4 tiles (2×2) per page; output 5+ lands on the next
    // page and the wall SWIPES to it (snap-one-item Flickable), with dot
    // indicators below. One output still gets the full-width hero tile;
    // 2–4 share a 2×2 page. Slots are computed positions (page =
    // index/capacity) so a single Repeater feeds every page.
    Flickable {
        id: wall

        anchors.horizontalCenter: parent.horizontalCenter
        y: 0

        readonly property int count: root._wallRev >= 0 ? OutputListModel.rowCount() : 0
        // Page capacity: a lone output gets a hero page of its own.
        readonly property int perPage: count === 1 ? 1 : 4
        readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
        readonly property real pageW: count === 1 ? 376 : 376
        readonly property real slotW: count === 1 ? 376 : 182
        readonly property real slotH: count === 1
                                     ? 6 + (slotW - 12) * 9 / 16 + 6 + 16 + 6
                                     : 143

        width: 376
        height: pageCount === 1 ? slotH
                                : 2 * slotH + 11
        contentWidth: pageCount * pageW
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 2000
        interactive: pageCount > 1

        property int currentPage: 0
        // Page snap (Flickable has no snapMode): when a drag or a flick
        // ends, settle on the nearest page with a short ease. contentX
        // writes from these handlers are programmatic, not user drags, so
        // no fighting occurs.
        function snapToPage() {
            const target = Math.max(0, Math.min(pageCount - 1, Math.round(contentX / pageW)))
            currentPage = target
            snapAnim.to = target * pageW
            snapAnim.restart()
        }
        onDragEnded: snapToPage()
        onFlickEnded: snapToPage()

        NumberAnimation {
            id: snapAnim
            target: wall
            property: "contentX"
            duration: 180
            easing.type: Easing.OutCubic
        }

        function goTo(page) {
            currentPage = Math.max(0, Math.min(pageCount - 1, page))
            snapAnim.to = currentPage * pageW
            snapAnim.restart()
        }

        Repeater {
            model: OutputListModel

            delegate: OutputMonitorTile {
                required property int index

                x: {
                    const perPage = wall.perPage
                    const page = wall.count === 1 ? 0 : Math.floor(index / perPage)
                    const slot = wall.count === 1 ? 0 : index % perPage
                    return page * wall.pageW + (slot % 2) * (wall.slotW + 12)
                }
                y: {
                    const perPage = wall.perPage
                    const slot = wall.count === 1 ? 0 : index % perPage
                    return Math.floor(slot / 2) * (wall.slotH + 11)
                }
                width: wall.slotW
            }
        }
    }

    // Page dots — one per page, current page lit; click to swipe. Drawn
    // BELOW wall.height (outside bounds; root doesn't clip) so host
    // spacing constants under the wall keep their original values.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.height + 6
        visible: wall.pageCount > 1
        spacing: 6

        Repeater {
            model: wall.pageCount

            delegate: Rectangle {
                required property int index

                width: 6; height: 6; radius: 3
                color: index === wall.currentPage ? "#6c5ce7" : "#2b2e3d"
                Behavior on color { ColorAnimation { duration: 120 } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: wall.goTo(parent.index)
                }
            }
        }
    }
}
