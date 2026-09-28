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
    // ---- AUDIO card state windows: the live level meter ------------------------
    // One click meters that device (the SAME WASAPI tap API the AV dialogs
    // use — startInputMeter/stopInputMeter); the card's window then renders
    // the device's peaks from the bridge's published snapshot list. Re-click
    // releases the tap.
    property var meteredLabels: []
    // Refreshed by the bridge's ~20 Hz meter pump while any tap is live.
    property var meterList: []
    Connections {
        target: EngineBridge
        function onInputLevelsChanged() { root.meterList = EngineBridge.inputMeterList() }
        Component.onCompleted: root.meterList = EngineBridge.inputMeterList()
    }
    function meterSnapshot(label) {
        const want = label === undefined ? "" : String(label).trim()
        for (let i = 0; i < root.meterList.length; ++i) {
            const s = root.meterList[i]
            if ((s.label || "") === want)
                return s
        }
        return null
    }
    function toggleAudioMeter(label) {
        const want = label === undefined ? "" : String(label).trim()
        const i = root.meteredLabels.indexOf(want)
        if (i >= 0) {
            const next = root.meteredLabels.slice()
            next.splice(i, 1)
            root.meteredLabels = next
            EngineBridge.stopInputMeter(want)
            return
        }
        root.meteredLabels = root.meteredLabels.concat([want])
        EngineBridge.startInputMeter(want)
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
                            // glyph when the source is not taken. AUDIO cards
                            // meter the device live (one click starts the
                            // WASAPI tap); BUS cards show open/muted. The name
                            // sits below the window on every big card.
                            readonly property bool bigCard: true   // all three tabs share the state-window card
                            readonly property bool busCard: kind === "bus"
                            readonly property bool audioCard: kind === "audio"
                            readonly property bool videoCard: kind === "video"
                            readonly property string sub: {
                                void root.modelsRev
                                if (kind !== "video" && kind !== "audio")
                                    return ""
                                const v = kind === "video"
                                    ? VideoSourceListModel.getSource(index)
                                    : AudioInputListModel.getInput(index)
                                return v.sublabel !== undefined ? v.sublabel : ""
                            }
                            // The ROW's real device kind ("screen"/"camera" on
                            // the video tab) — the TAB kind here is just
                            // "video", which the reachability probe (and the
                            // bridge's resolvers) cannot answer; probing with
                            // it slashed the icon on every click even though
                            // the take had started. Audio rows ARE "audio".
                            readonly property string rowKind: {
                                void root.modelsRev
                                if (kind !== "video")
                                    return kind
                                const v = VideoSourceListModel.getSource(index)
                                return v.kind !== undefined ? v.kind : ""
                            }
                            // The TAKEN source's card wears the accent —
                            // "which one is on the output preview" reads
                            // from the pane too. (Audio devices and buses are
                            // sound-only: there is no output-preview layer to
                            // take — their state window is the live METER.)
                            readonly property bool taken: videoCard && sub !== ""
                                                          && LiveOutputService.inputLabel === sub
                            // INTERNAL PREVIEW: the ONE-CLICK thumbnail (owner
                            // "card", this card's own state window) — separate
                            // from the output take. Read through inputRev so
                            // the membership re-evaluates at the pump's rate.
                            readonly property bool previewing: {
                                void LiveOutputService.inputRev
                                return videoCard && sub !== ""
                                       && LiveOutputService.cardPreviews.indexOf(sub) >= 0
                            }
                            // AUDIO: one-click live metering (the card's own
                            // window shows the device's peaks; an empty sublabel
                            // meters the DEFAULT input, same as the AV dialogs).
                            readonly property bool metering: {
                                void root.meteredLabels
                                return audioCard && root.meteredLabels.indexOf(sub) >= 0
                            }
                            readonly property bool holding: taken || previewing || metering
                            // BUS: the window shows open/muted; the click
                            // toggles mute (the bus's only live truth).
                            readonly property bool busMuted: {
                                void root.modelsRev
                                const b = BusListModel.getBus(index)
                                return b.muted === true
                            }
                            // REACHABILITY: a closed window / unplugged device
                            // flips this false (slash over the centre icon).
                            // Probed only while holding (the pump re-evaluates)
                            // or hovered — an idle, unhovered card shows idle
                            // art without probing (camera enumeration is not
                            // free). inputRev keeps held cards current. Buses
                            // are graph nodes — always reachable.
                            readonly property bool healthy: {
                                void root.modelsRev
                                void LiveOutputService.inputRev
                                if (busCard)
                                    return true
                                // Audio reachability is a lookup against the ALREADY-
                                // CACHED device roster (see EngineBridge::
                                // inputSourceReachable's "audio" branch) — no live
                                // enumeration, unlike video/camera. Cheap enough to
                                // check unconditionally, so an idle (never-clicked)
                                // audio card can still show "disconnected" instead of
                                // a misleadingly neutral idle glyph when its device is
                                // unplugged or removed.
                                if (audioCard)
                                    return LiveOutputService.inputHealthy(sub, rowKind)
                                if (!holding && !cardHover.hovered)
                                    return true
                                return LiveOutputService.inputHealthy(sub, rowKind)
                            }
                            width: (inputCards.width - 16) / 3
                            height: 8 + (width - 16) * 9 / 16 + 6 + 32 + 8
                            radius: 6
                            color: cardHover.hovered ? "#1e1f28" : "#16171e"
                            border.width: taken ? 2 : 1
                            border.color: taken ? "#6c5ce7" : "transparent"

                            PositionHoverArea {
                                id: cardHover
                                anchors.fill: parent
                                // ONE CLICK = THE CARD'S OWN STATE WINDOW:
                                //   video → live thumbnail (owner "card")
                                //   audio → live level meter (the WASAPI tap)
                                //   bus   → mute toggle (the bus's live truth)
                                // Re-click releases/re-toggles. DOUBLE-CLICK on a
                                // VIDEO card = OUTPUT PREVIEW (the deliberate
                                // take, purple border) — Qt delivers the first
                                // click on the way, so a double-click previews
                                // AND takes, reading as one motion.
                                onClicked: {
                                    if (videoCard) {
                                        const v = VideoSourceListModel.getSource(index)
                                        const label = v.sublabel !== undefined ? v.sublabel : ""
                                        if (label === "")
                                            return
                                        if (LiveOutputService.cardPreviews.indexOf(label) >= 0)
                                            LiveOutputService.endPreviewInput(label)
                                        else
                                            LiveOutputService.previewInput(label, v.kind,
                                                                           v.mode !== undefined ? v.mode : "")
                                    } else if (audioCard) {
                                        root.toggleAudioMeter(sub)
                                    } else if (busCard) {
                                        BusListModel.setMuted(index, !card.busMuted)
                                    }
                                }
                                onDoubleClicked: {
                                    if (videoCard)
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

                                // RECEIVING (video): the live feed (the same
                                // provider frames the monitor tile shows — the
                                // tap runs under EITHER hold: the card preview
                                // or the output take; the pump's rev re-fetches
                                // at ~15 fps).
                                Image {
                                    anchors.fill: parent
                                    visible: videoCard && card.holding && LiveOutputService.inputProducing(card.sub)
                                    source: visible
                                        ? "image://videopreview/" + encodeURIComponent(card.sub)
                                          + "?n=" + LiveOutputService.inputRev : ""
                                    fillMode: Image.PreserveAspectCrop
                                    cache: false
                                    asynchronous: false
                                }
                                // RECEIVING (audio): the L / R BUTTONS — one
                                // capsule per channel that GLOWS with its own
                                // live level and doubles as a channel-isolate
                                // toggle (click L or R to highlight just that
                                // channel; click again to clear). Replaces the
                                // old pill-bar + separate text-label-below pair,
                                // which drifted out of alignment under the bars
                                // and didn't read as interactive — the letter now
                                // lives INSIDE the capsule so it can't drift, and
                                // the pair is centered as one unit.
                                Row {
                                    id: pillsRow
                                    anchors.centerIn: parent
                                    visible: audioCard && card.metering && card.healthy
                                    spacing: 18
                                    // Local UI state — which channel (if any) is
                                    // highlighted; -1 = none. Resets naturally
                                    // when the card is recreated (model reset).
                                    property int soloChannel: -1

                                    Repeater {
                                        // The classic L/R pair: one capsule per
                                        // side, from the snapshot's real channel
                                        // count (mono collapses to just L).
                                        model: {
                                            const s = root.meterSnapshot(card.sub)
                                            const n = s && s.channelCount !== undefined && s.channelCount > 0
                                                      ? s.channelCount : 2
                                            return Math.min(2, n)
                                        }

                                        Item {
                                            id: pill
                                            required property int modelData
                                            readonly property string chLabel: pill.modelData === 0 ? "L" : "R"
                                            readonly property bool soloed: pillsRow.soloChannel === pill.modelData
                                            width: 24
                                            height: stateWin.height - 30
                                            readonly property real level: {
                                                void root.meterList
                                                const s = root.meterSnapshot(card.sub)
                                                if (!s || s.peaks === undefined
                                                    || s.peaks.length <= modelData)
                                                    return 0
                                                // PERCEPTUAL CURVE (sqrt): a mic's
                                                // silence baseline (~1.5% peak) is
                                                // invisible on a linear scale — sqrt
                                                // lifts it to ~12% (a clear glowing
                                                // nub that jumps when you speak) while
                                                // hot signals still reach the top.
                                                const raw = Math.max(0, Math.min(1, s.peaks[modelData]))
                                                return Math.sqrt(raw)
                                            }
                                            readonly property bool active: pill.level > 0.1

                                            // Soft glow halo — the "premium" touch: a
                                            // wider, dim corona that brightens with
                                            // level, behind the capsule itself.
                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: parent.width + 12
                                                height: parent.height + 12
                                                radius: width / 2
                                                color: "transparent"
                                                border.width: 5
                                                border.color: "#6fe0a0"
                                                opacity: pill.level * 0.30
                                            }

                                            // The capsule track — visible against
                                            // the dark window; brightens at the rim
                                            // when soloed.
                                            Rectangle {
                                                anchors.fill: parent
                                                radius: 12
                                                color: "#171922"
                                                border.width: pill.soloed ? 2 : 1
                                                border.color: pill.soloed ? "#6fe0a0"
                                                            : pill.active ? "#454964" : "#2c2e3c"
                                                Behavior on border.color { ColorAnimation { duration: 120 } }
                                            }
                                            // The FILL: bottom-anchored window,
                                            // height = level, over a full-height
                                            // gradient — hot signals reach the
                                            // capsule's red top, quiet ones stay
                                            // green at the bottom.
                                            Item {
                                                anchors.bottom: parent.bottom
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                width: parent.width - 4
                                                height: Math.max(9, (parent.height - 4) * parent.level)
                                                clip: true
                                                Behavior on height { NumberAnimation { duration: 60 } }
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: pill.height - 4
                                                    radius: 10
                                                    gradient: Gradient {
                                                        orientation: Gradient.Vertical
                                                        GradientStop { position: 0.0; color: "#ff6b61" }
                                                        GradientStop { position: 0.28; color: "#ff6b61" }
                                                        GradientStop { position: 0.45; color: "#ffd166" }
                                                        GradientStop { position: 0.72; color: "#6fe0a0" }
                                                        GradientStop { position: 1.0; color: "#4ec98a" }
                                                    }
                                                }
                                            }
                                            // The channel letter — fused to the
                                            // capsule's foot on a small dark chip so
                                            // it reads clearly whether the fill is
                                            // under it or not; can't drift out of
                                            // alignment since it's part of the button.
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                anchors.bottomMargin: 3
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                width: chLabelText.implicitWidth + 8
                                                height: 13
                                                radius: 6
                                                color: Qt.rgba(0.04, 0.05, 0.08, 0.55)
                                                Text {
                                                    id: chLabelText
                                                    anchors.centerIn: parent
                                                    text: pill.chLabel
                                                    color: pill.soloed ? "#6fe0a0" : "#e9ebf5"
                                                    font.family: Theme.fontFamily
                                                    font.pixelSize: 9; font.bold: true
                                                }
                                            }
                                            Behavior on scale { NumberAnimation { duration: 80 } }
                                            scale: 1 + pill.level * 0.04

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: pillsRow.soloChannel =
                                                    (pill.soloed ? -1 : pill.modelData)
                                            }
                                        }
                                    }
                                }
                                // CONNECTING (video only; audio's meter shows
                                // silence as flat bars, which IS its truth):
                                // held, source reachable, frames not flowing yet
                                // (tap warm-up) — pulsing ring.
                                Item {
                                    anchors.centerIn: parent
                                    visible: videoCard && card.holding
                                             && !LiveOutputService.inputProducing(card.sub) && card.healthy
                                    width: 34; height: 34
                                    SequentialAnimation on scale {
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 0.85; to: 1.1; duration: 700 }
                                        NumberAnimation { from: 1.1; to: 0.85; duration: 700 }
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 26; height: 26; radius: 13
                                        color: "transparent"
                                        border.width: 2.2
                                        border.color: "#6c5ce7"
                                    }
                                }
                                // UNREACHABLE: the source can't be resolved — the
                                // CENTRE ICON with a SLASH over it (originally "let
                                // the icon in middle show / over the icon when I
                                // can't connect or reach the source"). For AUDIO
                                // this now also covers the IDLE case (never
                                // clicked) — reachability there is a cheap cached-
                                // roster lookup (see the `healthy` property), so a
                                // disconnected mic reads as disconnected even before
                                // the card's ever been clicked, styled as a muted
                                // mic rather than the generic "idle" glyph.
                                Item {
                                    anchors.centerIn: parent
                                    visible: !card.healthy && (card.holding || audioCard)
                                    width: 34; height: 34

                                    // Soft tinted backdrop — the "premium" pass:
                                    // a plain thin slash on a bare icon read as an
                                    // afterthought next to the glowing L/R buttons.
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 44; height: 44
                                        radius: 22
                                        color: Qt.rgba(1, 0.42, 0.38, 0.10)
                                        border.width: 1
                                        border.color: Qt.rgba(1, 0.42, 0.38, 0.28)
                                    }
                                    IconGlyph {
                                        anchors.centerIn: parent
                                        name: root.inputTabs[root.inputTab].icon
                                        color: "#6a6f82"
                                        fit: true; strokeWidth: 2
                                        width: 22; height: 22
                                    }
                                    // The slash: a clean red diagonal capsule,
                                    // ends just past the backdrop's circle.
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 32
                                        height: 2.5
                                        rotation: 45
                                        radius: 1.25
                                        color: "#ff6b61"
                                    }
                                    Text {
                                        anchors.top: parent.bottom
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.topMargin: 8
                                        text: audioCard
                                            ? qsTr("muted — device disconnected")
                                            : qsTr("can't reach — window closed or device gone")
                                        color: "#8a90a5"
                                        font.family: Theme.fontFamily; font.pixelSize: 9
                                    }
                                }
                                // IDLE (video/audio): nothing held (and, for audio,
                                // the device is actually reachable) — dim glyph;
                                // hover spells the gestures (one click = the
                                // card's own preview/meter, double-click =
                                // output take on video).
                                Column {
                                    anchors.centerIn: parent
                                    visible: !card.holding && !busCard && card.healthy
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
                                            ? (videoCard
                                               ? qsTr("click = preview here · double-click = output")
                                               : qsTr("click = live meter"))
                                            : qsTr("idle")
                                        color: cardHover.hovered ? "#a9b6d8" : "#565c72"
                                        font.family: Theme.fontFamily; font.pixelSize: 10
                                    }
                                }
                                // BUS STATE: a bus's live truth is its gain —
                                // the window shows the level as a bar (0..100 →
                                // the engine's -60..0 dB) with OPEN/MUTED.
                                Column {
                                    anchors.centerIn: parent
                                    visible: busCard
                                    width: parent.width - 48
                                    spacing: 8
                                    Item {
                                        width: parent.width
                                        height: 10
                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 5
                                            color: "#1c1e29"
                                        }
                                        Rectangle {
                                            width: {
                                                void root.modelsRev
                                                const b = BusListModel.getBus(index)
                                                const lv = b.level !== undefined ? Number(b.level) : 0
                                                return Math.max(0, Math.min(100, lv)) / 100 * parent.width
                                            }
                                            height: parent.height
                                            radius: 5
                                            color: card.busMuted ? "#5a5f72" : "#6fe0a0"
                                        }
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: card.busMuted
                                              ? qsTr("MUTED — click to open")
                                              : qsTr("OPEN — click to mute")
                                        color: card.busMuted ? "#ff8d7f" : "#9aa0b5"
                                        font.family: Theme.fontFamily; font.pixelSize: 10; font.bold: card.busMuted
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

    // ---- Output-preview input take (the video cards' double-click) --------
    // The service owns the PAL tap and the tile layer; this is the pane's
    // thin toggle: taken-again clears, unsupported kinds toast honestly.
    // (Activation is the DOUBLE-CLICK — a stray single click only toggles
    // the card's own internal preview/meter, never the output layer.)
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
