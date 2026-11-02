import QtQuick
import VGRPresenterUI

Item {
    id: root

    // LAYOUT PROBE (VGR_LAYOUT_TEST): the media grid's own card churn (see DesignLibraryPane).
    property int gridRowBuilds: 0
    property int gridRowDrops: 0

    // A tile was clicked / double-clicked: { type: "image"|"video"|"audio", ref: path, name }.
    signal itemActivated(var item)
    signal itemOpened(var item)

    property int modelsRev: 0
    Connections {
        target: VideoSourceListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
        function onModelReset() { root.modelsRev++ }
    }
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
        function onModelReset() { root.modelsRev++ }
    }
    Connections {
        target: BusListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
        function onModelReset() { root.modelsRev++ }
    }
    property int devicesRev: 0
    Connections {
        target: EngineBridge
        function onDevicesChanged() { root.devicesRev++ }
    }

    // The dock tab bar's Media search: only files whose name matches (the engine's search, so it
    // works by name across every folder), and the Inputs cards by name.
    property string filter: ""
    readonly property string query: filter.trim()

    // ---- what is selected ----------------------------------------------------
    property string selection: "all"   // "all" | "inputs" | a folder's path
    readonly property bool inputsMode: selection === "inputs"

    readonly property var inputRoster: ({ label: qsTr("Buses"), icon: "volume2", kind: "bus" })
    // The bus rows that match the search (all of them with no search).
    readonly property var inputRows: {
        const _ = root.modelsRev   // reactivity dependency
        const n = BusListModel.rowCount()
        const needle = root.query.toLowerCase()
        const rows = []
        for (let i = 0; i < n; ++i)
            if (needle === "" || root.cardName(i, "bus").toLowerCase().indexOf(needle) >= 0)
                rows.push(i)
        return rows
    }
    readonly property int inputTotal: {
        const _ = root.modelsRev   // reactivity dependency
        return BusListModel.rowCount()
    }

    // ---- the media grid's items ----------------------------------------------
    property var gridItems: []

    // ---- the grid's multi-selection ------------------------------------------
    property var picks: ({})
    readonly property int pickCount: Object.keys(root.picks).length
    readonly property bool multiPicked: root.pickCount > 1
    // The last plain-clicked (or Ctrl-clicked) path: where a Shift-click starts.
    property string pickAnchor: ""

    function isPicked(path) { return root.picks[String(path)] === true }
    function prunePicks() {
        const keys = Object.keys(root.picks)
        if (keys.length === 0)
            return
        const live = {}
        let changed = false
        for (let i = 0; i < root.gridItems.length; ++i) {
            const p = root.gridItems[i].path
            if (root.picks[p] === true)
                live[p] = true
        }
        for (let i = 0; i < keys.length; ++i)
            if (live[keys[i]] !== true) changed = true
        if (changed)
            root.picks = live
    }
    function clearPicks() {
        if (root.pickCount > 0)
            root.picks = ({})
        root.pickAnchor = ""
    }
    function togglePick(path) {
        const next = Object.assign({}, root.picks)
        if (next[path] === true)
            delete next[path]
        else
            next[path] = true
        root.picks = next
        root.pickAnchor = path
    }
    function extendPickTo(path) {
        const rows = root.gridItems
        let from = -1, to = -1
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].path === root.pickAnchor) from = i
            if (rows[i].path === path) to = i
        }
        if (from < 0 || to < 0) {
            root.togglePick(path)
            return
        }
        const next = Object.assign({}, root.picks)
        for (let i = Math.min(from, to); i <= Math.max(from, to); ++i)
            next[rows[i].path] = true
        root.picks = next
    }
    // The picked rows, in grid order ({ ref, name }) — what a drag carries and what
    // Delete acts on.
    function pickedItems() {
        const out = []
        for (let i = 0; i < root.gridItems.length; ++i) {
            const row = root.gridItems[i]
            if (root.picks[row.path] === true)
                out.push({ ref: row.path, name: row.name })
        }
        return out
    }
    function dragItemsFor(path) {
        if (root.pickCount > 1 && root.isPicked(path)) {
            const picked = root.pickedItems()
            if (picked.length > 0)
                return picked
        }
        return [{ ref: path, name: root.nameOfItem(path) }]
    }
    function nameOfItem(path) {
        for (let i = 0; i < root.gridItems.length; ++i)
            if (root.gridItems[i].path === path)
                return root.gridItems[i].name
        return ""
    }
    function tileClicked(path, modifiers, item) {
        if (modifiers & Qt.ControlModifier) {
            root.togglePick(path)
            return
        }
        if (modifiers & Qt.ShiftModifier) {
            root.extendPickTo(path)
            return
        }
        root.clearPicks()
        root.pickAnchor = path
        root.itemActivated(item)
    }
    // Is what is on screen still being read? (A folder's files arrive when its scan finishes, and until
    // then the grid is empty - which must not look the same as a folder with nothing in it.)
    readonly property bool loading: {
        const rows = MediaLibraryService.folderTree
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].scanning && (root.selection === "all" || root.selection === rows[i].path)) return true
        return false
    }
    // ---- AUDIO card state windows: the live level meter ------------------------
    property var meteredLabels: []
    // ---- the bus monitor's own bookkeeping: ONE VIDEO, ONE AUDIO --------------
    property int busMonitorVideoBus: -1
    property string busMonitorVideoLabel: ""
    property int busMonitorAudioBus: -1
    property var busMonitorAudioLabels: []
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
        if (want === "") {
            EventBus.notify(qsTr("This audio input has no device chosen yet — set its Source in Settings - Audio & Video, then click the card to meter it."),
                            "info", qsTr("Audio input"), "media.audio.meter.no-device")
            return
        }
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
    // ---- VIDEO/NDI card audio: the live STREAM meters ----------------------
    readonly property var videoMeterTargets: {
        void root.modelsRev
        void LiveOutputService.inputRev
        const live = []
        for (const l of LiveOutputService.cardPreviews)
            live.push(String(l).trim())
        if (LiveOutputService.inputLabel)
            live.push(String(LiveOutputService.inputLabel).trim())
        if (LiveOutputService.mediaOnAir && LiveOutputService.mediaPath)
            live.push(String(LiveOutputService.mediaPath).trim())
        const out = []
        for (let r = 0; r < VideoSourceListModel.rowCount(); ++r) {
            const v = VideoSourceListModel.getSource(r)
            if (!v || (v.kind !== "media" && v.kind !== "ndi"))
                continue
            const label = String(v.sublabel === undefined ? "" : v.sublabel).trim()
            if (label === "" || live.indexOf(label) < 0)
                continue
            out.push({ label: label, kind: v.kind })
        }
        return out
    }
    property var heldVideoMeters: []
    // Idempotent diff (see the AV screen's twin): release what left, start what
    // arrived, leave the rest decoding — a stream tap is a decoder.
    function syncVideoMeters() {
        root.heldVideoMeters = ScreenHelpers.syncVideoMeters(root.videoMeterTargets,
                                                             root.heldVideoMeters)
    }
    onVideoMeterTargetsChanged: root.syncVideoMeters()
    Component.onDestruction: {
        // Only this pane's own holds; Settings' holds are its own business.
        for (const t of root.heldVideoMeters)
            EngineBridge.stopStreamMeter(t.label)
        root.heldVideoMeters = []
        // ...and the bus monitor the pane itself opened: a tab switch must not
        // leave a WASAPI tap or a decoder running behind a card nobody sees.
        root.stopBusMonitorVideo()
        root.stopBusMonitorAudio()
    }
    function stopBusMonitorVideo() {
        if (root.busMonitorVideoBus < 0)
            return
        if (root.busMonitorVideoLabel !== ""
                && LiveOutputService.cardPreviews.indexOf(root.busMonitorVideoLabel) >= 0)
            LiveOutputService.endPreviewInput(root.busMonitorVideoLabel)
        root.busMonitorVideoBus = -1
        root.busMonitorVideoLabel = ""
    }
    function stopBusMonitorAudio() {
        for (const label of root.busMonitorAudioLabels)
            if (root.meteredLabels.indexOf(label) >= 0)
                root.toggleAudioMeter(label)
        root.busMonitorAudioBus = -1
        root.busMonitorAudioLabels = []
    }
    function toggleBusMonitorPreview(busIndex) {
        const bus = BusListModel.getBus(busIndex)
        if (!bus || Object.keys(bus).length === 0)
            return

        const type = String(bus.type || "audio").toLowerCase()
        const needsAudio = type === "audio" || type === "both" || type === "template"
        const needsVideo = type === "video" || type === "both" || type === "template"
        const audioRows = bus.routedAudioInputs || []
        const videoRows = bus.routedVideoSources || []
        if (needsVideo && videoRows.length === 0) {
            EventBus.notify(qsTr("This bus is ready for video monitoring. Add a video source and route it here in Settings - Audio & Video."),
                            "info", qsTr("Bus monitor"), "media.bus.preview.missing-video-route")
            return
        }
        if (needsAudio && audioRows.length === 0) {
            EventBus.notify(qsTr("This bus needs an audio input routed in Settings - Audio & Video before it can monitor audio."),
                            "warning", qsTr("Bus monitor"), "media.bus.preview.missing-audio-route")
            return
        }

        const audioLabels = []
        for (const row of audioRows) {
            const input = AudioInputListModel.getInput(row)
            if (!input || Object.keys(input).length === 0) continue
            const label = input.sublabel === undefined ? "" : String(input.sublabel).trim()
            if (audioLabels.indexOf(label) < 0) audioLabels.push(label)
        }
        let video = null
        let videoLabel = ""
        if (needsVideo) {
            for (const row of videoRows) {
                const candidate = VideoSourceListModel.getSource(row)
                const label = candidate && candidate.sublabel !== undefined
                    ? String(candidate.sublabel).trim() : ""
                const kind = candidate && candidate.kind !== undefined ? String(candidate.kind) : ""
                if (label === "" || ["camera", "screen", "ndi"].indexOf(kind) < 0)
                    continue
                if (!LiveOutputService.inputHealthy(label, kind))
                    continue
                video = candidate
                videoLabel = label
                break
            }
        }
        if (needsAudio && audioLabels.length === 0) {
            EventBus.notify(qsTr("This bus has no available routed audio input. Check its routes in Settings - Audio & Video."),
                            "warning", qsTr("Bus monitor"), "media.bus.preview.audio-unavailable")
            return
        }

        const meterAudio = needsAudio || audioLabels.length > 0;
        if (needsVideo && (!video || videoLabel === "")) {
            EventBus.notify(qsTr("This bus is ready to monitor video once you route an available camera, screen, or NDI source in Settings - Audio & Video."),
                            "info", qsTr("Bus monitor"), "media.bus.preview.video-unavailable")
            return
        }

        const holdsSomething = root.busMonitorAudioBus === busIndex
                               || root.busMonitorVideoBus === busIndex
        if (holdsSomething) {
            if (root.busMonitorVideoBus === busIndex) root.stopBusMonitorVideo()
            if (root.busMonitorAudioBus === busIndex) root.stopBusMonitorAudio()
            console.log("[BUSMON] released bus=" + busIndex)
            return
        }

        if (needsAudio) {
            const reasons = []
            if (bus.muted === true) reasons.push(qsTr("the bus is muted"))
            if (Number(bus.level) <= 0) reasons.push(qsTr("the bus volume is at 0"))
            const hasAudibleInput = audioRows.some((row) => {
                const input = AudioInputListModel.getInput(row)
                if (!input || input.muted || Number(input.level) <= 0) return false
                const channels = Number(input.channels) || 2
                for (let channel = 0; channel < channels; ++channel)
                    if (!Db.isChannelOff(AudioInputListModel.channelGain(row, channel))) return true
                return false
            })
            if (!hasAudibleInput) reasons.push(qsTr("every routed audio input is muted or has volume at 0"))
            if (reasons.length > 0) {
                EventBus.notify(qsTr("Audio will be silent because %1. Unmute the bus and raise the routed input volume in Settings - Audio & Video.")
                                    .arg(reasons.join(qsTr("; "))),
                                "warning", qsTr("Bus monitor"), "media.bus.preview.silent-route")
                return
            }
        } else if (needsVideo && bus.muted === true) {
            EventBus.notify(qsTr("This video bus is muted. Unmute it in Settings - Audio & Video before monitoring it."),
                            "warning", qsTr("Bus monitor"), "media.bus.preview.muted-video")
            return
        }

        if (needsVideo && root.busMonitorVideoBus !== busIndex)
            root.stopBusMonitorVideo()
        if (meterAudio && root.busMonitorAudioBus !== busIndex)
            root.stopBusMonitorAudio()

        if (meterAudio) {
            for (const label of audioLabels)
                if (root.meteredLabels.indexOf(label) < 0) root.toggleAudioMeter(label)
            root.busMonitorAudioBus = busIndex
            root.busMonitorAudioLabels = audioLabels
        }
        if (needsVideo) {
            if (LiveOutputService.cardPreviews.indexOf(videoLabel) < 0) {
                LiveOutputService.previewInput(videoLabel, video.kind,
                                               video.mode !== undefined ? video.mode : "")
                if (LiveOutputService.cardPreviews.indexOf(videoLabel) < 0) {
                    EventBus.notify(qsTr("The bus has a video source routed, but its feed did not start. Check that the source is available, then try monitoring again."),
                                    "info", qsTr("Bus monitor"), "media.bus.preview.video-start-failed")
                    return
                }
            }
            root.busMonitorVideoBus = busIndex
            root.busMonitorVideoLabel = videoLabel
        }
        console.log("[BUSMON] opened bus=" + busIndex + " type=" + type
                    + " audioTaps=" + root.busMonitorAudioLabels.length
                    + " video=" + (videoLabel === "" ? "-" : videoLabel))
    }
    function reload() {
        gridItems = inputsMode ? [] : MediaLibraryService.items(selection === "all" ? "" : selection, query)
        root.prunePicks()
    }
    onSelectionChanged: reload()
    onQueryChanged: reload()
    Component.onCompleted: {
        reload()
        root.syncVideoMeters()   // the pane's first stream-meter reconcile
    }

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
                icon: "volume2"
                label: qsTr("Buses")
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
            // Scrollbar room, plus the clock's column when it is showing: the
            // grid's own column maths then re-flows into fewer, wider cards.
            anchors.rightMargin: 10
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.gridItems

            readonly property int columns: 4
            cellWidth: Math.floor(width / columns)
            // The tile's 16:9 preview (its width less the 12 px of margins) plus the name bar.
            cellHeight: Math.round((cellWidth - 12) * 9 / 16) + 12 + 30

            delegate: MediaTile {
                required property var modelData
                // LAYOUT PROBE (VGR_LAYOUT_TEST): how many tiles the grid built, and how
                // many it threw away.
                Component.onCompleted: root.gridRowBuilds++
                Component.onDestruction: root.gridRowDrops++
                width: grid.cellWidth
                height: grid.cellHeight
                name: modelData.name
                path: modelData.path
                kind: modelData.kind
                // The file's own fill, so a tile can say it holds one (see MediaTile).
                fit: modelData.fit ?? ""
                // The click carries its modifiers: Ctrl picks, Shift extends, anything
                // else is the tile's own "show this file" gesture (see tileClicked).
                selected: root.isPicked(modelData.path)
                dragItems: root.dragItemsFor(modelData.path)
                onActivated: (modifiers) => root.tileClicked(modelData.path, modifiers,
                                                             { type: modelData.kind, ref: modelData.path, name: modelData.name })
                onOpened: root.itemOpened({ type: modelData.kind, ref: modelData.path, name: modelData.name })
                onContextMenuRequested: (source, mx, my) =>
                    root.openTileMenu(modelData.path, modelData.name, modelData.kind, source, mx, my)
            }
        }
        Rectangle {
            id: pickBar
            visible: root.pickCount > 0
            z: 4
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            width: pickRow.implicitWidth + 28
            height: 42
            radius: 21
            color: Theme.surface
            border.width: 1
            border.color: Theme.border

            Row {
                id: pickRow
                anchors.centerIn: parent
                spacing: 10
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1 selected").arg(root.pickCount)
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                AppButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Delete…")
                    variant: "danger"
                    onClicked: {
                        batchDeleteConfirm.title = qsTr("Delete these files?")
                        batchDeleteConfirm.message = qsTr("%1 files are deleted. One Undo puts them all back.")
                                                              .arg(root.pickCount)
                        batchDeleteConfirm.open()
                    }
                }
                AppButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Clear")
                    variant: "secondary"
                    onClicked: root.clearPicks()
                }
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

    // ---- Inputs: the buses --------------------------------------------------------
    Item {
        id: inputsArea
        visible: root.inputsMode
        x: sidebar.width
        width: parent.width - x
        height: parent.height

        // The buses' cards, three to a row.
        Flickable {
            id: cardsFlick
            y: 0
            // The clock's column is reserved here too, so the third card of a
            // row never slides under the time.
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
                        model: root.inputRows
                        delegate: Rectangle {
                            id: card
                            objectName: "selfTestBusCard_" + modelData
                            readonly property alias stateWinImage: stateWinImage
                            required property int modelData
                            readonly property int index: modelData
                            readonly property string kind: root.inputRoster.kind
                            readonly property bool bigCard: true   // all three tabs share the state-window card
                            readonly property bool busCard: kind === "bus"
                            readonly property bool audioCard: kind === "audio"
                            readonly property bool videoCard: kind === "video"
                            readonly property int busVideoSourceIndex: {
                                if (!busCard) return -1
                                void root.modelsRev
                                const b = BusListModel.getBus(index)
                                return b.routedVideoSources && b.routedVideoSources.length > 0
                                    ? b.routedVideoSources[0] : -1
                            }
                            readonly property var busVideoSource: {
                                if (busVideoSourceIndex < 0) return ({})
                                return VideoSourceListModel.getSource(busVideoSourceIndex)
                            }
                            readonly property string busVideoLabel: busVideoSource.sublabel !== undefined
                                ? String(busVideoSource.sublabel).trim() : ""
                            readonly property bool busVideoPreviewing: {
                                void LiveOutputService.inputRev
                                return busCard && busType !== "audio" && busVideoLabel !== ""
                                    && LiveOutputService.cardPreviews.indexOf(busVideoLabel) >= 0
                            }
                            readonly property string sub: {
                                void root.modelsRev
                                if (kind !== "video" && kind !== "audio")
                                    return ""
                                const v = kind === "video"
                                    ? VideoSourceListModel.getSource(index)
                                    : AudioInputListModel.getInput(index)
                                return v.sublabel !== undefined ? v.sublabel : ""
                            }
                            readonly property string rowKind: {
                                void root.modelsRev
                                if (kind !== "video")
                                    return kind
                                const v = VideoSourceListModel.getSource(index)
                                return v.kind !== undefined ? v.kind : ""
                            }
                            readonly property bool taken: videoCard && sub !== ""
                                                          && (LiveOutputService.inputLabel === sub
                                                              || (LiveOutputService.mediaOnAir
                                                                  && LiveOutputService.mediaPath === sub))
                            readonly property string heldVideoLabel: busCard ? busVideoLabel : sub
                            // ---- THE BUS STATUS: one fact, three states ----
                            readonly property string busStatus: {
                                void root.modelsRev
                                void root.devicesRev
                                void LiveOutputService.inputRev
                                void root.meterList.length
                                if (!busCard)
                                    return ""
                                const b = BusListModel.getBus(index)
                                const audio = b.routedAudioInputs !== undefined ? b.routedAudioInputs : []
                                const video = b.routedVideoSources !== undefined ? b.routedVideoSources : []
                                const wantsAudio = busType !== "video"
                                const wantsVideo = busType !== "audio"
                                const routedVideo = wantsVideo ? video.length : 0
                                const routedAudio = wantsAudio ? audio : []
                                if (routedVideo === 0 && routedAudio.length === 0)
                                    return "none"
                                let starved = false
                                if (routedVideo > 0) {
                                    const vkind = busVideoSource.kind !== undefined
                                        ? String(busVideoSource.kind) : ""
                                    if (LiveOutputService.inputAvailable(busVideoLabel, vkind)) {
                                        if (LiveOutputService.inputDelivery(busVideoLabel, vkind) === "starved")
                                            starved = true
                                        else
                                            return "connected"
                                    }
                                }
                                for (const r of routedAudio) {
                                    const data = AudioInputListModel.getInput(r)
                                    if (!data)
                                        continue
                                    const label = data.sublabel !== undefined ? String(data.sublabel) : ""
                                    if (LiveOutputService.inputAvailable(label, "audio")) {
                                        if (LiveOutputService.inputDelivery(label, "audio") === "starved")
                                            starved = true
                                        else
                                            return "connected"
                                    }
                                }
                                // Routed, and the device is there — but a source the
                                // app IS pulling from is handing nothing over.
                                if (starved)
                                    return "starved"
                                // Routed, but every source it names is gone.
                                return "offline"
                            }
                            readonly property color busStatusColor: busStatus === "connected" ? Theme.success
                                : (busStatus === "offline" || busStatus === "starved") ? Theme.warning
                                : Theme.dangerLight
                            readonly property string busStatusText: busStatus === "connected"
                                ? qsTr("Bus connected")
                                : busStatus === "offline"
                                  ? qsTr("Bus connected · source not available")
                                : busStatus === "starved"
                                  ? qsTr("Bus connected · source is not delivering")
                                  : qsTr("Bus not connected")

                            // ---- WHAT THE WINDOW SAYS ----------------
                            readonly property string previewFailure: {
                                void root.modelsRev
                                void LiveOutputService.inputRev
                                if (!holding || heldVideoLabel === "")
                                    return ""
                                const kind = busCard
                                    ? (busVideoSource.kind !== undefined ? String(busVideoSource.kind) : "")
                                    : rowKind
                                return LiveOutputService.inputFailure(heldVideoLabel, kind)
                            }
                            readonly property bool previewing: {
                                void LiveOutputService.inputRev
                                return videoCard && sub !== ""
                                       && LiveOutputService.cardPreviews.indexOf(sub) >= 0
                            }
                            readonly property bool metering: {
                                void root.meteredLabels
                                return audioCard && root.meteredLabels.indexOf(sub) >= 0
                            }
                            readonly property bool holding: taken || previewing || metering || busVideoPreviewing
                            // BUS: the window shows open/muted; the click
                            // toggles mute (the bus's only live truth).
                            readonly property bool busMuted: {
                                void root.modelsRev
                                const b = BusListModel.getBus(index)
                                return b.muted === true
                            }
                            readonly property string busType: {
                                void root.modelsRev
                                if (!busCard) return ""
                                const b = BusListModel.getBus(index)
                                return b.type !== undefined ? b.type : "audio"
                            }
                            readonly property var busChannelLevels: {
                                void root.meterList
                                void root.modelsRev
                                if (!busCard) return [0, 0]
                                if (root.busMonitorAudioBus !== index
                                        && root.busMonitorVideoBus !== index)
                                    return [0, 0]
                                const b = BusListModel.getBus(index)
                                const mix = [0, 0]
                                for (const r of b.routedAudioInputs) {
                                    const data = AudioInputListModel.getInput(r)
                                    if (!data || data.muted) continue
                                    const s = root.meterSnapshot(data.sublabel)
                                    if (!s || s.peaks === undefined || s.peaks.length === 0) continue
                                    const sourceLevel = Math.max(0, Math.min(1, Number(data.level || 0) / 100))
                                    for (let c = 0; c < 2; ++c) {
                                        const sourceChannel = s.peaks.length === 1 ? 0 : c
                                        if (sourceChannel >= s.peaks.length) continue
                                        const gainChannel = s.peaks.length === 1 ? 0 : c
                                        const gain = AudioInputListModel.channelGain(r, gainChannel)
                                        const level = Math.max(0, Math.min(1, s.peaks[sourceChannel]))
                                                     * gain * sourceLevel
                                        if (level > mix[c]) mix[c] = level
                                    }
                                }
                                if (card.busMuted) return [0, 0]
                                const busLevel = Math.max(0, Math.min(1, Number(b.level || 0) / 100))
                                const sideL = BusListModel.channelGain(index, 0)
                                const sideR = BusListModel.channelGain(index, 1)
                                return [mix[0] * busLevel * sideL, mix[1] * busLevel * sideR]
                            }
                            readonly property bool healthy: {
                                void root.modelsRev
                                void root.devicesRev
                                void LiveOutputService.inputRev
                                if (busCard)
                                    return !card.busVideoPreviewing
                                        || LiveOutputService.inputHealthy(card.busVideoLabel,
                                                                         card.busVideoSource.kind || "")
                                if (audioCard)
                                    return LiveOutputService.inputHealthy(sub, rowKind)
                                if (!holding && !cardHover.hovered)
                                    return true
                                return LiveOutputService.inputHealthy(sub, rowKind)
                            }
                            width: (inputCards.width - 16) / 3
                            height: 8 + (width - 16) * 9 / 16 + 6 + 32 + 8
                            radius: 6
                            color: Theme.rowBg
                            border.width: taken ? 2 : 1
                            HoverGlow {
                                anchors.fill: parent
                                radius: parent.radius
                                hovered: cardHover.hovered
                                glowColor: Theme.activeBg
                                edge: true
                                edgeColor: Qt.alpha(Theme.accent, 0.45)
                            }
                            border.color: taken ? Theme.accent : "transparent"

                            Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                            MouseArea {
                                anchors.fill: parent
                                visible: busCard
                                acceptedButtons: Qt.RightButton
                                onClicked: (mouse) => root.openBusMenu(card, index, mouse.x, mouse.y)
                            }

                            PositionHoverArea {
                                id: cardHover
                                anchors.fill: parent
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
                                        root.toggleBusMonitorPreview(index)
                                    }
                                }
                                onDoubleClicked: {
                                    if (videoCard) {
                                        root.toggleVideoSourcePreview(index)
                                    } else if (busCard && card.busVideoSourceIndex >= 0) {
                                        root.toggleVideoSourcePreview(card.busVideoSourceIndex)
                                    }
                                    if (audioCard) {
                                        if (root.meteredLabels.indexOf(sub) < 0)
                                            root.toggleAudioMeter(sub)
                                    } else if (busCard) {
                                        const b = BusListModel.getBus(index)
                                        for (const r of b.routedAudioInputs) {
                                            const data = AudioInputListModel.getInput(r)
                                            const label = data && data.sublabel !== undefined ? data.sublabel : ""
                                            if (label !== "" && root.meteredLabels.indexOf(label) < 0)
                                                root.toggleAudioMeter(label)
                                        }
                                    }
                                }
                            }

                            // ---- COMPACT row (audio / bus) ----------------
                            Rectangle {
                                visible: !bigCard
                                x: 8; y: 10
                                width: 24; height: 24
                                radius: 5
                                color: kind === "audio" ? Qt.alpha(Theme.success, 0.16) : Qt.alpha(Theme.danger, 0.16)
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: root.inputRoster.icon
                                    color: kind === "audio" ? Theme.successLight : Theme.dangerLight
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
                            Rectangle {
                                id: stateWin
                                visible: bigCard
                                x: 8; y: 8
                                width: parent.width - 16
                                height: width * 9 / 16
                                radius: 4
                                clip: true
                                color: Theme.windowBg
                                border.width: 1
                                border.color: card.taken ? Qt.alpha(Theme.accent, 0.45) : Theme.border

                                Image {
                                    id: stateWinImage
                                    anchors.fill: parent
                                    visible: {
                                        void LiveOutputService.inputRev
                                        return (videoCard && card.holding
                                                || busCard && card.busVideoPreviewing)
                                            && LiveOutputService.inputProducing(card.heldVideoLabel)
                                    }
                                    source: visible
                                        ? "image://videopreview/" + encodeURIComponent(card.heldVideoLabel)
                                          + "?n=" + LiveOutputService.inputRev : ""
                                    fillMode: Image.PreserveAspectCrop
                                    cache: false
                                    asynchronous: false
                                }
                                Item {
                                    id: busDot
                                    visible: busCard
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.topMargin: 6
                                    anchors.rightMargin: 6
                                    width: 10; height: 10

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18; height: 18; radius: 9
                                        color: "transparent"
                                        border.width: 2
                                        border.color: card.busStatusColor
                                        opacity: 0.35
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 10; height: 10; radius: 5
                                        color: card.busStatusColor
                                    }
                                    PositionHoverArea {
                                        anchors.fill: parent
                                        anchors.margins: -6
                                        showCursor: false
                                        clickEnabled: false
                                        onEntered: TipService.show(card.busStatusText, busDot)
                                        onExited: TipService.hide(busDot)
                                    }
                                    Component.onDestruction: TipService.forget(busDot)
                                }
                                Row {
                                    id: pillsRow
                                    anchors.centerIn: parent
                                    visible: audioCard && card.metering && card.healthy
                                    spacing: 20

                                    Repeater {
                                        model: {
                                            const s = root.meterSnapshot(card.sub)
                                            const n = s && s.channelCount !== undefined && s.channelCount > 0
                                                      ? s.channelCount : 2
                                            return Math.min(2, n)
                                        }

                                        // THE ONE PILL (see ChannelPill) — the very
                                        // same control the bus and video cards show.
                                        ChannelPill {
                                            required property int modelData

                                            channelIndex: modelData
                                            objectName: "selfTestAudioPill_" + card.index + "_" + modelData
                                            gain: {
                                                void root.modelsRev
                                                return AudioInputListModel.channelGain(card.index, modelData)
                                            }
                                            level: {
                                                void root.meterList
                                                void root.modelsRev
                                                const input = AudioInputListModel.getInput(card.index)
                                                if (!input || input.muted) return 0
                                                const s = root.meterSnapshot(card.sub)
                                                if (!s || s.peaks === undefined
                                                    || s.peaks.length <= modelData)
                                                    return 0
                                                const raw = Math.max(0, Math.min(1, s.peaks[modelData]))
                                                    * AudioInputListModel.channelGain(card.index, modelData)
                                                    * Math.max(0, Math.min(1, Number(input.level || 0) / 100))
                                                return Math.sqrt(raw)
                                            }
                                            onToggled: Qt.callLater(() =>
                                                AudioInputListModel.toggleChannelGain(card.index, modelData))
                                        }
                                    }
                                }
                                Item {
                                    anchors.centerIn: parent
                                visible: {
                                    void LiveOutputService.inputRev
                                    return (videoCard && card.holding
                                            || busCard && card.busVideoPreviewing)
                                        && !LiveOutputService.inputProducing(card.heldVideoLabel)
                                        && card.healthy && card.previewFailure === ""
                                }
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
                                        border.color: Theme.accent
                                    }
                                }
                                Item {
                                    anchors.centerIn: parent
                                    visible: card.previewFailure !== ""
                                    width: 34; height: 34
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 44; height: 44; radius: 22
                                        color: Qt.rgba(1, 0.42, 0.38, 0.10)
                                        border.width: 1
                                        border.color: Qt.rgba(1, 0.42, 0.38, 0.28)
                                    }
                                    IconGlyph {
                                        anchors.centerIn: parent
                                        name: root.inputRoster.icon
                                        color: Theme.textMuted
                                        fit: true; strokeWidth: 2
                                        width: 22; height: 22
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 32; height: 2.5
                                        rotation: 45
                                        radius: 1.25
                                        color: Theme.dangerLight
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 5
                                    width: parent.width - 16
                                    visible: card.previewFailure !== ""
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    text: qsTr("preview failed — %1").arg(card.previewFailure)
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily; font.pixelSize: 8
                                }
                                Item {
                                    anchors.centerIn: parent
                                    visible: !card.healthy && (card.holding || audioCard || busCard && card.busVideoPreviewing)
                                    width: 34; height: 34

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
                                        name: root.inputRoster.icon
                                        color: Theme.textMuted
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
                                        color: Theme.dangerLight
                                    }
                                    Text {
                                        anchors.top: parent.bottom
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.topMargin: 8
                                        text: audioCard
                                            ? qsTr("muted — device disconnected")
                                            : qsTr("can't reach — window closed or device gone")
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily; font.pixelSize: 9
                                    }
                                }
                                Column {
                                    anchors.centerIn: parent
                                    visible: !card.holding && !busCard && card.healthy
                                    spacing: 6
                                    IconGlyph {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        name: root.inputRoster.icon
                                        color: Theme.textMuted
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
                                        color: cardHover.hovered ? Theme.iconChrome : Theme.textMuted
                                        font.family: Theme.fontFamily; font.pixelSize: 10
                                    }
                                }
                                Row {
                                    id: busAudioRow
                                    visible: busCard
                                    readonly property bool centred: card.busType === "audio"
                                    width: implicitWidth
                                    height: implicitHeight
                                    x: centred ? (parent.width - width) / 2
                                               : parent.width - width - 6
                                    y: centred ? (parent.height - height) / 2
                                               : parent.height - height - 6
                                    spacing: 6

                                    Repeater {
                                        model: 2

                                        ChannelPill {
                                            required property int modelData

                                            channelIndex: modelData
                                            objectName: "selfTestBusPill_" + card.index + "_" + modelData
                                            gain: {
                                                void root.modelsRev
                                                return BusListModel.channelGain(card.index, modelData)
                                            }
                                            level: {
                                                const raw = Math.max(0, Math.min(1, card.busChannelLevels[modelData] || 0))
                                                return Math.sqrt(raw)   // same perceptual lift as the input cards
                                            }
                                            compact: !busAudioRow.centred
                                            onToggled: Qt.callLater(() =>
                                                BusListModel.toggleChannelGain(card.index, modelData))
                                        }
                                    }
                                }

                                Row {
                                    id: vidAudioRow
                                    visible: videoCard
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: 6
                                    spacing: 6

                                    readonly property bool audioCapable: rowKind === "media" || rowKind === "ndi"
                                    readonly property bool liveAudio: {
                                        void root.meterList
                                        if (!audioCapable || String(card.sub).trim() === "")
                                            return false
                                        const s = root.meterSnapshot(card.sub)
                                        return s !== null && s.peaks !== undefined && s.peaks.length > 0
                                    }
                                    readonly property real storedLevel: {
                                        void root.modelsRev
                                        if (!vidAudioRow.audioCapable) return 0
                                        const v = VideoSourceListModel.getSource(index)
                                        return v.level !== undefined
                                               ? Math.max(0, Math.min(100, Number(v.level))) / 100 : 0
                                    }
                                    readonly property bool storedMuted: {
                                        void root.modelsRev
                                        const v = VideoSourceListModel.getSource(index)
                                        return v.muted === true
                                    }

                                    Repeater {
                                        model: 2

                                        ChannelPill {
                                            required property int modelData

                                            channelIndex: modelData
                                            objectName: "selfTestVideoPill_" + card.index + "_" + modelData
                                            capable: vidAudioRow.audioCapable
                                            gain: {
                                                void root.modelsRev
                                                return VideoSourceListModel.channelGain(card.index, modelData)
                                            }
                                            level: {
                                                void root.meterList
                                                if (vidAudioRow.storedMuted)
                                                    return 0
                                                const s = root.meterSnapshot(card.sub)
                                                if (!s || s.peaks === undefined || s.peaks.length === 0)
                                                    return vidAudioRow.storedLevel
                                                const src = s.peaks.length === 1 ? 0 : modelData
                                                if (src >= s.peaks.length)
                                                    return vidAudioRow.storedLevel
                                                const raw = Math.max(0, Math.min(1, s.peaks[src]))
                                                            * vidAudioRow.storedLevel
                                                return Math.sqrt(raw)
                                            }
                                            // The small overlay pair (it sits over
                                            // the feed — see ChannelPill.compact).
                                            compact: true
                                            // Breathe only on a REAL signal; a
                                            // stored fader never moves on its own.
                                            scaleWithLevel: vidAudioRow.liveAudio

                                            onToggled: Qt.callLater(() =>
                                                VideoSourceListModel.toggleChannelGain(card.index, modelData))
                                        }
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
                    visible: root.inputRows.length === 0
                    width: parent.width
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: root.query !== ""
                          ? qsTr("Nothing named \u201c%1\u201d.").arg(root.query)
                          : qsTr("No buses yet — make one with the button below.")
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

        Rectangle {
            id: newBus
            objectName: "selfTestBusNew"
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            width: newBusRow.width + 32
            height: 36
            radius: 18
            color: newBusHover.hovered ? Qt.lighter(Theme.accent, 1.08) : Theme.accent

            // the soft shadow (drawn first, so it sits under the pill)
            Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: parent.height + 4; radius: 20; color: "#26000000" }
            Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: parent.height + 2; radius: 19; color: "#33000000" }

            Row {
                id: newBusRow
                anchors.centerIn: parent
                spacing: 8
                PlusGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 12; thickness: 1.6; color: Theme.controlInk
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("New Bus")
                    color: Theme.controlInk
                    font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold
                }
            }
            PositionHoverArea {
                id: newBusHover
                anchors.fill: parent
                onClicked: newBusDialog.open = true
            }
        }
    }

    BusSourceModal {
        id: newBusDialog
        objectName: "newBusPicker"
        mode: "newBus"
        onCancelled: newBusDialog.open = false
        onCreated: (busRow) => {
            // The picker reports and leaves `open` alone (see its header), so the
            // caller that opened it closes it.
            newBusDialog.open = false
            // The pane that owns the row is the roster the whole app reads, so
            // land on the list the new bus appeared in.
            root.selection = "inputs"
            void busRow
        }
    }

    // ---- Output-preview input take (the video cards' double-click) --------
    function toggleVideoSourcePreview(i) {
        const v = VideoSourceListModel.getSource(i)
        const label = v.sublabel !== undefined ? v.sublabel : ""
        if (label === "")
            return
        if (v.kind === "media") {
            if (LiveOutputService.mediaOnAir && LiveOutputService.mediaPath === label) {
                LiveOutputService.clearMedia()
                return
            }
            LiveOutputService.takeMedia(label, v.name !== undefined ? v.name : "")
            return
        }
        if (LiveOutputService.inputLabel === label) {
            LiveOutputService.clearInput()
            return
        }
        if (v.kind !== "camera" && v.kind !== "screen" && v.kind !== "ndi") {
            EventBus.notify(qsTr("%1 feeds can't show in the output preview yet — camera, screen and NDI sources only.")
                                .arg(v.kind), "info", qsTr("Output preview"), "media.preview.input")
            return
        }
        LiveOutputService.takeInput(label, v.kind, v.mode !== undefined ? v.mode : "")
    }

    // ---- a media item's own menu: rename, move, delete --------------------------------
    property string tileMenuPath: ""
    property string tileMenuName: ""
    property string tileMenuKind: ""
    property var tileMenuRow: null       // the menu's "Move to" row (the flyout's anchor)
    readonly property string moveToLabel: qsTr("Move to")
    // The row label this menu gave the flyout ("Move to" or "Move 4 files to…"), so the
    // hover that opens the flyout can recognise it whatever the count is.
    property string moveRowLabel: root.moveToLabel
    // THE FILE'S OWN FILL (see MediaLibraryService::setItemFit): "" when it follows
    // whatever the output says, and the row label names what it holds.
    property string tileMenuFit: ""
    property string fitRowLabel: qsTr("Fill")
    function fitLabel(fit) {
        if (fit === "contain") return qsTr("Contain (whole picture)")
        if (fit === "cover") return qsTr("Cover (crop to fill)")
        if (fit === "fill") return qsTr("Stretch")
        return qsTr("Output default")
    }
    property bool tileMenuMany: false

    function openFitMenu(rowItem) {
        if (!rowItem || root.tileMenuPath === "")
            return
        root.tileMenuRow = rowItem
        fitMenu.model = [
            { label: root.fitLabel("contain"), payload: "contain", trailing: root.tileMenuFit === "contain" ? "\u2713" : "" },
            { label: root.fitLabel("cover"), payload: "cover", trailing: root.tileMenuFit === "cover" ? "\u2713" : "" },
            { label: root.fitLabel("fill"), payload: "fill", trailing: root.tileMenuFit === "fill" ? "\u2713" : "" },
            { divider: true },
            { label: root.fitLabel(""), payload: "", trailing: root.tileMenuFit === "" ? "\u2713" : "" }
        ]
        fitMenu.openBeside(rowItem, root.Window.contentItem)
    }

    function openTileMenu(path, name, kind, source, mx, my) {
        moveToMenu.closeFlyout()
        fitMenu.closeFlyout()
        root.tileMenuPath = path
        root.tileMenuName = name
        root.tileMenuKind = kind
        root.tileMenuMany = root.multiPicked && root.isPicked(path)
        root.moveRowLabel = root.tileMenuMany
                           ? qsTr("Move %1 files to…").arg(root.pickCount)
                           : root.moveToLabel
        // The fill the file holds ("" = the output's default), and the row's own
        // label, which states it — the menu has to say what a click would CHANGE.
        root.tileMenuFit = MediaLibraryService.itemFit(path)
        root.fitRowLabel = qsTr("Fill: %1").arg(root.fitLabel(root.tileMenuFit))
        tileMenu.model = root.tileMenuMany
            ? [
                { label: root.moveRowLabel, trailing: "\u203a", payload: "move" },
                { divider: true },
                { label: qsTr("Delete %1 files…").arg(root.pickCount), danger: true, payload: "delete-many" }
              ]
            : [
                { label: qsTr("Rename…"), payload: "rename" },
                { label: root.fitRowLabel, trailing: "\u203a", payload: "fit" },
                { label: root.moveRowLabel, trailing: "\u203a", payload: "move" },
                { divider: true },
                { label: qsTr("Delete…"), danger: true, payload: "delete" }
              ]
        // The WINDOW, not the pane: a DropdownPanel lifts itself to window level,
        // so a pane-sized bounds rect let the menu hang over the other screens.
        tileMenu.openAt(source, mx, my, root.Window.contentItem)
    }

    function openMoveToMenu(rowItem) {
        if (!rowItem || root.tileMenuPath === "")
            return
        root.tileMenuRow = rowItem
        const current = root.folderOfItem(root.tileMenuPath)
        moveToMenu.model = MediaLibraryService.folderTree.map((row) => ({
            label: (row.depth === 0 ? "" : "    ".repeat(row.depth)) + row.name,
            payload: row.path,
            trailing: row.path === current ? "\u2713" : ""
        }))
        moveToMenu.openBeside(rowItem, root.Window.contentItem)
    }
    // The item's folder, for the tick above (the row's own `folder`, not a
    // re-derivation of the path — the library is the one that knows).
    function folderOfItem(path) {
        const rows = root.gridItems
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].path === path)
                return rows[i].folder !== undefined ? rows[i].folder : ""
        return ""
    }
    Timer {
        id: moveToGrace
        interval: 260
        onTriggered: if (!AppCursor.hovered(moveToMenu) && !AppCursor.hovered(root.tileMenuRow))
                         moveToMenu.closeFlyout()
    }
    Timer {
        id: fitGrace
        interval: 260
        onTriggered: if (!AppCursor.hovered(fitMenu) && !AppCursor.hovered(root.tileMenuRow))
                         fitMenu.closeFlyout()
    }
    MenuCatcher { menu: tileMenu }
    DropdownPanel {
        id: tileMenu
        objectName: "selfTestMediaItemMenu"
        visible: false
        z: 25
        fitContentWidth: true
        maxHeight: 320
        onItemHovered: (label, hovering, rowItem) => {
            if (label === root.moveRowLabel) {
                if (hovering) {
                    moveToGrace.stop()
                    root.openMoveToMenu(rowItem)
                } else {
                    moveToGrace.restart()
                }
            } else {
                moveToGrace.stop()
                moveToMenu.closeFlyout()
            }
            // The FILL row opens its own flyout the same way Move to does: four
            // choices on the file itself, with the one it holds ticked.
            if (label === root.fitRowLabel) {
                if (hovering) {
                    fitGrace.stop()
                    root.openFitMenu(rowItem)
                } else {
                    fitGrace.restart()
                }
            } else {
                fitGrace.stop()
                fitMenu.closeFlyout()
            }
        }
        // A row that carries a payload reports through itemPicked (see
        // DropdownPanel), so every row of this menu comes here.
        onItemPicked: (index, payload) => {
            void index
            moveToMenu.closeFlyout()
            fitMenu.closeFlyout()
            tileMenu.visible = false
            if (payload === "rename") {
                itemNameDialog.title = qsTr("Rename file")
                itemNameDialog.placeholder = qsTr("File name")
                itemNameDialog.confirmLabel = qsTr("Rename")
                itemNameDialog.open(root.tileMenuName)
            } else if (payload === "delete") {
                itemDeleteConfirm.title = qsTr("Delete this file?")
                itemDeleteConfirm.message = qsTr("\u201c%1\u201d is deleted. An Undo appears for a few seconds afterwards.")
                                                .arg(root.tileMenuName)
                itemDeleteConfirm.open()
            } else if (payload === "delete-many") {
                batchDeleteConfirm.title = qsTr("Delete these files?")
                batchDeleteConfirm.message = qsTr("%1 files are deleted. One Undo puts them all back.")
                                                      .arg(root.pickCount)
                batchDeleteConfirm.open()
            }
        }
    }
    DropdownPanel {
        id: moveToMenu
        objectName: "selfTestMediaItemMoveMenu"
        visible: false
        z: 26
        fitContentWidth: true
        maxHeight: 320
        onItemPicked: (index, payload) => {
            void index
            moveToMenu.closeFlyout()
            tileMenu.visible = false
            if (payload === undefined || payload === "")
                return
            if (root.tileMenuMany) {
                const picked = root.pickedItems()
                root.clearPicks()
                for (let i = 0; i < picked.length; ++i)
                    MediaLibraryService.moveItem(picked[i].ref, payload)
                return
            }
            MediaLibraryService.moveItem(root.tileMenuPath, payload)
        }
    }
    DropdownPanel {
        id: fitMenu
        objectName: "selfTestMediaFitMenu"
        visible: false
        z: 26
        fitContentWidth: true
        maxHeight: 320
        onItemPicked: (index, payload) => {
            void index
            fitMenu.closeFlyout()
            tileMenu.visible = false
            if (payload === undefined)
                return
            const chosen = String(payload)
            if (LiveOutputService.mediaOnAir && LiveOutputService.mediaPath === root.tileMenuPath)
                LiveOutputService.setMediaFit(chosen)
            else
                MediaLibraryService.setItemFit(root.tileMenuPath, chosen)
        }
    }
    // The flyouts go when their menu does, and when this tab does.
    Connections {
        target: tileMenu
        function onVisibleChanged() {
            if (tileMenu.visible)
                return
            moveToMenu.closeFlyout()
            fitMenu.closeFlyout()
        }
    }
    onVisibleChanged: if (!visible) tileMenu.visible = false
    NameDialog {
        id: itemNameDialog
        z: 30000
        onAccepted: (text) => {
            itemNameDialog.close()
            if (text !== "" && text !== root.tileMenuName)
                MediaLibraryService.renameItem(root.tileMenuPath, text)
        }
        onDismissed: itemNameDialog.close()
    }
    ConfirmDialog {
        id: itemDeleteConfirm
        z: 30000
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            itemDeleteConfirm.close()
            MediaLibraryService.removeItem(root.tileMenuPath)
        }
        onDismissed: itemDeleteConfirm.close()
    }
    // The selection's delete: ONE confirm, ONE service call, ONE notice (and one Undo
    // that puts the whole batch back — see MediaLibraryService::removeItems).
    ConfirmDialog {
        id: batchDeleteConfirm
        z: 30000
        confirmLabel: qsTr("Delete all")
        onConfirmed: {
            batchDeleteConfirm.close()
            const picked = root.pickedItems().map((p) => p.ref)
            root.clearPicks()
            MediaLibraryService.removeItems(picked)
        }
        onDismissed: batchDeleteConfirm.close()
    }

    // ---- a bus's own menu: rename, duplicate, remove ----------------------------------
    property int busMenuId: -1
    property string busMenuName: ""
    function openBusMenu(source, index, mx, my) {
        if (!source || index < 0)
            return
        const b = BusListModel.getBus(index)
        root.busMenuId = b.id !== undefined ? Number(b.id) : -1
        root.busMenuName = b.name !== undefined ? String(b.name) : ""
        busMenu.model = [
            { label: qsTr("Rename…"), payload: "rename" },
            { label: qsTr("Duplicate"), payload: "duplicate" },
            { divider: true },
            { label: qsTr("Delete…"), danger: true, payload: "delete" }
        ]
        busMenu.openAt(source, mx, my, root.Window.contentItem)
    }
    function rowOfBusId(id) {
        void root.modelsRev
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) {
            const b = BusListModel.getBus(i)
            if (b.id !== undefined && Number(b.id) === Number(id))
                return i
        }
        return -1
    }
    MenuCatcher { menu: busMenu }
    DropdownPanel {
        id: busMenu
        objectName: "selfTestBusCardMenu"
        visible: false
        z: 25
        fitContentWidth: true
        maxHeight: 320
        onItemPicked: (index, payload) => {
            void index
            busMenu.visible = false
            const row = root.rowOfBusId(root.busMenuId)
            if (row < 0)
                return
            if (payload === "rename") {
                busNameDialog.open(root.busMenuName)
            } else if (payload === "duplicate") {
                BusListModel.duplicateBus(row)
            } else if (payload === "delete") {
                busDeleteConfirm.message = qsTr("\u201c%1\u201d is removed, and any slide using it loses its feed. An Undo appears for a few seconds afterwards.")
                                                .arg(root.busMenuName)
                busDeleteConfirm.open()
            }
        }
    }
    NameDialog {
        id: busNameDialog
        z: 30000
        title: qsTr("Rename bus")
        placeholder: qsTr("Bus name")
        confirmLabel: qsTr("Rename")
        onAccepted: (text) => {
            busNameDialog.close()
            const row = root.rowOfBusId(root.busMenuId)
            if (row >= 0 && text !== "")
                BusListModel.renameBus(row, text)
        }
        onDismissed: busNameDialog.close()
    }
    ConfirmDialog {
        id: busDeleteConfirm
        z: 30000
        title: qsTr("Delete this bus?")
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            busDeleteConfirm.close()
            const row = root.rowOfBusId(root.busMenuId)
            if (row >= 0)
                BusListModel.deleteBus(row)   // the operator's delete: its notice offers the Undo
        }
        onDismissed: busDeleteConfirm.close()
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