import QtQuick
import VGRPresenterUI

// The centre page of the Show screen (FreeShow's Show.svelte): what the item you clicked - in the Projects panel or the Shows table -
// looks like. A show is its slide grid (drawn by the same DesignPreview as everywhere else, read from the show's file WITHOUT opening
// it for editing); media shows the picture or the file; audio, overlays, scripture and sections show what they are. "Edit" opens the show
// on the Edit screen. Nothing clicked yet: the splash behind this stays.
//
// `item` is a project item ({ type, ref, name, layout, meta }) or a bare one ({ type: "show", ref: path, name }).
Item {
    id: root

    property var item: null
    signal editRequested(string path)
    signal closeRequested()

    readonly property string type: item ? (item.type ?? "") : ""
    property int reload: 0            // bumped when the show's file changed under us (the next timer), so it is read again
    readonly property var loaded: { root.reload; return root.type === "show" ? ShowService.peekShow(item.ref) : null }
    readonly property var show: loaded && loaded.ok ? loaded.show : null
    readonly property var slides: show ? show.slides : []
    property int selectedSlide: -1
    onItemChanged: selectedSlide = -1

    // The view (FreeShow's slidesOptions), kept by the engine's settings: how many slides across, and grid / list / lyrics.
    readonly property int columns: Math.max(2, Math.min(10, Number(SettingsService.values["session.slideColumns"] ?? 4)))
    readonly property string viewMode: String(SettingsService.values["session.slideView"] ?? "grid")
    readonly property real cellWidth: Math.floor((width - 10) / columns)     // (FreeShow: the grid has 5px padding, a cell is 100 / columns %)
    // FreeShow's "dark" theme
    readonly property color cLighter: "#2f3542"
    readonly property color cDarker: "#191923"
    readonly property color cDarkest: "#12121c"
    readonly property color cText: "#f0f0ff"
    readonly property color cSecondary: "#E64934"
    readonly property string mono: "Consolas"
    property bool headerMenuOpen: false
    // the colour of a slide's group (verse, chorus...): the bar under its picture
    function groupColor(slide) {
        const map = { break: "#f5255e", bridge: "#f52598", chorus: "#f525d2", intro: "#d525f5", outro: "#a525f5", pre_chorus: "#8825f5", tag: "#7525f5", verse: "#5825f5" }
        const norm = (t) => String(t ?? "").toLowerCase().replace(/[\s-]+/g, "_").replace(/_?\d+$/, "")
        return map[norm(slide.tag)] ?? map[norm(slide.title)] ?? ""
    }
    property bool barOpen: false      // the floating bar's chevron: shows the zoom
    property bool zoomOpen: false     // the zoom's + / - popup

    function setColumns(n) {
        const next = Math.max(2, Math.min(10, n))
        if (next !== root.columns)
            SettingsService.setValue("session.slideColumns", next)
    }
    // grid -> list -> lyrics -> grid, like FreeShow's view button
    function nextView() {
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
    function applyTimer() {
        if (!root.item) return
        const seconds = root.timerApplied ? 0 : root.timerSeconds
        if (ShowService.setNextTimer(root.item.ref, seconds)) {
            root.reload++
            root.timerOpen = false
        }
    }
    // A slide's words, for the list and lyrics views.
    function slideText(slide) {
        return (slide.blocks ?? []).filter((b) => (b.kind ?? "text") === "text" && (b.text ?? "") !== "").map((b) => b.text).join("\n")
    }

    // The show on this page can be the one open in the editor: read it again when it changes, and when this page comes back into view.
    Connections {
        target: ShowService
        function onShowChanged() { if (root.visible && root.type === "show") root.reload++ }
    }
    onVisibleChanged: if (visible && root.type === "show") root.reload++

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
            Rectangle { anchors.fill: parent; color: headerMoreHover.hovered ? "#14ffffff" : "transparent" }
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
                    Rectangle { visible: menuEntry.modelData !== "-"; anchors.fill: parent; color: menuEntryHover.hovered ? "#0dffffff" : "transparent" }
                    Text { visible: menuEntry.modelData !== "-"; x: 12; anchors.verticalCenter: parent.verticalCenter; text: menuEntry.modelData; color: root.cText; font.family: root.mono; font.pixelSize: 15 }
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
                            width: parent.width; height: Math.round(width * 428 / 754)
                            color: "#000000"; clip: true
                            DesignPreview {
                                width: parent.width
                                blocks: cell.modelData.blocks ?? []
                                background: cell.modelData.background ?? "transparent"
                                checkerSize: 32
                            }
                            Rectangle { anchors.fill: parent; color: "#0dffffff"; visible: cellHover.hovered }      // the hover veil
                            // the slide's next timer
                            Rectangle {
                                visible: (cell.modelData.nextTimer ?? 0) > 0
                                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
                                width: timerLabel.width + 8; height: 16; radius: 3; color: "#cc0b0b14"
                                Text { id: timerLabel; anchors.centerIn: parent; text: root.formatTime(cell.modelData.nextTimer ?? 0); color: "#ff8a7e"; font.family: root.mono; font.pixelSize: 12 }
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
                        onTapped: root.selectedSlide = cell.index
                        onDoubleTapped: root.editRequested(root.item.ref)
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
                    width: listCol.width; height: 84; radius: 6
                    color: selected ? "#1a1b23" : (listHover.hovered ? "#15161d" : "#12131a")
                    border.color: selected ? "#ff4d3d" : "#1d1f2a"
                    Rectangle {
                        x: 10; y: 10; width: 106; height: Math.round(width * 428 / 754); color: "#000"; radius: 3; clip: true
                        DesignPreview {
                            width: parent.width
                            blocks: listRow.modelData.blocks ?? []
                            background: listRow.modelData.background ?? "transparent"
                            checkerSize: 24
                        }
                    }
                    Text { x: 130; y: 10; text: (listRow.index + 1) + "  " + (listRow.modelData.title ?? "") + ((listRow.modelData.nextTimer ?? 0) > 0 ? "  ·  " + root.formatTime(listRow.modelData.nextTimer) : ""); color: "#e2e8f0"; font.family: "Segoe UI"; font.pixelSize: 14; font.weight: Font.DemiBold }
                    Text {
                        x: 130; y: 30; width: parent.width - 146
                        text: root.slideText(listRow.modelData)
                        color: "#8a94a6"; wrapMode: Text.Wrap; elide: Text.ElideRight; maximumLineCount: 3
                        font.family: "Segoe UI"; font.pixelSize: 14
                    }
                    HoverHandler { id: listHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: root.selectedSlide = listRow.index
                        onDoubleTapped: root.editRequested(root.item.ref)
                    }
                }
            }
        }

        // the words only, group by group
        Column {
            id: lyricsCol
            visible: root.viewMode === "lyrics"
            x: Math.max(0, (parent.width - width) / 2)
            width: Math.min(parent.width, 760)
            spacing: 4
            Repeater {
                model: root.viewMode === "lyrics" ? root.slides : []
                delegate: Rectangle {
                    id: lyricRow
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.selectedSlide === index
                    width: lyricsCol.width; height: lyricText.height + lyricTitle.height + 24; radius: 6
                    color: selected ? "#1a1b23" : (lyricHover.hovered ? "#13141b" : "transparent")
                    Text {
                        id: lyricTitle
                        x: 14; y: 8
                        text: (lyricRow.modelData.title ?? "") !== "" ? lyricRow.modelData.title : qsTr("Slide %1").arg(lyricRow.index + 1)
                        color: "#ff4d3d"; font.capitalization: Font.AllUppercase
                        Component.onCompleted: if ((lyricRow.modelData.nextTimer ?? 0) > 0) text += "  ·  " + root.formatTime(lyricRow.modelData.nextTimer)
                        font.family: "Segoe UI"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1
                    }
                    Text {
                        id: lyricText
                        x: 14; y: lyricTitle.y + lyricTitle.height + 4; width: parent.width - 28
                        text: root.slideText(lyricRow.modelData)
                        color: "#e2e8f0"; wrapMode: Text.Wrap
                        font.family: "Segoe UI"; font.pixelSize: 17; lineHeight: 1.25
                    }
                    HoverHandler { id: lyricHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: root.selectedSlide = lyricRow.index
                        onDoubleTapped: root.editRequested(root.item.ref)
                    }
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

            // chevron: opens the bar (the zoom hides behind it, like FreeShow's)
            Item {
                objectName: "selfTestBarChevron"
                width: 40; height: parent.height
                Rectangle { anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: chevHover.hovered ? "#22242e" : "transparent" }
                // a chevron drawn as two bars, so it sits dead centre: > while the bar is open, < while it is shut
                Item {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    opacity: root.barOpen ? 1 : 0.65
                    rotation: root.barOpen ? 0 : 180
                    Behavior on rotation { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                    Rectangle { x: 4.5 - width / 2; y: 3.4 - height / 2; width: 8; height: 2; radius: 1; color: "#ff4d3d"; rotation: 45 }
                    Rectangle { x: 4.5 - width / 2; y: 8.6 - height / 2; width: 8; height: 2; radius: 1; color: "#ff4d3d"; rotation: -45 }
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
                Rectangle { anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: timerHover.hovered || root.timerOpen ? "#22242e" : "transparent" }
                IconGlyph { anchors.centerIn: parent; name: "clock"; color: root.timerTotal > 0 ? "#ff4d3d" : "#ffffff"; fit: true; width: 16; height: 16 }
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
                    Column {
                        id: timerPanel
                        x: 16; y: 14; width: parent.width - 32
                        spacing: 12

                        Text { text: qsTr("NEXT TIMER"); color: "#8a94a6"; font.family: "Segoe UI"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1.2 }

                        // seconds: - [ 10 ] +
                        Rectangle {
                            width: parent.width; height: 40; radius: 8; color: "#0f1015"; border.color: "#232530"
                            Item {
                                width: 40; height: parent.height
                                opacity: root.timerSeconds > 0 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: "#ffffff" }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.max(0, root.timerSeconds - 1) }
                            }
                            Row {
                                anchors.centerIn: parent; spacing: 6
                                TextInput {
                                    id: timerInput
                                    width: 44; horizontalAlignment: TextInput.AlignRight
                                    color: "#ffffff"; selectByMouse: true
                                    font.family: "Segoe UI"; font.pixelSize: 18; font.weight: Font.DemiBold
                                    validator: IntValidator { bottom: 0; top: 3600 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    onTextEdited: root.timerSeconds = Number(text) || 0
                                    Component.onCompleted: text = String(root.timerSeconds)
                                    Connections {
                                        target: root
                                        function onTimerSecondsChanged() { if (timerInput.text !== String(root.timerSeconds)) timerInput.text = String(root.timerSeconds) }
                                    }
                                }
                                Text { anchors.baseline: timerInput.baseline; text: qsTr("seconds"); color: "#5c6475"; font.family: "Segoe UI"; font.pixelSize: 14 }
                            }
                            Item {
                                anchors.right: parent.right
                                width: 40; height: parent.height
                                opacity: root.timerSeconds < 3600 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: "#ffffff" }
                                Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: "#ffffff" }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.min(3600, root.timerSeconds + 1) }
                            }
                        }

                        // apply it to every slide, or take it off again
                        Rectangle {
                            width: parent.width; height: 40; radius: 20
                            readonly property bool usable: root.timerApplied || root.timerSeconds > 0
                            opacity: usable ? 1 : 0.4
                            color: root.timerApplied ? (applyHover.hovered ? "#22242e" : "transparent") : (applyHover.hovered ? "#a03a30" : "#85261f")
                            border.color: root.timerApplied ? "#3a3d4c" : "#ff4d3d"
                            Row {
                                anchors.centerIn: parent; spacing: 8
                                Text { text: root.timerApplied ? qsTr("Reset") : qsTr("To all slides"); color: "#ffffff"; font.family: "Segoe UI"; font.pixelSize: 15; font.weight: Font.DemiBold }
                                Text { visible: !root.timerApplied && root.timerSeconds > 0; text: root.formatTime(root.timerSeconds * root.slides.length); color: "#ffb4ab"; font.family: "Segoe UI"; font.pixelSize: 14 }
                            }
                            HoverHandler { id: applyHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            TapHandler { enabled: parent.usable; onTapped: root.applyTimer() }
                        }

                        Text {
                            visible: root.timerTotal > 0
                            width: parent.width; horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Duration: %1").arg(root.formatTime(root.timerTotal))
                            color: "#8a94a6"; font.family: "Segoe UI"; font.pixelSize: 13
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
                Rectangle { anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: zoomHover.hovered || root.zoomOpen ? "#22242e" : "transparent" }
                IconGlyph { anchors.centerIn: parent; name: "zoomIn"; color: root.zoomOpen ? "#ffffff" : "#c9cedd"; fit: true; width: 16; height: 16 }
                HoverHandler { id: zoomHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { root.zoomOpen = !root.zoomOpen; root.timerOpen = false } }

                Rectangle {
                    visible: root.zoomOpen
                    x: -1; y: -height - 2; width: parent.width + 2; height: 122
                    radius: 20; color: "#f018192a"; border.color: root.cLighter
                    // (rounded top only: the bottom sits flat on the button)
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 20; color: parent.color }
                    Rectangle { anchors.bottom: parent.bottom; anchors.bottomMargin: -1; x: parent.border.width; width: parent.width - 2; height: 2; color: parent.color }
                    Column {
                        anchors.fill: parent; anchors.topMargin: 1
                        Item {
                            width: parent.width; height: 38
                            Text { anchors.centerIn: parent; text: Math.round(100 / root.columns) + "%"; color: "#8a94a6"; font.family: "Segoe UI"; font.pixelSize: 13 }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(4) }   // the usual size
                        }
                        Rectangle { width: parent.width; height: 1; color: root.cLighter }
                        // + makes the slides bigger (fewer across)
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns > 2 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: "#ffffff" }
                            Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: "#ffffff" }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(root.columns - 1) }
                        }
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns < 10 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: "#ffffff" }
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
                Rectangle { anchors.centerIn: parent; width: 34; height: 34; radius: 17; color: viewHover.hovered ? "#22242e" : "transparent" }
                IconGlyph {
                    anchors.centerIn: parent
                    name: root.viewMode === "list" ? "listView" : (root.viewMode === "lyrics" ? "textLines" : "gridView")
                    color: root.viewMode === "grid" ? "#ffffff" : "#c9cedd"
                    fit: true; width: 18; height: 18
                }
                HoverHandler { id: viewHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nextView() }
            }
        }
    }

    Text {
        visible: root.type === "show" && root.show === null
        anchors.centerIn: parent
        text: root.loaded && root.loaded.error ? qsTr("This show could not be read: %1").arg(root.loaded.error) : ""
        color: "#ff6b61"; font.family: "Segoe UI"; font.pixelSize: 15
    }

    // ---- a picture ----
    Image {
        visible: root.type === "image"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        source: root.type === "image" && root.item ? "file:///" + String(root.item.ref).replace(/\\/g, "/") : ""
    }

    // ---- an overlay ----
    Item {
        visible: root.type === "overlay"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        readonly property var design: root.type === "overlay" ? OverlayLibraryService.design(root.item.ref) : ({})
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height * 754 / 428); height: Math.round(width * 428 / 754)
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
            width: Math.min(parent.width, parent.height * 754 / 428); height: Math.round(width * 428 / 754)
            color: "#000"; clip: true
            DesignPreview {
                anchors.fill: parent
                blocks: scriptureView.design.blocks ?? []
                background: scriptureView.design.background ?? "transparent"
            }
        }
    }

    // ---- everything else: what it is ----
    Column {
        visible: root.item !== null && root.type !== "show" && root.type !== "image" && root.type !== "overlay" && !(root.type === "scripture" && scriptureView.ready)
        anchors.centerIn: parent
        spacing: 10
        IconGlyph {
            anchors.horizontalCenter: parent.horizontalCenter
            name: root.type === "audio" ? "music" : root.type === "video" || root.type === "media" ? "play" : root.type === "scripture" ? "bookOpen"
                : root.type === "camera" ? "camera" : root.type === "pdf" ? "fileText" : "layoutDashboard"
            color: "#5c6475"; fit: true; width: 40; height: 40
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type === "section"
            text: root.item ? root.item.name : ""
            color: "#e2e8f0"; font.family: "Segoe UI"; font.pixelSize: 30; font.weight: Font.DemiBold; font.capitalization: Font.AllUppercase
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type !== "section" && root.item !== null
            width: Math.min(root.width - 60, 520); horizontalAlignment: Text.AlignHCenter
            text: root.item ? String(root.item.ref) : ""
            color: "#5c6475"; elide: Text.ElideMiddle
            font.family: "Segoe UI"; font.pixelSize: 14
        }
    }
}
