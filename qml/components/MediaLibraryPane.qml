import QtQuick
import VGRPresenterUI

// The Media library tab: a resizable sidebar (All / Inputs / your folders) and a content area.
//
//   All, and each folder .... a grid of the images and videos MediaLibraryService found in the
//                             folders the user added (Add folder = import), with previews.
//   Inputs .................. the live sources: Video sources | Audio inputs | Buses as a
//                             horizontal tab row, with the selected roster's cards below. They
//                             are the exact rosters Settings - Audio & Video creates
//                             (VideoSourceListModel / AudioInputListModel / BusListModel).
//
// The sidebar is a LibrarySidebar (drag its divider to resize), its rows are SidebarRows and
// its foot button a SidebarAddButton - the same pieces the other library tabs use.
Item {
    id: root

    // A tile was clicked / double-clicked: { type: "image"|"video"|"audio", ref: path, name }.
    signal itemActivated(var item)
    signal itemOpened(var item)

    // Reactivity bridge (same pattern as AudioVideoScreen): plain Q_INVOKABLE rowCount()/get*()
    // reads aren't tracked by QML's binding system, so this counter is the honest dependency -
    // bumped by all three models, referenced by the count bindings below.
    property int modelsRev: 0
    Connections {
        target: VideoSourceListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: BusListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    // The dock tab bar's Media search: only files whose name matches (the engine's search, so it
    // works by name across every folder), and the Inputs cards by name.
    property string filter: ""
    readonly property string query: filter.trim()

    // ---- what is selected ----------------------------------------------------
    property string selection: "all"   // "all" | "inputs" | a folder's path
    property int inputTab: 0           // 0 = Video sources, 1 = Audio inputs, 2 = Buses
    readonly property bool inputsMode: selection === "inputs"

    readonly property var inputTabs: [
        { label: qsTr("Video sources"), icon: "camera",  kind: "video" },
        { label: qsTr("Audio inputs"),  icon: "mic",     kind: "audio" },
        { label: qsTr("Buses"),         icon: "volume2", kind: "bus" }
    ]
    // The rosters, as lists of the row numbers that match the search (all of them with no search).
    readonly property var inputRows: {
        const _ = root.modelsRev   // reactivity dependency
        const counts = [VideoSourceListModel.rowCount(), AudioInputListModel.rowCount(), BusListModel.rowCount()]
        const kinds = ["video", "audio", "bus"]
        const needle = root.query.toLowerCase()
        return counts.map((n, roster) => {
            const rows = []
            for (let i = 0; i < n; ++i)
                if (needle === "" || root.cardName(i, kinds[roster]).toLowerCase().indexOf(needle) >= 0)
                    rows.push(i)
            return rows
        })
    }
    readonly property var inputCounts: {
        const _ = root.modelsRev   // reactivity dependency
        return [VideoSourceListModel.rowCount(), AudioInputListModel.rowCount(), BusListModel.rowCount()]
    }
    readonly property int inputTotal: inputCounts[0] + inputCounts[1] + inputCounts[2]
    readonly property int tabRowHeight: 44

    // ---- the media grid's items ----------------------------------------------
    property var gridItems: []
    // Is what is on screen still being read? (A folder's files arrive when its scan finishes, and until
    // then the grid is empty - which must not look the same as a folder with nothing in it.)
    readonly property bool loading: {
        const rows = MediaLibraryService.folderTree
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].scanning && (root.selection === "all" || root.selection === rows[i].path)) return true
        return false
    }
    function reload() {
        gridItems = inputsMode ? [] : MediaLibraryService.items(selection === "all" ? "" : selection, query)
    }
    onSelectionChanged: reload()
    onQueryChanged: reload()
    Component.onCompleted: reload()

    // Which folders of the tree are open (path -> true). A library folder shows everything under it whether
    // or not it is open; opening it only lists the folders inside so one can be chosen on its own.
    property var expanded: ({})
    function toggle(path) {
        const next = Object.assign({}, root.expanded)
        next[path] = !next[path]
        root.expanded = next
    }
    // The tree rows that are showing: a row is there when every folder above it is open.
    readonly property var treeRows: {
        const all = MediaLibraryService.folderTree
        const open = root.expanded
        const shown = {}
        const out = []
        for (let i = 0; i < all.length; ++i) {
            const row = all[i]
            const visible = row.depth === 0 || (shown[row.parent] === true && open[row.parent] === true)
            shown[row.path] = visible
            if (visible) out.push(row)
        }
        return out
    }

    function folderExists(path) {
        const rows = MediaLibraryService.folderTree
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].path === path) return true
        return false
    }
    Connections {
        target: MediaLibraryService
        function onFoldersChanged() {
            // A folder that was removed can't stay selected.
            if (root.selection !== "all" && root.selection !== "inputs" && !root.folderExists(root.selection))
                root.selection = "all"
            root.reload()
        }
    }

    // Asks for a folder and shows it.
    function addFolder() {
        if (!MediaLibraryService.addFolder())
            return
        const list = MediaLibraryService.folders
        root.selection = list[list.length - 1].path
    }

    // ---- Sidebar --------------------------------------------------------------
    LibrarySidebar {
        id: sidebar
        height: parent.height
        defaultWidth: 260

        Column {
            id: fixedRows
            x: 8; y: 8
            width: parent.width - 16
            spacing: 2

            SidebarRow {
                width: parent.width
                icon: "layoutDashboard"
                label: qsTr("All")
                count: String(MediaLibraryService.totalCount)
                selected: root.selection === "all"
                onClicked: root.selection = "all"
            }
            SidebarRow {
                width: parent.width
                icon: "camera"
                label: qsTr("Inputs")
                count: String(root.inputTotal)
                selected: root.inputsMode
                onClicked: root.selection = "inputs"
            }
        }

        Text {
            id: foldersHeading
            x: 16
            y: fixedRows.y + fixedRows.height + 14
            text: qsTr("FOLDERS")
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
        }

        Flickable {
            id: folderFlick
            x: 8
            y: foldersHeading.y + foldersHeading.height + 6
            width: parent.width - 16
            height: parent.height - y - 48
            clip: true
            contentWidth: width
            contentHeight: folderColumn.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: folderColumn
                width: folderFlick.width
                spacing: 2

                Repeater {
                    model: root.treeRows
                    delegate: SidebarRow {
                        required property var modelData
                        objectName: "selfTestRow_" + modelData.name
                        width: folderColumn.width
                        icon: "folder"
                        label: modelData.name
                        count: String(modelData.count)
                        busy: modelData.scanning && modelData.root
                        // Only the folders the user added can be removed; nested ones come with them.
                        removable: modelData.root
                        tree: true
                        depth: modelData.depth
                        expandable: modelData.hasChildren
                        expanded: root.expanded[modelData.path] === true
                        selected: root.selection === modelData.path
                        onClicked: root.selection = modelData.path
                        onToggleRequested: root.toggle(modelData.path)
                        onRemoveRequested: MediaLibraryService.removeFolder(modelData.path)
                    }
                }
            }
        }

        SidebarAddButton {
            x: 8
            y: parent.height - 40
            width: parent.width - 16
            text: qsTr("Add folder")
            onClicked: root.addFolder()
        }
    }

    // ---- Media grid (All / a folder) ---------------------------------------------
    Item {
        id: mediaArea
        visible: !root.inputsMode
        x: sidebar.width
        width: parent.width - x
        height: parent.height

        GridView {
            id: grid
            anchors.fill: parent
            anchors.margins: 4
            anchors.rightMargin: 10   // room for the scrollbar
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.gridItems

            readonly property int columns: Math.max(2, Math.floor(width / 230))
            cellWidth: Math.floor(width / columns)
            // The tile's 16:9 preview (its width less the 12 px of margins) plus the name bar.
            cellHeight: Math.round((cellWidth - 12) * 9 / 16) + 12 + 30

            delegate: MediaTile {
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight
                name: modelData.name
                path: modelData.path
                kind: modelData.kind
                onActivated: root.itemActivated({ type: modelData.kind, ref: modelData.path, name: modelData.name })
                onOpened: root.itemOpened({ type: modelData.kind, ref: modelData.path, name: modelData.name })
            }
        }
        AppScrollBar {
            anchors.right: parent.right
            anchors.rightMargin: 3
            height: parent.height
            flickable: grid
        }

        // Nothing to show: still reading the folder (a spinner), nothing added yet, or really empty.
        Column {
            visible: root.gridItems.length === 0
            anchors.centerIn: parent
            spacing: 12
            width: Math.min(parent.width - 40, 380)

            BusySpinner {
                visible: root.loading
                anchors.horizontalCenter: parent.horizontalCenter
                width: 30; height: 30
                color: Theme.textSecondary
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.loading
                      ? qsTr("Reading the folder...")
                      : (MediaLibraryService.folders.length === 0
                         ? qsTr("No media yet. Add a folder with your images, videos and audio and they appear here, with previews.")
                         : (root.query !== ""
                            ? qsTr("Nothing named \u201c%1\u201d.").arg(root.query)
                            : qsTr("No images, videos or audio found here.")))
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: 14
            }
            AppButton {
                visible: !root.loading && MediaLibraryService.folders.length === 0
                anchors.horizontalCenter: parent.horizontalCenter
                variant: "secondary"
                text: qsTr("Add folder")
                onClicked: root.addFolder()
            }
        }
    }

    // ---- Inputs: Video sources | Audio inputs | Buses ------------------------------
    Item {
        id: inputsArea
        visible: root.inputsMode
        x: sidebar.width
        width: parent.width - x
        height: parent.height

        // The horizontal tab row, like the reference's Cameras / Screens / NDI / Blackmagic.
        Item {
            id: tabRow
            width: parent.width
            height: root.tabRowHeight

            Row {
                anchors.fill: parent

                Repeater {
                    model: root.inputTabs.length
                    delegate: Item {
                        id: tabItem
                        required property int index
                        readonly property bool selected: root.inputTab === index
                        width: tabRow.width / root.inputTabs.length
                        height: tabRow.height

                        Rectangle {
                            anchors.fill: parent
                            color: !tabItem.selected && tabHover.hovered ? "#14151c" : "transparent"
                        }
                        Row {
                            anchors.centerIn: parent
                            spacing: 9
                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: -1   // centres the glyph on the text's x-height rather than its line box
                                name: root.inputTabs[tabItem.index].icon
                                color: tabItem.selected ? Theme.textPrimary : Theme.textSecondary
                                fit: true
                                strokeWidth: 2   // the mic draws 1.5 by default; the camera and speaker draw 2
                                width: 16; height: 16
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.inputTabs[tabItem.index].label
                                color: tabItem.selected ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontFamily; font.pixelSize: 15
                                font.weight: tabItem.selected ? Font.DemiBold : Font.Medium
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.inputCounts[tabItem.index]
                                color: Theme.textMuted
                                font.family: Theme.fontFamily; font.pixelSize: 13
                            }
                        }
                        // Selected underline (full tab width, like the reference).
                        Rectangle {
                            visible: tabItem.selected
                            anchors.bottom: parent.bottom
                            width: parent.width; height: 2
                            color: Theme.danger
                        }
                        PositionHoverArea {
                            id: tabHover
                            anchors.fill: parent
                            onClicked: root.inputTab = tabItem.index
                        }
                    }
                }
            }
            // Hairline under the whole row.
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        // The selected roster's cards, three to a row.
        Flickable {
            id: cardsFlick
            y: root.tabRowHeight
            width: parent.width
            height: parent.height - y
            clip: true
            contentHeight: inputCards.height + 20

            Column {
                id: inputCards
                x: 0; y: 10
                width: parent.width - 16
                spacing: 8

                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: root.inputRows[root.inputTab]
                        delegate: Rectangle {
                            id: card
                            required property int modelData
                            readonly property int index: modelData
                            readonly property string kind: root.inputTabs[root.inputTab].kind
                            // VIDEO cards are a STATE WINDOW + the name below:
                            // the rectangle shows the feed when the source is
                            // receiving frames, "No signal" when it is not
                            // (device disconnected / window gone), and an idle
                            // glyph when the source is not taken. Audio/bus
                            // keep the compact 44px rows.
                            readonly property bool bigCard: kind === "video"
                            readonly property string sub: {
                                void root.modelsRev
                                if (kind !== "video")
                                    return ""
                                const v = VideoSourceListModel.getSource(index)
                                return v.sublabel !== undefined ? v.sublabel : ""
                            }
                            // The TAKEN source's card wears the accent —
                            // "which one is on the output preview" reads
                            // from the pane too.
                            readonly property bool taken: sub !== ""
                                                          && LiveOutputService.inputLabel === sub
                            width: (inputCards.width - 16) / 3
                            height: bigCard ? 8 + (width - 16) * 9 / 16 + 6 + 32 + 8 : 44
                            radius: 6
                            color: cardHover.hovered ? "#1e1f28" : "#16171e"
                            border.width: taken ? 2 : 1
                            border.color: taken ? "#6c5ce7" : "transparent"

                            PositionHoverArea {
                                id: cardHover
                                anchors.fill: parent
                                // DOUBLE-CLICK = OUTPUT PREVIEW (the deliberate
                                // take): a stray single click can never grab or
                                // drop a feed. Video cards toggle their feed
                                // into the monitor wall's input layer on the
                                // second click of a double-click; audio/bus
                                // cards have no video feed — inert, as before.
                                onDoubleClicked: {
                                    if (kind === "video")
                                        root.toggleVideoSourcePreview(index)
                                }
                            }

                            // ---- COMPACT row (audio / bus) ----------------
                            Rectangle {
                                visible: !bigCard
                                x: 8; y: 10
                                width: 24; height: 24
                                radius: 5
                                color: kind === "audio" ? "#173326" : "#3a1e18"
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: root.inputTabs[root.inputTab].icon
                                    color: kind === "audio" ? "#6fe0a0" : "#ff8d7f"
                                    fit: true
                                    strokeWidth: 2
                                    width: 14; height: 14
                                }
                            }
                            Column {
                                visible: !bigCard
                                x: 40; y: 6
                                width: parent.width - 52
                                spacing: 1
                                Text {
                                    width: parent.width
                                    text: root.cardName(index, kind)
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily; font.pixelSize: 14
                                }
                                Text {
                                    width: parent.width
                                    text: root.cardSub(index, kind)
                                    color: Theme.textMuted
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily; font.pixelSize: 12
                                }
                            }

                            // ---- STATE WINDOW (video) ---------------------
                            // The rectangle IS the signal state: feed while
                            // frames flow, "No signal" while the taken source
                            // delivers nothing (disconnected device, closed
                            // window), idle glyph while not taken.
                            Rectangle {
                                id: stateWin
                                visible: bigCard
                                x: 8; y: 8
                                width: parent.width - 16
                                height: width * 9 / 16
                                radius: 4
                                clip: true
                                color: "#0d0f14"
                                border.width: 1
                                border.color: card.taken ? "#3d3f6e" : "#232530"

                                // RECEIVING: the live feed (the same provider
                                // frames the monitor tile shows — the tap runs
                                // under the service's take; the pump's rev
                                // re-fetches at ~15 fps).
                                Image {
                                    anchors.fill: parent
                                    visible: card.taken && LiveOutputService.inputLive
                                    source: visible
                                        ? "image://videopreview/" + encodeURIComponent(card.sub)
                                          + "?n=" + LiveOutputService.inputRev : ""
                                    fillMode: Image.PreserveAspectCrop
                                    cache: false
                                    asynchronous: false
                                }
                                // NOT RECEIVING: taken but no frames — the
                                // honest "this device is not connected / not
                                // delivering" state (also covers tap warm-up).
                                Column {
                                    anchors.centerIn: parent
                                    visible: card.taken && !LiveOutputService.inputLive
                                    spacing: 6
                                    IconGlyph {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        name: root.inputTabs[root.inputTab].icon
                                        color: "#5a5f72"
                                        fit: true; strokeWidth: 2
                                        width: 20; height: 20
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: qsTr("No signal — device not receiving")
                                        color: "#8a90a5"
                                        font.family: Theme.fontFamily; font.pixelSize: 10
                                    }
                                }
                                // IDLE: not taken — dim glyph; hover spells the
                                // gesture (the double-click take).
                                Column {
                                    anchors.centerIn: parent
                                    visible: !card.taken
                                    spacing: 6
                                    IconGlyph {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        name: root.inputTabs[root.inputTab].icon
                                        color: "#4a5068"
                                        fit: true; strokeWidth: 2
                                        width: 20; height: 20
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: cardHover.hovered
                                              ? qsTr("double-click to preview on output")
                                              : qsTr("idle")
                                        color: cardHover.hovered ? "#a9b6d8" : "#565c72"
                                        font.family: Theme.fontFamily; font.pixelSize: 10
                                    }
                                }
                            }
                            // NAME below the window, sublabel under it.
                            Text {
                                visible: bigCard
                                x: 8; y: 8 + stateWin.height + 6
                                width: parent.width - 16
                                text: root.cardName(index, kind)
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily; font.pixelSize: 13; font.bold: true
                            }
                            Text {
                                visible: bigCard
                                x: 8; y: 8 + stateWin.height + 6 + 17 + 2
                                width: parent.width - 16
                                text: root.cardSub(index, kind)
                                color: Theme.textMuted
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily; font.pixelSize: 11
                            }
                        }
                    }
                }
                Text {
                    visible: root.inputRows[root.inputTab].length === 0
                    width: parent.width
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: root.query !== ""
                          ? qsTr("Nothing named \u201c%1\u201d.").arg(root.query)
                          : qsTr("Nothing here yet - add %1 in Settings - Audio & Video.")
                                .arg(root.inputTabs[root.inputTab].label.toLowerCase())
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 14
                    wrapMode: Text.WordWrap
                }
            }

            // A big video-input roster overflows — wheel-only before.
            AppScrollBar {
                flickable: cardsFlick
                anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.right: parent.right; anchors.rightMargin: 2
            }
        }
    }

    // ---- Output-preview input take (the video cards' click) --------------
    // The service owns the PAL tap and the tile layer; this is the pane's
    // thin toggle: taken-again clears, unsupported kinds toast honestly.
    // (The double-click guard lives in the SERVICE — a double-click fires
    // TWO clicked signals, and the too-fast second toggle is ignored there,
    // so the take STAYS instead of flashing.)
    function toggleVideoSourcePreview(i) {
        const v = VideoSourceListModel.getSource(i)
        const label = v.sublabel !== undefined ? v.sublabel : ""
        if (label === "")
            return
        if (LiveOutputService.inputLabel === label) {
            LiveOutputService.clearInput()
            return
        }
        if (v.kind !== "camera" && v.kind !== "screen") {
            EventBus.notify(qsTr("%1 feeds can't show in the output preview yet — camera and screen sources only.")
                                .arg(v.kind), "info", qsTr("Output preview"), "media.preview.input")
            return
        }
        LiveOutputService.takeInput(label, v.kind, v.mode !== undefined ? v.mode : "")
    }

    // Card label helpers - one lookup per roster so the delegates stay dumb.
    function cardName(i, kind) {
        if (kind === "video") {
            const v = VideoSourceListModel.getSource(i); return v.name !== undefined ? v.name : ""
        }
        if (kind === "audio") {
            const a = AudioInputListModel.getInput(i); return a.name !== undefined ? a.name : ""
        }
        const b = BusListModel.getBus(i); return b.name !== undefined ? b.name : ""
    }
    function cardSub(i, kind) {
        if (kind === "video") {
            const v = VideoSourceListModel.getSource(i)
            return (v.kind !== undefined ? v.kind : "") + (v.sublabel !== undefined && v.sublabel !== "" ? " - " + v.sublabel : "")
        }
        if (kind === "audio") {
            const a = AudioInputListModel.getInput(i)
            return (a.kind !== undefined ? a.kind : "") + (a.sublabel !== undefined && a.sublabel !== "" ? " - " + a.sublabel : "")
        }
        const b = BusListModel.getBus(i)
        return (b.type !== undefined ? b.type : "") + (b.muted !== undefined && b.muted ? " - muted" : "")
    }
}
