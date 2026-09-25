import QtQuick
import VGRPresenterUI

// The output monitor wall — ONE component, TWO hosts (Show screen right
// column, Edit screen ITEMS tab). FreeShow's preview chrome, adapted:
//
//   [• Main Output] [• Stage]        <- output tabs (click = that output
//   [  GO LIVE  ]                     becomes the on-air one)
//   [ live frame | live frame ]      <- REAL frames from the engine preview
//   [ < >  ▶  ⌫ Clear ]              <- transport: prev/next slide, clear
//
// The old tile's fake "LIVE 1" badge and play glyph are GONE — a tile either
// shows the actual distributed frame (its output is active) or the
// checkerboard empty state. Geometry contract kept: `height` is the WALL's
// height; hosts keep their +22/+11 spacing constants.
Item {
    id: root

    // Roster-change revision: rowCount() isn't notifyable from QML.
    property int _wallRev: 0
    Connections {
        target: OutputListModel
        function onRowsInserted() { root._wallRev++ }
        function onRowsRemoved() { root._wallRev++ }
        function onDataChanged() { root._wallRev++ }
    }

    // Frame cadence: the preview provider's cache-buster — bumped by the live
    // service's frameRev while live, by a slow idle timer otherwise (cheap; a
    // static image request is skipped entirely while the frame hash is null).
    readonly property int frameRev: LiveOutputService.frameRev
    readonly property bool live: LiveOutputService.live

    // External contract: hosts position content under the wall and drive
    // the page dots from these.
    readonly property alias pageCount: wall.pageCount
    readonly property alias currentPage: wall.currentPage
    function goTo(page) { wall.goTo(page) }

    height: tabs.height + wall.height + (toolbar.visible ? toolbar.height : 0)

    // ---- Output tabs (FreeShow's PreviewOutputs tab strip) -----------------
    // GO LIVE lives in the app header now (user call) — the wall keeps only
    // the per-output tabs.
    Row {
        id: tabs
        x: 0; y: 0
        width: parent.width
        height: 26
        spacing: 4

        Repeater {
            model: root._wallRev >= 0 ? OutputListModel.rowCount() : 0

            delegate: Rectangle {
                id: tab
                required property int index
                readonly property var out: OutputListModel.getOutput(index)
                readonly property bool isCurrent: OutputListModel.activeIndex() === index

                width: Math.max(64, (tabs.width - (OutputListModel.rowCount() - 1) * 4) / OutputListModel.rowCount())
                height: parent.height
                radius: 5
                color: isCurrent ? "#1e1f28" : (tabArea.containsMouse ? "#181922" : "#14151d")
                border.color: isCurrent ? "#34384a" : "#232530"
                border.width: 1

                Row {
                    anchors.centerIn: parent
                    spacing: 6

                    // State dot: green = enabled, grey = disabled (FreeShow's
                    // indicator; red is reserved for the LIVE border on tiles).
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 7; radius: 3.5
                        color: tab.out.isEnabled ? "#6dff85" : "#5a5f72"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, tab.width - 30)
                        text: tab.out.name
                        color: tab.isCurrent ? "#e2e8f0" : "#9aa0b5"
                        elide: Text.ElideRight
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                        font.bold: tab.isCurrent
                    }
                }

                // Disabled screens can't take air — the click is inert.
                MouseArea {
                    id: tabArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: tab.out.isEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: if (tab.out.isEnabled) OutputListModel.setActive(tab.index)
                }
            }
        }
    }

    // ---- Paged tile wall -----------------------------------------------------
    Flickable {
        id: wall

        anchors.horizontalCenter: parent.horizontalCenter
        y: tabs.height + 6

        readonly property int count: root._wallRev >= 0 ? OutputListModel.rowCount() : 0
        // Page capacity: a lone output gets a hero page of its own.
        readonly property int perPage: count === 1 ? 1 : 4
        readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
        readonly property real pageW: 376
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
    // BELOW wall.height (outside bounds; root doesn't clip).
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + wall.height + 6
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

    // ---- Preview chrome (FreeShow's ShowActions + ClearButtons, rebuilt) ----
    // Two sections under the preview, like FreeShow's Preview.svelte:
    //   [ < > ▶ 🔒 ◐ ]   ShowActions — previous/next slide, play (go live /
    //                     resume), the output LOCK, and the (informational)
    //                     transition glyph
    //   [ ✕ Clear all ]   ClearButtons' red full-width clear.all — everything
    //                     off air
    //   [🖼 📄 ⊙ 🎵 ⏱]   ClearButtons' group — clear background / slide /
    //                     overlays / audio / timers, red-tinted while there
    //                     is something of that layer on air to clear, dimmed
    //                     when that layer is already empty
    // The engine exposes stop() (all layers) and prev/next per slide; the
    // per-layer buttons today do what the engine can honestly do — they
    // light by whether there IS anything on air at all, and Clear all does
    // the real work.
    Rectangle {
        id: toolbar
        visible: root.live
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + wall.height + (wall.pageCount > 1 ? 22 : 8)
        width: 376
        height: transportRow.height + 12
        radius: 8
        color: "#14151d"
        border.color: "#232530"
        border.width: 1

        readonly property bool atStart: LiveOutputService.onAirIndex <= 0
        readonly property bool atEnd: LiveOutputService.onAirTotal > 0 && LiveOutputService.onAirIndex >= LiveOutputService.onAirTotal - 1
        readonly property bool onAir: LiveOutputService.onAirTotal > 0

        // A toolbar button — one glyph in a PILL (always-visible rounded
        // chip, like FreeShow's MaterialButton tiles) that brightens on
        // hover; danger buttons tint red. The pill NEVER dims: a disabled
        // action (prev on the first slide, the inert lock/transition) dims
        // its GLYPH only, so every slot keeps its chip and its hover.
        component ToolButton: Item {
            id: toolBtn
            property string icon: ""
            property bool enabled2: true
            property bool danger: false
            signal picked()
            width: 36; height: 30
            Rectangle {
                anchors.fill: parent; radius: 8
                color: {
                    if (toolBtn.danger)
                        return toolArea.containsMouse ? "#33ff4d3d" : "#26ff4d3d"
                    return toolArea.containsMouse ? "#262a3a" : "#1c1e29"
                }
                border.color: {
                    if (toolBtn.danger)
                        return toolArea.containsMouse ? "#66ff4d3d" : "#33ff4d3d"
                    return toolArea.containsMouse ? "#39405c" : "#262a3a"
                }
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }
            }
            IconGlyph {
                anchors.centerIn: parent
                name: toolBtn.icon
                color: toolBtn.danger ? "#ff6b61" : (toolBtn.enabled2 ? Theme.textPrimary : Theme.textMuted)
                width: 15; height: 15; fit: true
                Behavior on color { ColorAnimation { duration: 100 } }
            }
            HoverHandler { id: toolArea; cursorShape: toolBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (toolBtn.enabled2) toolBtn.picked() }
        }

        Column {
            id: transportRow
            x: 10; y: 8
            width: parent.width - 20
            spacing: 6

            // ShowActions: [previous next play lock transition] — spread
            // across the full strip (FreeShow's buttons are flex-grow: 1, so
            // every action owns an equal slot instead of huddling mid-bar).
            Row {
                width: parent.width

                ToolButton {
                    width: parent.width / 5; height: 30
                    icon: "previous"
                    enabled2: !toolbar.atStart
                    onPicked: LiveOutputService.previous()
                }
                ToolButton {
                    width: parent.width / 5; height: 30
                    icon: "next"
                    enabled2: !toolbar.atEnd
                    onPicked: LiveOutputService.next()
                }
                // Play: when live, an end-of-list restart (the engine runs
                // the show itself; there is no separate start).
                ToolButton {
                    width: parent.width / 5; height: 30
                    icon: "play"
                    enabled2: toolbar.atEnd || !toolbar.onAir
                    onPicked: {
                        if (!LiveOutputService.live) return
                        LiveOutputService.jumpTo(0)
                    }
                }
                // Lock: Ctrl+L in FreeShow — arms the "nothing can change the
                // output" state; the engine has no output lock yet, so the
                // button shows the unlocked state and is inert (glyph dimmed,
                // pill + hover intact).
                ToolButton {
                    width: parent.width / 5; height: 30
                    icon: "unlocked"
                    enabled2: false
                }
                // Transition: FreeShow opens its transition popup; there is
                // no engine transition editor yet, so the glyph is a marker.
                ToolButton {
                    width: parent.width / 5; height: 30
                    icon: "transition"
                    enabled2: false
                }
            }

            // Clear all (ClearButtons' red full-width clear.all).
            Item {
                width: parent.width; height: 28
                Rectangle {
                    anchors.fill: parent; radius: 8
                    color: clearAllArea.containsMouse ? "#2fff4d3d" : "#1cff4d3d"
                    border.color: clearAllArea.containsMouse ? "#66ff4d3d" : "#33ff4d3d"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }
                }
                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    IconGlyph { name: "clearIcon"; color: "#ff6b61"; width: 13; height: 13; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: qsTr("Clear all"); color: "#ff6b61"; font.family: "Segoe UI"; font.pixelSize: 12; font.weight: Font.Medium; anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: clearAllArea; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: LiveOutputService.stop() }
            }

            // ClearButtons' per-layer group (image/slide/overlays/audio/timer)
            // — same equal-slot spreading as the transport row. Until the
            // engine grows per-layer clear, each button IS the whole-output
            // clear like Clear all.
            Row {
                width: parent.width

                Repeater {
                    model: [
                        { icon: "image", tip: qsTr("Clear background") },
                        { icon: "scripture", tip: qsTr("Clear slide") },
                        { icon: "overlays", tip: qsTr("Clear overlays") },
                        { icon: "audio", tip: qsTr("Clear audio") },
                        { icon: "timerFill", tip: qsTr("Clear timers") }
                    ]

                    delegate: ToolButton {
                        required property var modelData
                        width: parent.width / 5; height: 30
                        icon: modelData.icon
                        onPicked: LiveOutputService.stop()
                    }
                }
            }
        }
    }

    // The wall's total height must account for the dots row's visual space
    // even when the toolbar is hidden (hosts hang content +22 below).
    onVisibleChanged: if (visible) root.height = tabs.height + wall.height + (toolbar.visible ? toolbar.height : 0)
}
