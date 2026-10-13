import QtQuick
import VGRPresenterUI

// The Media library tab: a resizable sidebar (All / Inputs / your folders) and a content area.
//
//   All, and each folder .... a grid of the images and videos MediaLibraryService found in the
//                             folders the user added (Add folder = import), with previews.
//   Inputs .................. the BUSES: one card per bus, showing its live mix and taking the
//                             monitor gesture, from BusListModel itself. A bus is where the
//                             sources are mixed, so this is the roster the Inputs section is
//                             about; the audio-input and video-source rosters stay on the
//                             Settings - Audio & Video board that owns them.
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
    // THE DEVICE ROSTERS' OWN REVISION — the missing half of the reactivity
    // above. A binding that asks "is the device this row names still there?"
    // (the cards' slash state, THE BUS STATUS LIGHT) reads plain Q_INVOKABLE
    // methods, which QML does not track: without a dependency on the bridge's
    // own re-enumerations such a binding keeps whatever answer it got when it
    // was first evaluated. That is why a bus light sat amber for a whole session
    // with every device present and the engine's rosters full — the sweep that
    // knew the answer landed and nobody was listening. EngineBridge announces
    // every roster update with devicesChanged (boot, a picker, a hot-plug, the
    // status light's own ping), so this counter is the honest dependency.
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

    // ONE roster, not a tab row: the buses. (Video sources / Audio inputs / Buses
    // used to be sibling tabs here, which made the same rosters live in two places
    // — this pane and Settings - Audio & Video — and read as if the Media pane
    // owned the devices. A bus is the thing this section is for: it is where the
    // sources are mixed, and monitoring it is the gesture that belongs beside the
    // media grid.)
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
    // ---- the bus monitor's own bookkeeping: ONE VIDEO, ONE AUDIO --------------
    // "only video bus can be active … if another bus is selected the prev goes
    // inactive for the new one, same for the audio route — only one audio can
    // open". A monitor owns real resources (a WASAPI tap per routed audio input,
    // a decoder for the routed video), so a new request of a kind RELEASES the
    // previous bus's of that same kind instead of stacking decoders the user can
    // no longer see. Recorded, not re-derived: the routes can be edited while a
    // monitor is open, and a release has to stop exactly what was started.
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
    // Same green→amber→red language the meters use elsewhere this session,
    // sampled as a single COLOR at a given level (0..1) rather than a
    // gradient fill — the L/R channel buttons glow with this instead of
    // showing a partial-height bar.
    function levelGlowColor(level, alpha) {
        const t = Math.max(0, Math.min(1, level))
        const stops = [
            { r: 0.29, g: 0.88, b: 0.63 },   // #4ade80-ish green (quiet)
            { r: 1.00, g: 0.82, b: 0.40 },   // amber (mid)
            { r: 1.00, g: 0.42, b: 0.38 }    // red (hot)
        ]
        const seg = t < 0.5 ? 0 : 1
        const k = t < 0.5 ? t / 0.5 : (t - 0.5) / 0.5
        const a = stops[seg], b = stops[seg + 1]
        return Qt.rgba(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k,
                       a.b + (b.b - a.b) * k, alpha === undefined ? 1 : alpha)
    }
    function toggleAudioMeter(label) {
        const want = label === undefined ? "" : String(label).trim()
        // NO DEVICE, NO TAP. This used to hand the bridge an empty label, which
        // it read as "the default input" — so one click on an audio card whose
        // row names no device (the Add dialog can create one: pick nothing and
        // press Add Source) opened a REAL capture session on the machine's
        // default microphone, lit the MonitorWall's audio button and animated
        // every strip that reads a snapshot (user call: "it is animating
        // without any device selected … so the WASAPI is being opened"). Say so
        // instead: an input with no device has nothing to meter.
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
    // NOTE (deliberate): this pane does NOT meter anything by itself. An
    // "every device row meters itself" version was tried and reverted on the
    // user's call: it opened a WASAPI capture session at startup with nothing
    // selected, lit the MonitorWall's audio button (which reads
    // anyInputMetering) and animated the dialogs' channel strips off the
    // DEFAULT input. A tap belongs to what the user actually turned on — the
    // card click below, a bus double-click, the Settings board's own rows.
    // What was broken is the tap REQUEST: a label requested before the (async)
    // device enumeration landed used to stay dead for the whole session, which
    // is why a clicked card often never animated. The bridge re-resolves every
    // requested tap after each enumeration now (EngineBridge::enumerateDevices'
    // callback), so the click below genuinely starts a working meter.
    //
    // ---- VIDEO/NDI card audio: the live STREAM meters ----------------------
    // A media file or an NDI card carries its own audio, which no capture device
    // can reach: the bridge's stream tap is its carrier, and it publishes in the
    // SAME inputLevels list under the row's sublabel. The pane holds a tap while
    // the card is LIVE here (its internal preview) or ON AIR (taken by an
    // output) — the identical "a tap belongs to what the user turned on" rule
    // the audio cards follow, so an idle roster decodes nothing.
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
        const wanted = root.videoMeterTargets
        const held = root.heldVideoMeters
        const has = (list, t) => list.some(x => x.label === t.label && x.kind === t.kind)
        for (const t of held)
            if (!has(wanted, t)) EngineBridge.stopStreamMeter(t.label)
        for (const t of wanted)
            if (!has(held, t)) EngineBridge.startStreamMeter(t.label, t.kind, t.label)
        root.heldVideoMeters = wanted
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
    // The two releases the one-video/one-audio rule is made of. Each stops
    // exactly what this pane started (the recorded label / labels) and forgets
    // it, so a release can never touch a tap or a preview someone else owns.
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
        const needsAudio = type === "audio" || type === "both"
        const needsVideo = type === "video" || type === "both"
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

        // THE ROUTED AUDIO IS METERED WHOEVER THE BUS FEEDS, so this one flag is
        // read by the release below AND the start further down — declared HERE,
        // after the labels are known and before either uses it (it used to be
        // declared at the start site, so the release above it read an undefined
        // name and every monitor click died with "meterAudio is not defined").
        // Opening a bus monitor opens the taps for the audio routed into THIS bus,
        // whatever the bus's type: the card's L/R pair reads that mix, so a video
        // bus with an input routed in — a real configuration, and the one behind
        // the "has audio connected and the bottom-right pair is not glowing"
        // report — showed a pair nothing could light. A tap belongs to what the
        // user turned on, and they turned this bus on.
        const meterAudio = needsAudio || audioLabels.length > 0;
        if (needsVideo && (!video || videoLabel === "")) {
            EventBus.notify(qsTr("This bus is ready to monitor video once you route an available camera, screen, or NDI source in Settings - Audio & Video."),
                            "info", qsTr("Bus monitor"), "media.bus.preview.video-unavailable")
            return
        }

        // "Is THIS bus's monitor the open one?" — not "is anything previewing".
        // Another bus monitoring video must not make this bus read as active, and
        // a click here has to mean "open mine", never "close mine because someone
        // else's is on".
        // "Is THIS bus's monitor the open one?" — asked of the BOOKKEEPING, not of
        // the type: a second click must close exactly what this bus opened, and a
        // video bus can hold audio taps too now (see the start block), so a pair
        // that started only partly still closes in one click.
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

        // THE NEW MONITOR REPLACES THE OLD ONE, per kind: the previous video bus
        // stops previewing and the previous audio bus's taps stop BEFORE this
        // bus's start, so exactly one video and one audio monitor is ever open.
        // (Ordering matters: releasing first means the new decoder is the only one
        // left, and a start that then fails leaves nothing of the old one running.)
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
        // One line per open: which bus, which taps it took, which feed it shows.
        // "The pair is not glowing" and "nothing is metered" look identical from
        // the card, and this is the difference (the pane's taps live in
        // `meteredLabels`; nothing else in the app can say why a pill is flat).
        console.log("[BUSMON] opened bus=" + busIndex + " type=" + type
                    + " audioTaps=" + root.busMonitorAudioLabels.length
                    + " video=" + (videoLabel === "" ? "-" : videoLabel))
    }
    function reload() {
        gridItems = inputsMode ? [] : MediaLibraryService.items(selection === "all" ? "" : selection, query)
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
                // The TAKE pill: this file on/off the monitor wall's media
                // layer (videos + images; audio refuses honestly).
                onTakeToggled: {
                    if (LiveOutputService.mediaOnAir && LiveOutputService.mediaPath === modelData.path)
                        LiveOutputService.clearMedia()
                    else
                        LiveOutputService.takeMedia(modelData.path, modelData.name)
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
                            // Test hook (see tools/selftest/run_bus_light_test.sh):
                            // the bus status light is drawn from this card's own
                            // properties, so a scenario has to reach the card to
                            // read the state the operator sees.
                            objectName: "selfTestBusCard_" + modelData
                            // Test hook (see tools/selftest/run_bus_light_test.sh):
                            // the light's live half can only see frames while this
                            // Image is actually ASKING the provider for them, so a
                            // scenario reading the light has to be able to tell
                            // "nothing is watching" from "watching and empty".
                            readonly property alias stateWinImage: stateWinImage
                            required property int modelData
                            readonly property int index: modelData
                            readonly property string kind: root.inputRoster.kind
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
                                                          && (LiveOutputService.inputLabel === sub
                                                              || (LiveOutputService.mediaOnAir
                                                                  && LiveOutputService.mediaPath === sub))
                            // The label of the video tap THIS card holds: the first
                            // routed video source on a bus card, the card's own
                            // source otherwise. Named once — the state window's
                            // feed, the connecting ring and the failure line all
                            // ask about the same tap.
                            readonly property string heldVideoLabel: busCard ? busVideoLabel : sub
                            // ---- THE BUS STATUS: one fact, three states ----
                            //
                            // The user's own words: "the green dot that indicate the
                            // bus has a connected source that is active — green
                            // connect, but the source is not active yellow, not
                            // connected red", and then the correction that settles
                            // what "active" means here: "it should be green because
                            // it's connected and there is audio, but it [is] not
                            // just playing because of [the] efficiency design".
                            // So nothing has to be FLOWING for green:
                            //   "connected" a source is routed into this bus — of a
                            //               kind its type carries — and that
                            //               source's device is still there;
                            //   "offline"   routed, but the device it names is
                            //               not available right now (a closed
                            //               window, an unplugged camera, a monitor
                            //               that is gone);
                            //   "none"      nothing routed into this bus at all.
                            //
                            // NOTHING IS STARTED to answer this: no tap, no
                            // enumeration, no device open. The routing graph is in
                            // the model and reachability is the SAME cached-roster
                            // probe the cards' slash state uses — that is the point
                            // of a status light ("the l/r won't glow until the pill
                            // is clicked — that's to relieve the engine from
                            // unnecessary work").
                            readonly property string busStatus: {
                                void root.modelsRev
                                void root.devicesRev
                                void LiveOutputService.inputRev
                                // The AUDIO half's live refresh: inputRev rides the
                                // video/NDI pump, and a meter tap is published through
                                // the meter snapshots instead — so this is what makes a
                                // routed input whose tap stopped read "not delivering"
                                // rather than keep the answer it last got. (meterList
                                // changes only while a tap is live, so an idle pane
                                // pays nothing for it.)
                                void root.meterList.length
                                if (!busCard)
                                    return ""
                                const b = BusListModel.getBus(index)
                                const audio = b.routedAudioInputs !== undefined ? b.routedAudioInputs : []
                                const video = b.routedVideoSources !== undefined ? b.routedVideoSources : []
                                // Only the kinds this bus's type can carry count as a
                                // connection: an audio bus with a video source
                                // routed would be dropped by the engine anyway.
                                const wantsAudio = busType !== "video"
                                const wantsVideo = busType !== "audio"
                                const routedVideo = wantsVideo ? video.length : 0
                                const routedAudio = wantsAudio ? audio : []
                                if (routedVideo === 0 && routedAudio.length === 0)
                                    return "none"
                                // ANY routed source still being there makes the bus
                                // connected: a bus carrying two inputs is not
                                // offline because one of them went away.
                                //
                                // ...AND WHILE THE APP IS PULLING IT, "there" IS
                                // NOT ENOUGH — the light must also mean DELIVERING
                                // ("green only when the device is delivering
                                // frames or audio"), so a source the app already
                                // holds a tap on counts as connected only while
                                // that tap is handing data over. The live half is
                                // asked, never started: inputDelivery reads the
                                // taps the app has (a card's thumbnail, a held
                                // monitor, the output take, a meter tap) and adds
                                // no work of its own; "" (nothing held) leaves the
                                // presence answer standing, which is why an idle
                                // bus does not read amber just because nothing is
                                // being pulled from it.
                                // inputAvailable, not inputHealthy: THE LIGHT MAY PING.
                                // The slash state (inputHealthy) is a pure cached-roster
                                // read by design, and the roster is only filled by an
                                // enumeration a PICKER used to trigger — so a light
                                // built on it stayed amber until the operator opened
                                // the Audio & Video screen ("the probe gate should not
                                // affect this light — give them access to ping so they
                                // can stay updated without me having to open the
                                // gate"). This probe asks the machine itself when its
                                // cache cannot answer, off the GUI thread.
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
                            // THREE COLOURS, FOUR STATES: "starved" is the same amber
                            // as "offline" — both mean "look at this bus" — and the
                            // tooltip is what says which of the two it is.
                            readonly property color busStatusColor: busStatus === "connected" ? Theme.success
                                : (busStatus === "offline" || busStatus === "starved") ? Theme.warning
                                : Theme.dangerLight
                            // The DOT's own sentence (hover only): the state in words,
                            // because a green/amber/red light is not readable to
                            // everyone.
                            readonly property string busStatusText: busStatus === "connected"
                                ? qsTr("Bus connected")
                                : busStatus === "offline"
                                  ? qsTr("Bus connected · source not available")
                                : busStatus === "starved"
                                  ? qsTr("Bus connected · source is not delivering")
                                  : qsTr("Bus not connected")

                            // ---- WHAT THE WINDOW SAYS ----------------
                            // NOTHING. See the note in the middle of the card's
                            // state window: the routing counts and the "click to
                            // monitor" line were removed. The names below the
                            // window keep the bus's own caption.
                            // WHY the held tap can never show a frame ("" while it
                            // is fine, warm-up included) — the state the pulsing
                            // ring could not express: a capture tap whose open
                            // failed used to be indistinguishable from one that was
                            // still connecting, so the ring pulsed for as long as
                            // the card was on screen with no reason anywhere (see
                            // IVideo::PreviewFailure).
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
                            // window shows the device's peaks). A row that names
                            // no device has nothing to meter — toggleAudioMeter
                            // explains that instead of opening the default mic.
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
                            // Which state-window style this bus gets — the
                            // audio input card's design for an audio (or
                            // "both") bus, a video-flavored one for a video
                            // bus (see the BUS STATE block below).
                            readonly property string busType: {
                                void root.modelsRev
                                if (!busCard) return ""
                                const b = BusListModel.getBus(index)
                                return b.type !== undefined ? b.type : "audio"
                            }
                            // Live stereo bus mix (0..1, post-fader). Each
                            // output channel reads only its matching source
                            // channel and gain; a mono source feeds both sides.
                            readonly property var busChannelLevels: {
                                void root.meterList
                                void root.modelsRev
                                // THE PAIR READS THIS BUS'S OWN MIX, whatever kind
                                // of bus it is. A bus's declared type is its
                                // ROUTING POLICY — what it may carry — not a
                                // statement that it carries no audio: the same
                                // input can be routed into an audio bus and a
                                // video bus at once, and the video bus's pair is
                                // where that route's level shows. (The old
                                // `busType === "video"` early-out made every
                                // video bus's pair permanently flat — the
                                // "has audio connected and the bottom-right pair
                                // is not glowing" report.)
                                if (!busCard) return [0, 0]
                                // ...AND ONLY THE BUS THAT IS MONITORING READS A
                                // LEVEL. The taps live on the INPUT, so one click on
                                // one bus would otherwise light every bus that
                                // routes that input at once — the report: "when i
                                // click one source input ... the other audio source
                                // starts to glow when i clicked only one". The
                                // user's rule is one audio monitor at a time; this
                                // is that rule on screen.
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
                                // ...and the bus's OWN per-side trims, the same
                                // channelGain convention a source card applies: a
                                // side closed by this card's L/R pill reads 0 here
                                // too, so the pill can never glow while the audio
                                // path has that side silenced.
                                const sideL = BusListModel.channelGain(index, 0)
                                const sideR = BusListModel.channelGain(index, 1)
                                return [mix[0] * busLevel * sideL, mix[1] * busLevel * sideR]
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
                                void root.devicesRev
                                void LiveOutputService.inputRev
                                if (busCard)
                                    return !card.busVideoPreviewing
                                        || LiveOutputService.inputHealthy(card.busVideoLabel,
                                                                         card.busVideoSource.kind || "")
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
                            color: cardHover.hovered ? Theme.activeBg : Theme.rowBg
                            border.width: taken ? 2 : 1
                            border.color: taken ? Theme.accent : "transparent"

                            PositionHoverArea {
                                id: cardHover
                                anchors.fill: parent
                                // ONE CLICK = THE CARD'S OWN STATE WINDOW:
                                //   video → live thumbnail (owner "card")
                                //   audio → live level meter (the WASAPI tap)
                                //   bus   → monitor the routed inputs required by its type
                                // Re-click releases/re-toggles. DOUBLE-CLICK on a
                                // VIDEO card = OUTPUT PREVIEW (the deliberate
                                // take, purple border) — Qt delivers the first
                                // click on the way, so a double-click previews
                                // AND takes, reading as one motion.
                                //
                                // BUS MUTE lives only on its L/R pill. The card
                                // body monitors the routes required by the bus
                                // type and explains missing routes with a toast.
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
                                        // NOT a real "take to output" — there is no
                                        // playback engine behind the production
                                        // graph yet (ProductionEngine.hpp says so
                                        // itself: "never renders pixels, plays
                                        // audio"), so nothing routed here reaches
                                        // real audio hardware. Starting the SAME
                                        // real WASAPI meter tap a single click
                                        // starts is the honest interim: the L/R
                                        // pills genuinely animate off this mic's
                                        // live levels instead of double-click doing
                                        // nothing. Revisit once a real mix/playback
                                        // engine exists to actually route to.
                                        if (root.meteredLabels.indexOf(sub) < 0)
                                            root.toggleAudioMeter(sub)
                                    } else if (busCard) {
                                        // Same honesty note as audioCard above,
                                        // for the bus's OUTGOING audio specifically
                                        // (user call: "if there is out going audio
                                        // it should turn it on to as well, if
                                        // not[,] [leave it]"): busAudioLevel (this
                                        // delegate's own mix computation) already
                                        // reads real meter taps on the bus's
                                        // routed inputs — this just starts those
                                        // taps (a no-op when the bus routes no
                                        // audio) so the bus card's live mix
                                        // actually animates instead of sitting
                                        // flat until someone separately metered
                                        // each routed input by hand.
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
                                color: Theme.windowBg
                                border.width: 1
                                border.color: card.taken ? Qt.alpha(Theme.accent, 0.45) : Theme.border

                                // RECEIVING (video): the live feed (the same
                                // provider frames the monitor tile shows — the
                                // tap runs under EITHER hold: the card preview
                                // or the output take; the pump's rev re-fetches
                                // at ~15 fps).
                                Image {
                                    id: stateWinImage
                                    anchors.fill: parent
                                    // THE TRACKED DEPENDENCY, and it has to be spelled
                                    // out: inputProducing() is a plain call, so the
                                    // binding cannot see the tick that flips it, and
                                    // the value it CAN see (busVideoPreviewing) goes
                                    // false→true the moment the take starts — a pump
                                    // tick BEFORE the first frame lands. Evaluating
                                    // `visible` right then cached false and nothing
                                    // ever invalidated it: the monitor stayed empty
                                    // for the whole take (no frames pulled, and the
                                    // light's live half saw no pulls at all).
                                    // inputRev bumps on every pump tick, so the
                                    // binding re-reads itself once the frames start.
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
                                // THE BUS STATUS DOT — three states, one light (see
                                // card.busStatus): green = a routed source is
                                // active, amber = routed but idle, red = nothing
                                // routed or its device is gone. It sits in the
                                // window's top-right corner, out of the centred L/R
                                // pair's way and out of the picture's middle.
                                Item {
                                    id: busDot
                                    visible: busCard
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.topMargin: 6
                                    anchors.rightMargin: 6
                                    width: 10; height: 10

                                    // The halo: the dot's own colour, wide and
                                    // dim, so a green reads at a glance on a dark
                                    // window without the dot itself having to be
                                    // large.
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
                                        // The room the dot needs for the pointer to
                                        // find it at this size — the light is 10px
                                        // and the pointer should not have to be
                                        // pixel-perfect.
                                        anchors.margins: -6
                                        showCursor: false
                                        clickEnabled: false
                                        onEntered: TipService.show(card.busStatusText, busDot)
                                        onExited: TipService.hide(busDot)
                                    }
                                    Component.onDestruction: TipService.forget(busDot)
                                }
                                // RECEIVING (audio): the L / R BUTTONS — one
                                // round button per channel, centered as a pair,
                                // that GLOWS with its own live level (a single
                                // color sampled off the green→amber→red scale,
                                // not a partial-height fill bar) and MUTES that
                                // channel on click — a real mute, written
                                // through the same setChannelGain the Edit
                                // dialog's gain knob uses (gain 0 = muted).
                                Row {
                                    id: pillsRow
                                    anchors.centerIn: parent
                                    visible: audioCard && card.metering && card.healthy
                                    spacing: 20

                                    Repeater {
                                        // The classic L/R pair: one button per
                                        // side, from the snapshot's real channel
                                        // count (mono collapses to just L).
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
                                            // Reactive through modelsRev — channelGain()
                                            // itself is an untracked Q_INVOKABLE read.
                                            // The channel's GAIN, not a pre-chewed
                                            // boolean: the pill reads the state off it
                                            // through the app's one vocabulary, so a
                                            // closed OR trimmed side reads here exactly
                                            // as it does on the dialogs' gain knob.
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
                                                // PERCEPTUAL CURVE (sqrt): a mic's
                                                // silence baseline (~1.5% peak) is
                                                // invisible on a linear scale — sqrt
                                                // lifts it to ~12% (a clear glowing
                                                // nub that jumps when you speak) while
                                                // hot signals still reach the top.
                                                const raw = Math.max(0, Math.min(1, s.peaks[modelData]))
                                                    * AudioInputListModel.channelGain(card.index, modelData)
                                                    * Math.max(0, Math.min(1, Number(input.level || 0) / 100))
                                                return Math.sqrt(raw)
                                            }
                                            // Deferred, and the state is re-read
                                            // inside the call rather than captured:
                                            // this codebase never writes a model
                                            // from inside its own delegate's event
                                            // (the write re-fires the bindings that
                                            // own the delegate that dispatched it).
                                            // The model owns the reopen policy: a
                                            // closed side comes back at the trim it was
                                            // closed with, not at unity. This used to
                                            // be `closed ? 1.0 : 0.0` here — three copies
                                            // of the expression that LOST the trim.
                                            onToggled: Qt.callLater(() =>
                                                AudioInputListModel.toggleChannelGain(card.index, modelData))
                                        }
                                    }
                                }
                                // CONNECTING (video only; audio's meter shows
                                // silence as flat bars, which IS its truth):
                                // held, source reachable, frames not flowing yet
                                // (tap warm-up) — pulsing ring. A tap that FAILED
                                // is not warming up: the ring stops for it and the
                                // reason below takes its place.
                                Item {
                                    anchors.centerIn: parent
                                // The same tracked dependency as the feed above (and
                                // it matters in the same direction): this ring is
                                // shown while frames are NOT flowing, so a binding
                                // that never re-evaluates would park it over the
                                // picture for the rest of the take.
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
                                // SAME WINDOW, NO RING: the tap is held and the
                                // source is still in the roster, but the capture
                                // itself never opened — the device is busy or its
                                // driver refused, or the monitor/window's device
                                // context is gone. The ring above is for a tap that
                                // is WARMING UP; this one says what happened. The
                                // reason is the platform's own words, so it is the
                                // one place a real cause is readable from the UI.
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
                                // ...and the reason as a caption at the window's
                                // foot: the words can be long ("display or window
                                // device context unavailable"), and a caption is the
                                // only shape that reads at this size.
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
                                    visible: !card.healthy && (card.holding || audioCard || busCard && card.busVideoPreviewing)
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
                                // THE BUS'S L/R PAIR — one control, TWO PLACEMENTS
                                // (user call: "for the audio bus all the L and R
                                // are meant to be at the center, for the video and
                                // both the L and R are meant to be at the bottom
                                // right because of the thumbnail preview they
                                // have"): an audio bus has no picture to protect,
                                // so its pair sits CENTRED — the same place, and the
                                // same glow-button design, the audio input cards'
                                // meters use. A video or "both" bus shows its routed
                                // feed in this window, so its pair tucks into the
                                // corner out of the picture's way. Every bus card
                                // carries the pair: it is the bus's own output trim,
                                // which exists for a video bus too (its bars simply
                                // stay flat — a video bus carries no audio).
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

                                        // THE SAME PILL A SOURCE CARD USES (see
                                        // ChannelPill) — the bus is a combination of
                                        // an audio and a video source, so its L/R is
                                        // the source cards' L/R, not a bus-flavoured
                                        // copy of it. Clicking closes THAT SIDE of
                                        // the bus's own output (BusListModel's
                                        // per-side trim, applied by the mixer); it
                                        // used to mute the whole bus, which is the
                                        // "clicking L or R closes all" report.
                                        // The bus's overall Mute stays a bus-level
                                        // control (the Edit Bus dialog), and the
                                        // card's sublabel already says " - muted".
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
                                            // SIZE FOLLOWS PLACEMENT: the compact
                                            // corner sizing only while the pair sits
                                            // in the window's corner (a video or
                                            // "both" bus, where the feed is behind
                                            // it). The centred pair on an audio bus
                                            // is the BIG one — the same live-meter
                                            // pill the audio input cards use, whose
                                            // own glow is the meter. Both breathe
                                            // off the bus's live mix.
                                            compact: !busAudioRow.centred
                                            onToggled: Qt.callLater(() =>
                                                BusListModel.toggleChannelGain(card.index, modelData))
                                        }
                                    }
                                }
                                // (THE BUS'S MIDDLE IS EMPTY.) It used to carry
                                // a sentence — "1 video source routed · 1 audio
                                // input routed" over "Click to monitor video" —
                                // and then the status sentence as well. All of it
                                // is gone (the user's call: "remove those
                                // descriptive text"): the WINDOW is for the bus's
                                // picture and its live L/R pair, the light in the
                                // corner is the status, and the sublabel under the
                                // name already spells the routing out in words
                                // ("2 sources · both"). What is left here is the
                                // tap target, nothing drawn.

                                // VIDEO AUDIO — a small L/R pair overlaid on the
                                // state window, always visible regardless of
                                // preview state. Camera/screen sources have NO
                                // audio capability in this engine (see
                                // VideoSourceListModel's own documented
                                // contract) — those stay greyed out and non-
                                // interactive, a visible "this source type
                                // doesn't carry audio" signal rather than
                                // hidden entirely. Media/NDI rows DO carry
                                // audio (NDI embeds it with its video); those
                                // glow from the row's stored level, dimmed by its
                                // own Mute — there is no live audio TAP for
                                // video-tab rows yet (unlike the real per-device
                                // WASAPI metering audio inputs get), so this
                                // reads the stored fader, not a live signal,
                                // same "no fake signal" rule as everywhere else.
                                // Each pill also CLOSES ITS OWN SIDE (the row's
                                // per-side trim), which is a different thing
                                // from the row-wide Mute the Edit dialog owns.
                                Row {
                                    id: vidAudioRow
                                    visible: videoCard
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: 6
                                    spacing: 6

                                    readonly property bool audioCapable: rowKind === "media" || rowKind === "ndi"
                                    // A LIVE stream snapshot exists for this row (the
                                    // bridge's tap on the media file / NDI stream,
                                    // held while this card is previewing or on air) —
                                    // then the pills read REAL audio and may breathe.
                                    // Without one all we know is the stored fader, so
                                    // they show it and stay still (never implying live
                                    // motion a stored value can't back up).
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

                                        // THE ONE PILL (see ChannelPill) — the SAME
                                        // control the audio and bus cards use, in its
                                        // small overlay size. It closes its OWN SIDE
                                        // only: the hand-rolled copy here wrote the
                                        // row's single mute flag, so clicking L also
                                        // closed R (both pills went red at once — the
                                        // "if i click on the L or R it closes all"
                                        // report).
                                        ChannelPill {
                                            required property int modelData

                                            channelIndex: modelData
                                            objectName: "selfTestVideoPill_" + card.index + "_" + modelData
                                            capable: vidAudioRow.audioCapable
                                            // The row's own Mute still darkens the
                                            // level below (nothing to show), but it
                                            // is not what makes a pill read as
                                            // closed — that is the side's trim.
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
                                                // Same perceptual sqrt lift the audio
                                                // cards use, and the SAME source-
                                                // channel rule (a mono file's one
                                                // channel feeds both pills).
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
                          : qsTr("Nothing here yet - add %1 in Settings - Audio & Video.")
                                .arg(root.inputRoster.label.toLowerCase())
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
        // Media-kind rows go through the real decoder now (takeMedia): a
        // video/image file plays into the compositor's media layer, same
        // convention as a taken camera/screen input. takeMedia() itself
        // refuses anything that isn't a video/image extension — an AUDIO
        // file has no compositor layer to fill, so that stays untaken
        // (there's genuinely nothing to preview visually), same honest
        // refusal the engine gives.
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
