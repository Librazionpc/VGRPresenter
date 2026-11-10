import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

Item {
    id: root

    property int modelsRev: 0
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
        function onModelReset() { root.modelsRev++ }
    }
    Connections {
        target: VideoSourceListModel
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

    // ---- Self-test hook (VGR_SELFTEST scenarios only): mimic the Add
    function selfTestPickVideoSource(label, kind) {
        root.addSourceType = "video"
        root.addSourceKind = kind || "camera"
        root.addSourceSublabel = label
        root.addSourceShown = true
    }

    // ---- Live input metering (the board rows' + dialogs' real VU feed) ----
    readonly property var audioMeterList: EngineBridge.inputLevels
    function meterSnapshotFor(label) {
        const want = label === undefined ? "" : String(label).trim()
        if (want === "")
            return null
        const list = root.audioMeterList
        for (let i = 0; i < list.length; i++) {
            const s = list[i]
            if (String(s.label || "").trim() === want)
                return s
        }
        return null
    }
    function inputChannelLevels(rowIndex, sublabel) {
        const s = root.meterSnapshotFor(sublabel)
        if (!s || s.peaks === undefined || s.peaks.length === 0)
            return [0, 0]
        const data = AudioInputListModel.getInput(rowIndex)
        if (!data || data.muted || Number(data.level) <= 0)
            return [0, 0]
        const result = [0, 0]
        for (let c = 0; c < 2; ++c) {
            const sourceChannel = s.peaks.length === 1 ? 0 : c
            if (sourceChannel >= s.peaks.length) continue
            const gainChannel = s.peaks.length === 1 ? 0 : c
            const gain = AudioInputListModel.channelGain(rowIndex, gainChannel)
            result[c] = Math.max(0, Math.min(1, s.peaks[sourceChannel]))
                        * gain * (Number(data.level) / 100)
        }
        return result
    }
    function inputMixLevel(rowIndex, sublabel) {
        const channels = root.inputChannelLevels(rowIndex, sublabel)
        return Math.max(channels[0], channels[1])
    }
    function videoChannelLevels(rowIndex, sublabel, kind, muted, level) {
        if (kind !== "media" && kind !== "ndi")
            return [0, 0]   // camera/screen feeds carry no audio at all
        if (muted || Number(level) <= 0)
            return [0, 0]
        const s = root.meterSnapshotFor(sublabel)
        if (!s || s.peaks === undefined || s.peaks.length === 0)
            return [0, 0]
        const result = [0, 0]
        for (let c = 0; c < 2; ++c) {
            const sourceChannel = s.peaks.length === 1 ? 0 : c
            if (sourceChannel >= s.peaks.length)
                continue
            const gain = rowIndex >= 0
                ? VideoSourceListModel.channelGain(rowIndex, s.peaks.length === 1 ? 0 : c)
                : 1.0
            result[c] = Math.max(0, Math.min(1, s.peaks[sourceChannel]))
                        * gain * (Number(level) / 100)
        }
        return result
    }
    function videoLiveLevels(sublabel, kind, muted, level) {
        if (kind !== "media" && kind !== "ndi")
            return [0, 0]
        if (muted || Number(level) <= 0)
            return [0, 0]
        const s = root.meterSnapshotFor(sublabel)
        if (!s || s.peaks === undefined || s.peaks.length === 0)
            return [0, 0]
        const result = [0, 0]
        for (let c = 0; c < 2; ++c) {
            const sourceChannel = s.peaks.length === 1 ? 0 : c
            if (sourceChannel >= s.peaks.length)
                continue
            result[c] = Math.max(0, Math.min(1, s.peaks[sourceChannel]))
                        * (Number(level) / 100)
        }
        return result
    }
    function videoMixLevel(rowIndex, sublabel, kind, muted, level) {
        const channels = root.videoChannelLevels(rowIndex, sublabel, kind, muted, level)
        return Math.max(channels[0], channels[1]) * 100
    }
    // The channel layout a source's OWN tap reports (a mono file really is one
    // strip), with `fallback` while no tap has published yet.
    function meterLayoutFor(sublabel, fallback) {
        const s = root.meterSnapshotFor(sublabel)
        return s && s.layout !== undefined && s.layout !== "" ? s.layout : fallback
    }

    function busChannelLevels(busIndex) {
        if (busIndex < 0) return [0, 0]
        const b = BusListModel.getBus(busIndex)
        const mix = [0, 0]
        for (const r of b.routedAudioInputs) {
            const data = AudioInputListModel.getInput(r)
            if (!data || data.muted) continue
            const channels = root.inputChannelLevels(r, data.sublabel)
            for (let c = 0; c < 2; ++c)
                if (channels[c] > mix[c]) mix[c] = channels[c]
        }
        for (const r of b.routedVideoSources) {
            const v = VideoSourceListModel.getSource(r)
            if (!v || v.muted) continue
            const channels = root.videoChannelLevels(r, v.sublabel, v.kind,
                                                     v.muted, v.level)
            for (let c = 0; c < 2; ++c)
                if (channels[c] > mix[c]) mix[c] = channels[c]
        }
        // The bus strip applies its own fader and mute after this incoming
        // source signal is measured, so expose the source-side mix here only.
        return mix
    }
    function busMixLevel(busIndex) {
        const channels = root.busChannelLevels(busIndex)
        return Math.max(channels[0], channels[1])
    }
    function busChannelOpen(busIndex, channel) {
        if (busIndex < 0) return true
        const b = BusListModel.getBus(busIndex)
        const routed = b.routedAudioInputs
        if (routed.length === 0) return true
        for (const r of routed) {
            const data = AudioInputListModel.getInput(r)
            if (!data || data.muted) continue
            const count = Number(data.channels) > 0 ? Number(data.channels) : 2
            const sourceChannel = count === 1 ? 0 : channel
            if (!Db.isChannelOff(AudioInputListModel.channelGain(r, sourceChannel)))
                return true
        }
        return false
    }
    readonly property var audioMeterSnapshot: {
        if (root.editAudioIndex >= 0 && root.editAudioKind === "device")
            return root.meterSnapshotFor(root.editAudioSublabel)
        if (root.addSourceShown && root.addSourceType === "audio"
            && root.addSourceKind === "device")
            return root.meterSnapshotFor(root.addSourceSublabel)
        return null
    }
    readonly property var audioMeterLevels: {
        const s = root.audioMeterSnapshot
        return s && s.peaks !== undefined ? s.peaks : []
    }
    readonly property string audioMeterLayout: {
        const s = root.audioMeterSnapshot
        return s && s.layout !== undefined ? s.layout : ""
    }

    // ---- "Only while it is actually on screen" ----------------------------
    property var rowsInView: ({})      // "<kind>:<index>" → true while on screen
    property int rowsInViewRev: 0
    function setRowInView(kind, index, on) {
        const key = kind + ":" + index
        if ((root.rowsInView[key] === true) === !!on)
            return
        const next = {}
        for (const k in root.rowsInView)
            next[k] = root.rowsInView[k]
        if (on)
            next[key] = true
        else
            delete next[key]
        root.rowsInView = next
        root.rowsInViewRev++
    }
    function rowInView(kind, index) {
        void root.rowsInViewRev
        return root.rowsInView[kind + ":" + index] === true
    }

    readonly property var audioMeterTargets: {
        void root.modelsRev
        const labels = []
        const add = (value) => {
            const label = value === undefined || value === null ? "" : String(value).trim()
            if (label === "" || labels.indexOf(label) >= 0)
                return
            labels.push(label)
        }
        // A page that is not on screen holds no taps at all (every settings page is built at startup;
        // see RoutingScreen.syncMeters).
        if (!root.visible)
            return labels
        for (let r = 0; r < AudioInputListModel.rowCount(); ++r) {
            // ON SCREEN ONLY (see rowsInView): a row's meter is the reason its
            // device is open, so a row scrolled off the board gives it back.
            if (!root.rowInView("audio", r))
                continue
            const data = AudioInputListModel.getInput(r)
            if (data && data.kind === "device" && !data.muted)
                add(data.sublabel)
        }
        if (root.editAudioIndex >= 0 && root.editAudioKind === "device")
            add(root.editAudioSublabel)
        if (root.addSourceShown && root.addSourceType === "audio"
            && root.addSourceKind === "device")
            add(root.addSourceSublabel)
        return labels
    }
    // The labels THIS screen holds a tap for (its own bookkeeping — never a
    // reflection of what the rest of the app meters).
    property var heldAudioMeters: []
    function syncAudioMeters() {
        const wanted = EngineBridge.platformCapabilities.audioInputCapture === true
                     ? root.audioMeterTargets : []
        const held = root.heldAudioMeters
        const next = []
        for (const label of held)
            if (wanted.indexOf(label) >= 0) next.push(label)
        for (const label of wanted)
            if (next.indexOf(label) < 0) next.push(label)
        for (const label of held)
            if (next.indexOf(label) < 0) EngineBridge.stopInputMeter(label)
        for (const label of wanted)
            if (held.indexOf(label) < 0) EngineBridge.startInputMeter(label)
        root.heldAudioMeters = next
    }
    onAudioMeterTargetsChanged: root.syncAudioMeters()
    Connections {
        target: EngineBridge
        function onDevicesChanged() {
            root.syncAudioMeters()
            root.syncVideoMeters()
        }
    }
    Component.onCompleted: {
        root.syncAudioMeters()
        root.syncVideoMeters()   // the screen's first stream-meter reconcile
    }

    // ---- Video/NDI rows' OWN audio (the bridge's STREAM taps) -------------
    readonly property var videoMeterTargets: {
        void root.modelsRev
        const out = []
        const add = (label, kind) => {
            const l = label === undefined || label === null ? "" : String(label).trim()
            if (l === "" || (kind !== "media" && kind !== "ndi"))
                return
            for (let i = 0; i < out.length; ++i)
                if (out[i].label === l) return
            out.push({ label: l, kind: kind })
        }
        if (!root.visible)      // a hidden page holds no stream meters either
            return out
        for (let r = 0; r < VideoSourceListModel.rowCount(); ++r) {
            if (!root.rowInView("video", r))
                continue
            const data = VideoSourceListModel.getSource(r)
            if (data) add(data.sublabel, data.kind)
        }
        if (root.editVideoIndex >= 0)
            add(root.editVideoSublabel, root.editVideoKind)
        if (root.addSourceShown && root.addSourceType === "video")
            add(root.addSourceSublabel, root.addSourceKind)
        return out
    }
    // The stream labels this screen holds (its own bookkeeping — never a
    // reflection of what the rest of the app meters).
    property var heldVideoMeters: []
    function syncVideoMeters() {
        const wanted = EngineBridge.platformCapabilities.audioStreamTaps === true
                     ? root.videoMeterTargets : []
        root.heldVideoMeters = ScreenHelpers.syncVideoMeters(wanted,
                                                             root.heldVideoMeters)
    }
    onVideoMeterTargetsChanged: root.syncVideoMeters()

    function cleanupAudioVideoPreviews() {
        // Only this screen's own holds — the Media pane's meters must survive
        // a trip through Settings (see the metering note above).
        for (const label of root.heldAudioMeters)
            EngineBridge.stopInputMeter(label)
        root.heldAudioMeters = []
        for (const t of root.heldVideoMeters)
            EngineBridge.stopStreamMeter(t.label)
        root.heldVideoMeters = []
        EngineBridge.stopAllVideoPreviews()
    }
    Component.onDestruction: root.cleanupAudioVideoPreviews()

    // ---- Routing matrix modal — shared by the Add + Edit audio dialogs.
    property string routingModalFor: ""   // "add" | "edit"
    RoutingMatrixModal {
        id: routingModal
        z: 30001
        busRev: root.modelsRev
        onApplied: (autoRoute, routes) => {
            if (root.routingModalFor === "edit" && root.editAudioIndex >= 0) {
                AudioInputListModel.setRoutingAuto(root.editAudioIndex, autoRoute)
                const lists = []
                for (let c = 0; c < root.editAudioEffectiveChannels; c++)
                    lists.push(routes[c] !== undefined ? routes[c] : [])
                AudioInputListModel.setChannelRoutes(root.editAudioIndex, lists)
            } else if (root.routingModalFor === "add") {
                root.addSourceRoutingAuto = autoRoute
                root.addSourceRoutes = routes
            }
        }
    }

    // A media source IS its file: its name is the file's name until somebody renames it.
    function fileTitleOf(path) {
        const s = String(path).replace(/\\/g, "/")
        const f = s.substring(s.lastIndexOf("/") + 1)
        const d = f.lastIndexOf(".")
        return d > 0 ? f.substring(0, d) : f
    }
    // "Not renamed": empty, the generated "New Source N", or still the previous file's own name.
    function nameIsAutomatic(name, previousPath) {
        const n = String(name || "")
        return n === "" || /^New Source \d+$/.test(n)
               || (String(previousPath || "") !== "" && n === root.fileTitleOf(previousPath))
    }
    function applyVideoPickedFile(target, path) {
        if (target === "editVideo") {
            const prev = root.editVideoSublabel
            root.editVideoSublabel = path
            if (root.editVideoKind === "media" && root.nameIsAutomatic(root.editVideoName, prev))
                root.editVideoName = root.fileTitleOf(path)
        } else if (target === "addSourceVideo") {
            const prev = root.addSourceSublabel
            root.addSourceSublabel = path
            if (root.addSourceKind === "media" && root.nameIsAutomatic(root.addSourceName, prev))
                root.addSourceName = root.fileTitleOf(path)
        }
    }

    // ---- Media Library picker — shared by all four Source "Browse..."
    property string mediaPickTarget: ""   // "" | "editAudio" | "addSourceAudio" | "editVideo" | "addSourceVideo"
    MediaSourceModal {
        id: settingsMediaPicker
        z: 30001
        open: root.mediaPickTarget !== ""
        fixedKind: root.mediaPickTarget === "editVideo" || root.mediaPickTarget === "addSourceVideo"
                   ? "video" : "audio"
        onApplied: (item) => {
            const path = item.path !== undefined ? item.path : item.name
            if (root.mediaPickTarget === "editAudio") root.editAudioSublabel = path
            else if (root.mediaPickTarget === "addSourceAudio") root.addSourceSublabel = path
            else if (root.mediaPickTarget === "editVideo" || root.mediaPickTarget === "addSourceVideo")
                root.applyVideoPickedFile(root.mediaPickTarget, path)
            root.mediaPickTarget = ""
        }
        onCancelled: root.mediaPickTarget = ""
        // "Browse this PC..." — the Media Library only covers indexed
        // folders; a file that isn't in it yet still needs a way in.
        onBrowseRequested: {
            const isVideo = root.mediaPickTarget === "editVideo" || root.mediaPickTarget === "addSourceVideo"
            const file = isVideo ? VideoSourceListModel.pickVideoFile() : AudioInputListModel.pickAudioFile()
            if (file !== "") {
                if (root.mediaPickTarget === "editAudio") root.editAudioSublabel = file
                else if (root.mediaPickTarget === "addSourceAudio") root.addSourceSublabel = file
                else if (root.mediaPickTarget === "editVideo" || root.mediaPickTarget === "addSourceVideo")
                    root.applyVideoPickedFile(root.mediaPickTarget, file)
            }
            root.mediaPickTarget = ""
        }
    }

    // ---- Edit Audio Input dialog ----
    property int editAudioIndex: -1
    property string editAudioName: ""
    property string editAudioKind: "device"
    property string editAudioSublabel: ""
    property var editAudioSublabelByKind: ({})
    property real editAudioLevel: 75
    property bool editAudioMuted: false
    // Pro-audio form state — Delay / Channels (the reference mock's rows),
    // persisted on the model like level/muted.
    property int editAudioDelayMs: 0
    property int editAudioChannels: 2
    property var editAudioGains: []   // per-channel faders, live from the model
    readonly property int editAudioEffectiveChannels: {
        const n = root.editAudioKind === "device" ? root.deviceChannels(root.editAudioSublabel) : 0
        return n > 0 ? n : root.editAudioChannels
    }
    // Which rack chip is open in the effects editor.
    property string editAudioSelectedEffect: ""
    readonly property var editAudioEffects: {
        root.modelsRev
        return root.editAudioIndex < 0 ? [] : AudioInputListModel.getInput(root.editAudioIndex).effects
    }

    function rememberEditAudioGain(channel, gain) {
        if (root.editAudioIndex < 0)
            return
        const next = root.editAudioGains.slice()
        while (next.length <= channel)
            next.push(1.0)
        next[channel] = gain
        root.editAudioGains = next
    }

    function openEditAudio(index) {
        const data = AudioInputListModel.getInput(index)
        root.editAudioIndex = index
        root.editAudioName = data.name
        root.editAudioKind = data.kind
        root.editAudioSublabel = data.sublabel
        // Fresh buffer per row — a previous row's remembered picks must
        // never leak into this one.
        root.editAudioSublabelByKind = ({})
        root.editAudioLevel = data.level
        root.editAudioMuted = data.muted
        root.editAudioDelayMs = data.delayMs !== undefined ? data.delayMs : 0
        root.editAudioChannels = data.channels !== undefined ? data.channels : 2
        root.editAudioGains = data.channelGains !== undefined ? data.channelGains : []
        root.editAudioSelectedEffect = ""
        root.syncAudioMeters()
    }
    function saveEditAudio() {
        if (root.editAudioIndex < 0)
            return
        AudioInputListModel.renameInput(root.editAudioIndex, root.editAudioName)
        AudioInputListModel.setKind(root.editAudioIndex, root.editAudioKind)
        AudioInputListModel.setSublabel(root.editAudioIndex, root.editAudioSublabel)
        AudioInputListModel.setLevel(root.editAudioIndex, root.editAudioLevel)
        AudioInputListModel.setMuted(root.editAudioIndex, root.editAudioMuted)
        AudioInputListModel.setDelayMs(root.editAudioIndex, root.editAudioDelayMs)
        AudioInputListModel.setChannels(root.editAudioIndex, root.editAudioChannels)
        root.editAudioIndex = -1
        // Dialog closed — release the dialog's tap (the board rows' taps stay;
        // they live with the screen, not the dialog).
        root.syncAudioMeters()
    }

    // ---- Add Source dialog ----
    property bool addSourceShown: false
    property string addSourceType: "audio"
    property string addSourceName: ""
    property string addSourceKind: "device"
    property string addSourceSublabel: ""
    // Per-kind memory for the Source field — see editAudioSublabelByKind.
    property var addSourceSublabelByKind: ({})
    property string addSourceVideoMode: ""   // video rows' capture mode
    property real addSourceLevel: 75
    property bool addSourceMuted: false
    // Pro-audio form state — Delay / Channels, written through the setters
    // after addInput() like every other collected value.
    property int addSourceDelayMs: 0
    property int addSourceChannels: 2
    property var addSourceGains: []   // buffered per-channel faders (unity default)
    property var addSourceTrims: []
    // copy-on-write, like every other buffered list here: re-assignment is what
    // re-fires the form's readings.
    function rememberAddSourceTrim(i, g) {
        const v = Math.max(0, Math.min(1, g))
        const next = root.addSourceTrims.slice()
        while (next.length <= i)
            next.push(0.0)
        if (next[i] === v)
            return
        next[i] = v
        root.addSourceTrims = next
    }
    // Same engine-truth precedence as the Edit dialog (see there).
    readonly property int addSourceEffectiveChannels: {
        const n = root.addSourceKind === "device" ? root.deviceChannels(root.addSourceSublabel) : 0
        return n > 0 ? n : root.addSourceChannels
    }
    // Buffered routing-matrix state (the Routing… modal writes here; no
    // row exists until submit) — { channelIndex: [busIndex, ...] }.
    property bool addSourceRoutingAuto: false
    property var addSourceRoutes: ({})
    property var addSourceEffects: []
    property string addSourceSelectedEffect: ""

    readonly property var audioKinds: [
        { key: "device", label: qsTr("Device") },
        { key: "media", label: qsTr("Media") },
        { key: "ndi", label: qsTr("NDI") }
    ]

    readonly property var engineAudioDevices: {
        const list = EngineBridge.audioDevices.filter(d => d.isInput)
        return list.map(d => ({ label: d.label, value: d.value,
                                channels: d.channels !== undefined ? d.channels : 0,
                                sampleRateHz: d.sampleRateHz !== undefined ? d.sampleRateHz : 0 }))
    }
    function deviceChannels(sublabel) {
        if (!sublabel) return 0
        const hit = engineAudioDevices.find(d => d.value === sublabel)
        return hit ? hit.channels : 0
    }
    readonly property var deviceOptions: engineAudioDevices.length > 0
        ? engineAudioDevices
        : [{ label: qsTr("No input device found") }]
    readonly property var mediaSourceOptions: [
        { label: qsTr("Playlists & tracks") },
        { label: qsTr("Media File") },
        { label: qsTr("Stream Capture") }
    ]
    readonly property var audioMediaSourceOptions: [
        { label: qsTr("Select Playlist…"), value: "__browse_audio_file__" }
    ]
    readonly property var videoKinds: [
        { key: "camera", label: qsTr("Camera") },
        { key: "screen", label: qsTr("Screen") },
        { key: "media", label: qsTr("Media") },
        { key: "ndi", label: qsTr("NDI") }
    ]    // Capture modes for the video dialogs' Resolution picker. The ENGINE
    readonly property var videoModes: [
        "4Kp29.97", "2560x1440p29.97", "1080p60", "1080p29.97",
        "1280x960p29.97", "960x540p29.97", "720p29.97", "640x480p29.97",
        "640x360p29.97"
    ]

    // Real video-capture roster from the engine (the platform's capture stack).
    readonly property var engineVideoDevices: EngineBridge.videoDevices
    function videoDevice(name) {
        if (!name) return null
        const hit = engineVideoDevices.find(d => d.value === name)
        return hit || null
    }
    readonly property var cameraOptions: engineVideoDevices.length > 0
        ? engineVideoDevices
        : [{ label: qsTr("No camera found") }]
    readonly property var screenOptions: {
        const list = []
        for (let i = 0; i < EngineBridge.screenDevices.length; ++i) {
            const s = EngineBridge.screenDevices[i]
            let label = s.label
            if (s.widthPx > 0 && s.heightPx > 0)
                label += qsTr(" · %1 × %2").arg(s.widthPx).arg(s.heightPx)
            if (s.primary)
                label += qsTr(" · primary")
            list.push({ label: label, value: s.label })
        }
        if (list.length === 0)
            list.push({ label: qsTr("No displays found"), disabled: true })
        return list
    }
    // NDI roster: the engine's discovery results, or one honest row saying
    // why the list is empty (SDK absent vs still browsing).
    readonly property var ndiOptions: {
        if (!EngineBridge.ndiAvailable)
            return [{ label: EngineBridge.ndiState === "notInstalled"
                             ? qsTr("NDI runtime not installed — see the notice above")
                             : qsTr("NDI unavailable — see the notice above"),
                      disabled: true }]
        if (EngineBridge.ndiSources.length === 0)
            return [{ label: qsTr("No NDI sources found on the network"),
                      disabled: true }]
        return EngineBridge.ndiSources
    }

    // Audio NDI inputs draw from the same roster — NDI sources carry audio
    // as well as video.
    function audioSourceOptions(kind) {
        if (kind === "media") return audioMediaSourceOptions
        if (kind === "ndi") return ndiOptions
        return deviceOptions
    }
    function resolveAudioSourcePick(picked, previous, target) {
        if (picked !== "__browse_audio_file__")
            return picked
        // Async — opens the shared Select Media grid; its applied/cancelled
        // handler (see settingsMediaPicker) writes the real value later.
        root.mediaPickTarget = target
        return previous
    }
    readonly property var videoMediaSourceOptions: [
        { label: qsTr("Select Playlist…"), value: "__browse_video_file__" }
    ]
    function videoSourceOptions(kind) {
        if (kind === "camera") return cameraOptions
        if (kind === "screen") return screenOptions
        if (kind === "ndi") return ndiOptions
        if (kind === "media") return videoMediaSourceOptions
        return mediaSourceOptions
    }
    // Same "Browse for file..." resolution as resolveAudioSourcePick, for
    // the video dialogs' Source field.
    function resolveVideoSourcePick(picked, previous, target) {
        if (picked !== "__browse_video_file__")
            return picked
        root.mediaPickTarget = target
        return previous
    }
    function videoKindHasAudio(kind) {
        return kind === "media" || kind === "ndi"
    }
    // Modes + fps ceiling for the picked device (camera rows only — screen
    // and media have no per-device capability list yet).
    function videoModesFor(name) {
        const d = videoDevice(name)
        return (d && d.modes && d.modes.length > 0) ? d.modes : videoModes
    }
    function videoMaxFpsFor(name) {
        const d = videoDevice(name)
        return d ? d.maxFps : 0
    }

    function openAddSource(type) {
        root.addSourceType = type
        root.addSourceName = ""
        root.addSourceKind = type === "audio" ? "device" : "camera"
        root.addSourceSublabel = ""
        root.addSourceSublabelByKind = ({})   // fresh buffer per dialog session
        root.addSourceVideoMode = ""
        root.addSourceLevel = 75
        root.addSourceMuted = false
        root.addSourceDelayMs = 0
        root.addSourceChannels = 2
        root.addSourceGains = []
        root.addSourceTrims = []
        root.addSourceRoutingAuto = false
        root.addSourceRoutes = ({})
        // Seed the rack from the model's template (no row exists yet to read
        // from — defaultEffectsTemplate() is the pre-row snapshot).
        root.addSourceEffects = AudioInputListModel.defaultEffectsTemplate()
        root.addSourceSelectedEffect = ""
        root.addSourceShown = true
        if (type === "audio") EngineBridge.refreshAudioDevices()
        else EngineBridge.refreshDevices()
        root.syncAudioMeters()
    }

    function submitAddSource() {
        if (root.addSourceType === "audio") {
            AudioInputListModel.addInput()
            const idx = AudioInputListModel.rowCount() - 1
            AudioInputListModel.renameInput(idx, root.addSourceName)
            AudioInputListModel.setKind(idx, root.addSourceKind)
            AudioInputListModel.setSublabel(idx, root.addSourceSublabel)
            AudioInputListModel.setLevel(idx, root.addSourceLevel)
            AudioInputListModel.setMuted(idx, root.addSourceMuted)
            AudioInputListModel.setDelayMs(idx, root.addSourceDelayMs)
            AudioInputListModel.setChannels(idx, root.addSourceEffectiveChannels)
            for (let g = 0; g < root.addSourceGains.length; g++)
                AudioInputListModel.setChannelGain(idx, g, root.addSourceGains[g])
            for (let t = 0; t < root.addSourceTrims.length; t++) {
                if (!Db.isChannelOff(root.addSourceTrims[t]))
                    AudioInputListModel.setChannelTrim(idx, t, root.addSourceTrims[t])
            }
            AudioInputListModel.setRoutingAuto(idx, root.addSourceRoutingAuto)
            const routeLists = []
            for (let c = 0; c < root.addSourceEffectiveChannels; c++)
                routeLists.push(root.addSourceRoutes[c] !== undefined ? root.addSourceRoutes[c] : [])
            AudioInputListModel.setChannelRoutes(idx, routeLists)
            for (const e of root.addSourceEffects) {
                AudioInputListModel.setEffectEnabled(idx, e.key, e.enabled)
                AudioInputListModel.setEffectValue(idx, e.key, e.value)
            }
        } else {
            VideoSourceListModel.addSourceWith(root.addSourceName, root.addSourceKind,
                                               root.addSourceSublabel,
                                               root.addSourceLevel,
                                               root.addSourceMuted)
            VideoSourceListModel.setMode(VideoSourceListModel.rowCount() - 1,
                                         root.addSourceVideoMode)
        }
        root.addSourceShown = false
        root.syncAudioMeters()   // release the Add dialog's hold
    }

    // ---- Edit Video Source dialog ----
    property int editVideoIndex: -1
    property string editVideoName: ""
    property string editVideoKind: "camera"
    property string editVideoSublabel: ""
    // Per-kind memory for the Source field — see editAudioSublabelByKind.
    property var editVideoSublabelByKind: ({})
    property string editVideoMode: ""
    property bool editVideoMuted: false
    property real editVideoLevel: 75

    function openEditVideo(index) {
        const data = VideoSourceListModel.getSource(index)
        root.editVideoIndex = index
        root.editVideoName = (data.kind === "media" && String(data.sublabel || "") !== ""
                              && root.nameIsAutomatic(data.name, ""))
            ? root.fileTitleOf(data.sublabel) : data.name   // an un-renamed media source shows its file's name
        root.editVideoSublabelByKind = ({})   // fresh buffer per row
        root.editVideoKind = data.kind
        root.editVideoSublabel = data.sublabel
        root.editVideoMode = data.mode !== undefined ? data.mode : ""
        root.editVideoMuted = data.muted
        root.editVideoLevel = data.level
        // Ask for a current roster; the platform's device probing runs off-thread.
        EngineBridge.refreshDevices()
    }
    function saveEditVideo() {
        if (root.editVideoIndex < 0)
            return
        VideoSourceListModel.renameSource(root.editVideoIndex, root.editVideoName)
        VideoSourceListModel.setKind(root.editVideoIndex, root.editVideoKind)
        VideoSourceListModel.setSublabel(root.editVideoIndex, root.editVideoSublabel)
        VideoSourceListModel.setMode(root.editVideoIndex, root.editVideoMode)
        VideoSourceListModel.setMuted(root.editVideoIndex, root.editVideoMuted)
        VideoSourceListModel.setLevel(root.editVideoIndex, root.editVideoLevel)
        // This source's file is playing right now: the new level/mute applies to what is heard.
        if (root.editVideoKind === "media" && LiveOutputService.mediaOnAir
                && String(LiveOutputService.mediaPath).replace(/\\/g, "/").toLowerCase()
                   === String(root.editVideoSublabel).replace(/\\/g, "/").toLowerCase())
            LiveOutputService.setMediaLevel(root.editVideoLevel, root.editVideoMuted)
        root.editVideoIndex = -1
    }

    // ---- Bus dialog (Add + Edit share one form) ----
    property bool busDialogOpen: false
    property bool busDialogIsAdd: false
    property int editBusIndex: -1
    property string editBusName: ""
    property string editBusType: "audio"
    property string editBusOriginalType: "audio"
    property real editBusLevel: 75
    property bool editBusMuted: false

    function openEditBus(index) {
        const data = BusListModel.getBus(index)
        root.editBusIndex = index
        root.editBusName = data.name
        root.editBusType = data.type
        root.editBusOriginalType = data.type
        root.editBusLevel = data.level
        root.editBusMuted = data.muted
        root.editBusTemplateId = data.type === "template" ? String(data.templateId || "") : root.defaultBusTemplateId()
        root.editBusTemplateLocked = false
        root.busDialogOpen = true
        root.busDialogIsAdd = false
    }
    // What is routed into the bus being edited: [{ kind, index, name }], audio inputs then video
    // sources — the dialog's Sources list, each with its own Disconnect.
    function editBusSources() {
        root.modelsRev
        if (root.editBusIndex < 0)
            return []
        const b = BusListModel.getBus(root.editBusIndex)
        if (!b)
            return []
        const out = []
        for (const i of b.routedAudioInputs) {
            const d = AudioInputListModel.getInput(i)
            if (d) out.push({ kind: "audio", index: i, name: String(d.name || "") })
        }
        for (const i of b.routedVideoSources) {
            const d = VideoSourceListModel.getSource(i)
            if (d) out.push({ kind: "video", index: i, name: String(d.name || "") })
        }
        return out
    }
    // The design a template bus is made from (the Buses category). "" = none.
    property string editBusTemplateId: ""
    function defaultBusTemplateId() {
        const list = root.busTemplates
        for (let i = 0; i < list.length; ++i) {
            if (list[i].isDefault === true)
                return String(list[i].id)
        }
        return list.length > 0 ? String(list[0].id) : ""
    }
    function busTemplateIndex(id) {
        const want = String(id || "")
        for (let i = 0; i < root.busTemplates.length; ++i) {
            if (String(root.busTemplates[i].id) === want)
                return i
        }
        return -1
    }
    function busTemplateName(id) {
        const at = root.busTemplateIndex(id)
        return at >= 0 ? String(root.busTemplates[at].name || "") : ""
    }
    // SelectField carries the LABEL (the name); the bus stores the design ID.
    function busTemplateIdForName(name) {
        const want = String(name || "")
        for (let i = 0; i < root.busTemplates.length; ++i) {
            if (String(root.busTemplates[i].name || "") === want)
                return String(root.busTemplates[i].id)
        }
        return ""
    }

    function openAddBus() {
        root.editBusIndex = -1
        root.editBusName = ""
        root.editBusType = "audio"
        root.editBusOriginalType = "audio"
        root.editBusTemplateId = root.defaultBusTemplateId()
        root.editBusLevel = 75
        root.editBusMuted = false
        root.busDialogOpen = true
        root.busDialogIsAdd = true
    }
    // Picking a template makes its bus: the design IS the bus's windows, so there
    // is nothing else to place. Closes the dialog on success.
    function createTemplateBus(templateId) {
        const id = String(templateId === undefined || templateId === null ? "" : templateId).trim()
        if (id === "") {
            root.editBusTemplateLocked = true
            return
        }
        const design = TemplateLibraryService.designs().filter((d) => String(d.id) === id)[0]
        const name = root.editBusName.trim() !== "" ? root.editBusName
                   : String(design ? design.name : "")
        const made = BusListModel.addTemplateBus(name, id)
        if (made < 0) {
            EventBus.notify(qsTr("The template bus could not be created. Try again."),
                            "warning", qsTr("Bus"), "av.templateBus.failed")
            return
        }
        BusListModel.setLevel(made, root.editBusLevel)
        BusListModel.setMuted(made, root.editBusMuted)
        root.editBusTemplateLocked = false
        root.busDialogOpen = false
    }
    // Set when Add Bus was pressed with no template chosen: the dropdown says why
    // instead of the dialog closing on nothing.
    property bool editBusTemplateLocked: false
    function saveBusDialog() {
        if (root.busDialogIsAdd && root.editBusType === "template") {
            root.createTemplateBus(root.editBusTemplateId)
            return
        }
        if (!root.busDialogIsAdd && root.editBusType === "template"
                && String(root.editBusTemplateId || "").trim() === "") {
            root.editBusTemplateLocked = true
            return
        }
        if (root.busDialogIsAdd) {
            BusListModel.addBus(root.editBusName, root.editBusType)
            const idx = BusListModel.rowCount() - 1
            BusListModel.setLevel(idx, root.editBusLevel)
            BusListModel.setMuted(idx, root.editBusMuted)
        } else {
            if (root.editBusIndex < 0)
                return
            BusListModel.renameBus(root.editBusIndex, root.editBusName)
            BusListModel.setType(root.editBusIndex, root.editBusType)
            if (root.editBusType === "template")
                BusListModel.setTemplateId(root.editBusIndex, root.editBusTemplateId)
            BusListModel.setLevel(root.editBusIndex, root.editBusLevel)
            BusListModel.setMuted(root.editBusIndex, root.editBusMuted)
        }
        root.busDialogOpen = false
    }
    // ---- Bus templates: LISTED here, never created here -----------------------
    property int templateRev: 0
    readonly property var busTemplates: {
        root.templateRev
        const all = TemplateLibraryService.designs()
        const out = []
        for (let i = 0; i < all.length; ++i) {
            if (String(all[i].category || "") === "buses")
                out.push(all[i])
        }
        return out
    }
    // How many WINDOWS a template carries: its bus items, which is what an output
    // will fill with the feeds each one names.
    function busTemplateWindows(design) {
        const blocks = design && design.blocks ? design.blocks : []
        let n = 0
        for (let i = 0; i < blocks.length; ++i) {
            if (String(blocks[i].kind || "") === "bus")
                ++n
        }
        return n
    }
    Connections {
        target: TemplateLibraryService
        function onChanged() { root.templateRev++ }
    }

    function busRoutedAudio() {
        root.modelsRev
        return root.editBusIndex < 0 ? [] : BusListModel.getBus(root.editBusIndex).routedAudioInputs
    }
    function busRoutedVideo() {
        root.modelsRev
        return root.editBusIndex < 0 ? [] : BusListModel.getBus(root.editBusIndex).routedVideoSources
    }

    function rowRouted(kindName, rowIndex) {
        root.modelsRev
        for (let b = 0; b < BusListModel.rowCount(); ++b) {
            const bus = BusListModel.getBus(b)
            const list = kindName === "audio" ? bus.routedAudioInputs
                                              : bus.routedVideoSources
            if (list.indexOf(rowIndex) >= 0)
                return true
        }
        return false
    }

    // ---- Row removal (right-click menu + Edit dialogs' Delete) ----
    function removeAudioFixingRoutes(index) {
        for (let b = BusListModel.rowCount() - 1; b >= 0; --b)
            if (BusListModel.getBus(b).routedAudioInputs.indexOf(index) >= 0)
                BusListModel.disconnectAudioRoute(b, index)
        AudioInputListModel.removeInput(index)
        BusListModel.pruneOrphanSourceEdges()
    }
    function removeVideoFixingRoutes(index) {
        for (let b = BusListModel.rowCount() - 1; b >= 0; --b)
            if (BusListModel.getBus(b).routedVideoSources.indexOf(index) >= 0)
                BusListModel.disconnectVideoRoute(b, index)
        VideoSourceListModel.removeSource(index)
        BusListModel.pruneOrphanSourceEdges()   // same belt, video side
    }

    // Delete confirm state — one dialog serves all three columns.
    property string deleteTargetType: ""   // "audio" | "video" | "bus"
    property int deleteTargetIndex: -1
    property string deleteTargetName: ""
    function requestDelete(type, index, name) {
        root.deleteTargetType = type
        root.deleteTargetIndex = index
        root.deleteTargetName = name
    }
    function confirmDelete() {
        if (root.deleteTargetIndex < 0)
            return
        if (root.deleteTargetType === "audio")
            root.removeAudioFixingRoutes(root.deleteTargetIndex)
        else if (root.deleteTargetType === "video")
            root.removeVideoFixingRoutes(root.deleteTargetIndex)
        else if (root.deleteTargetType === "bus")
            BusListModel.deleteBus(root.deleteTargetIndex)   // the operator's delete: its notice offers the Undo
        root.deleteTargetIndex = -1
    }

    // ---- Which video rows are really delivering frames ----------------------
    property var liveVideoRows: ({})
    property int liveVideoRev: 0
    function setVideoLive(index, on) {
        if ((root.liveVideoRows[index] === true) === !!on)
            return
        const next = {}
        for (const k in root.liveVideoRows)
            next[k] = root.liveVideoRows[k]
        if (on)
            next[index] = true
        else
            delete next[index]
        root.liveVideoRows = next
        root.liveVideoRev++
    }
    function videoLive(index) {
        void root.liveVideoRev
        return root.liveVideoRows[index] === true
    }

    // ---- Drag-to-connect ----
    property bool dragConnectActive: false
    property string dragConnectFrom: ""   // "audio" | "video"
    property int dragConnectIndex: -1
    property point dragConnectStart
    property point dragConnectPos

    function dragPoint(sceneX, sceneY) {
        return board.mapFromItem(null, sceneX, sceneY)
    }
    function startDragConnect(kindName, srcIndex, sceneX, sceneY) {
        root.dragConnectFrom = kindName
        root.dragConnectIndex = srcIndex
        root.dragConnectStart = root.dragPoint(sceneX, sceneY)
        root.dragConnectPos = root.dragConnectStart
        root.dragConnectActive = true
    }
    function updateDragConnect(sceneX, sceneY) {
        root.dragConnectPos = root.dragPoint(sceneX, sceneY)
    }
    property int dragConnectFromBus: -1
    function startDragConnectFromBus(kindName, busIndex, sceneX, sceneY) {
        root.dragConnectFrom = kindName
        root.dragConnectFromBus = busIndex
        root.dragConnectStart = root.dragPoint(sceneX, sceneY)
        root.dragConnectPos = root.dragConnectStart
        root.dragConnectActive = true
    }
    function finishDragConnectFromBus(kindName, busIndex, sceneX, sceneY) {
        root.dragConnectActive = false
        root.dragConnectFromBus = -1
        const p = root.dragPoint(sceneX, sceneY)
        const s = root.sourceIndexAt(kindName, p)
        if (s < 0) {
            // Not on a source card: a VIDEO drag that ends on a TEMPLATE bus gives that
            // bus this bus's feeds (see routeBusIntoTemplate).
            if (kindName === "video") {
                const target = root.busIndexAt(p)
                if (target >= 0 && target !== busIndex) {
                    const rootRef = root
                    Qt.callLater(() => rootRef.routeBusIntoTemplate(busIndex, target))
                }
            }
            return
        }
        const refusal = BusListModel.connectRefusal(kindName, s, busIndex)
        if (refusal !== "") {
            root.refuseDrop(kindName, s, busIndex, true, refusal)
            return
        }
        const rootRef = root
        Qt.callLater(() => {
            rootRef.flashSource(kindName, s, rootRef.addRoute(kindName, busIndex, s))
        })
    }
    function routeBusIntoTemplate(fromBus, templateBus) {
        const from = BusListModel.getBus(fromBus)
        const to = BusListModel.getBus(templateBus)
        if (!from || !to || String(to.type) !== "template")
            return
        const fromName = String(from.name || "")
        const toName = String(to.name || "")
        const sources = from.routedVideoSources !== undefined ? from.routedVideoSources : []
        if (sources.length === 0) {
            EventBus.notify(qsTr("“%1” has no video source routed in, so there is no feed to give “%2”.")
                            .arg(fromName).arg(toName),
                            "info", qsTr("Audio & Video"), "av.busToTemplate.empty")
            return
        }
        let added = 0
        for (let i = 0; i < sources.length; ++i) {
            const why = BusListModel.connectRefusal("video", sources[i], templateBus)
            if (why === "already-connected") {
                ++added                    // already feeding it: that is the goal met
                continue
            }
            if (why !== "")
                continue
            if (BusListModel.connectVideoRoute(templateBus, sources[i]))
                ++added
        }
        if (added === 0) {
            EventBus.notify(qsTr("“%1” could not be routed into “%2”.")
                            .arg(fromName).arg(toName),
                            "warning", qsTr("Audio & Video"), "av.busToTemplate.refused")
            return
        }
        EventBus.notify(qsTr("“%1” now feeds “%2”.").arg(fromName).arg(toName),
                        "info", qsTr("Audio & Video"), "av.busToTemplate.ok")
    }
    function addRoute(kindName, busIndex, srcIndex) {
        return kindName === "audio"
                ? BusListModel.connectAudioRoute(busIndex, srcIndex)
                : BusListModel.connectVideoRoute(busIndex, srcIndex)
    }
    function cardIndexAt(repeater, rowCount, p) {
        if (!repeater)
            return -1
        for (let i = 0; i < repeater.count; ++i) {
            const card = repeater.itemAt(i)
            if (!card || !card.visible)
                continue
            if (i >= rowCount || card.index !== i)
                continue
            const local = card.mapFromItem(board, p.x, p.y)
            if (local.x >= -20 && local.x <= card.width + 20
                && local.y >= -6 && local.y <= card.height + 6)
                return i
        }
        return -1
    }
    function sourceIndexAt(kindName, p) {
        const isAudio = kindName === "audio"
        const count = isAudio ? AudioInputListModel.rowCount() : VideoSourceListModel.rowCount()
        const hit = root.cardIndexAt(isAudio ? audioCards : videoCards, count, p)
        if (hit >= 0)
            return hit
        const colX = isAudio ? board.audioX : board.videoX
        const colW = isAudio ? board.colW : board.vidColW
        const rowH = isAudio ? board.rowH : board.vidRowH
        if (count === 0)
            return -1
        if (p.x < colX - 20 || p.x > colX + colW + 20)
            return -1
        const relY = p.y - board.headerH
        if (relY < 0)
            return -1
        const step = rowH + board.rowGap
        return Math.max(0, Math.min(count - 1, Math.floor(relY / step)))
    }
    function lineClickedAt(x, y) {
        const pairs = board.connectorPairs()
        const SAMPLES = 17
        const CATCH = 12
        let best = -1
        let bestDist = Infinity
        for (let i = 0; i < pairs.length; ++i) {
            const p = pairs[i]
            // Same control points the drawn ShapePath uses: both control
            // x's at the horizontal midpoint, at the two endpoint y's.
            const cx = (p.x1 + p.x2) / 2
            const c1x = cx, c1y = p.y1, c2x = cx, c2y = p.y2
            for (let s = 0; s < SAMPLES; ++s) {
                const t = s / (SAMPLES - 1)
                const u = 1 - t
                // Cubic Bézier: (1-t)³P0 + 3(1-t)²tC1 + 3(1-t)t²C2 + t³P3.
                const b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t
                const px = b0 * p.x1 + b1 * c1x + b2 * c2x + b3 * p.x2
                const py = b0 * p.y1 + b1 * c1y + b2 * c2y + b3 * p.y2
                const dx = x - px, dy = y - py
                const d = dx * dx + dy * dy
                if (d < bestDist) {
                    bestDist = d
                    best = i
                }
            }
        }
        if (best >= 0 && bestDist <= CATCH * CATCH) {
            const p = pairs[best]
            root.disconnectRoute(p.kind, p.busIndex, p.srcIndex)
        }
    }

    function disconnectRoute(kindName, busIndex, srcIndex) {
        if (kindName === "audio")
            BusListModel.toggleAudioRoute(busIndex, srcIndex)
        else
            BusListModel.toggleVideoRoute(busIndex, srcIndex)
    }

    function finishDragConnect(kindName, srcIndex, sceneX, sceneY) {
        root.dragConnectActive = false
        const p = root.dragPoint(sceneX, sceneY)
        const b = root.busIndexAt(p)
        if (b < 0)
            return
        const refusal = BusListModel.connectRefusal(kindName, srcIndex, b)
        if (refusal !== "") {
            root.refuseDrop(kindName, srcIndex, b, false, refusal)   // red card + the reason
            return
        }
        const rootRef = root
        Qt.callLater(() => {
            rootRef.flashBus(b, rootRef.addRoute(kindName, b, srcIndex))
        })
    }
    property int flashBusIndex: -1
    property bool flashBusOk: false
    function flashBus(busIndex, ok) {
        root.flashBusIndex = busIndex
        root.flashBusOk = ok
        dropFlashTimer.restart()
    }
    Timer {
        id: dropFlashTimer
        interval: 600
        onTriggered: {
            root.flashBusIndex = -1
            root.flashSourceIndex = -1
        }
    }

    property string flashSourceKind: ""
    property int flashSourceIndex: -1
    property bool flashSourceOk: false
    function flashSource(kindName, rowIndex, ok) {
        root.flashSourceKind = kindName
        root.flashSourceIndex = rowIndex
        root.flashSourceOk = ok
        dropFlashTimer.restart()   // one timer for both ends: one gesture, one pulse
    }
    // Is this source row the live TARGET of a bus-originated drag?
    function sourceDragHover(kindName, rowIndex) {
        return root.dragConnectActive && root.dragConnectFromBus >= 0
                && root.dragConnectFrom === kindName
                && root.dragConnectHoverSource === rowIndex
    }
    // Is this source row showing a finished drop's own answer?
    function sourceFlashOn(kindName, rowIndex) {
        return root.flashSourceKind === kindName && root.flashSourceIndex === rowIndex
    }

    function dropRefusalTextFor(code, kindName, srcIndex, busIndex) {
        const busName = (BusListModel.getBus(busIndex) || {}).name ?? ""
        const src = kindName === "audio" ? (AudioInputListModel.getInput(srcIndex) || {})
                                         : (VideoSourceListModel.getSource(srcIndex) || {})
        const srcName = src.name ?? ""
        switch (code) {
        case "bus-has-video":
            return qsTr("\u201C%1\u201D already carries a video source. Remove that route first \u2014 a bus shows one video source at a time.").arg(busName)
        case "template-bus-has-audio":
            return qsTr("\u201C%1\u201D is a template bus \u2014 it takes one audio input. Remove the input it already carries first.").arg(busName)
        case "template-has-no-windows":
            return qsTr("\u201C%1\u201D is built from a template that has no bus items, so there is no window to show this feed. Add bus items to the template in Edit \u00b7 Templates under Buses first.").arg(busName)
        case "already-connected":
            return qsTr("\u201C%1\u201D is already routed into \u201C%2\u201D.").arg(srcName).arg(busName)
        case "audio-only-bus":
            return qsTr("\u201C%1\u201D is an audio-only bus \u2014 it has no video to route into.").arg(busName)
        case "engine-refused":
            return qsTr("The engine refused that route into \u201C%1\u201D (signal type or a loop).").arg(busName)
        case "no-source-row":
            return qsTr("The board moved under the drag \u2014 that row is not where the drop landed. Drag again.")
        default:
            return qsTr("That route was refused (%1).").arg(code === "" ? qsTr("no reason reported") : code)
        }
    }
    function refuseDrop(kindName, srcIndex, busIndex, refusedSourceEnd, code) {
        if (refusedSourceEnd)
            root.flashSource(kindName, srcIndex, false)
        else
            root.flashBus(busIndex, false)
        const refusal = (code === undefined || code === "")
                        ? BusListModel.connectRefusal(kindName, srcIndex, busIndex) : code
        EventBus.notify(root.dropRefusalTextFor(refusal, kindName, srcIndex, busIndex),
                        "warning", qsTr("Audio & Video"), "ui.avboard.routeRefused")
    }
    function busIndexAt(p) {
        const count = BusListModel.rowCount()
        const hit = root.cardIndexAt(busCards, count, p)
        if (hit >= 0)
            return hit
        if (count === 0)
            return -1
        if (p.x < board.busX - 20 || p.x > board.busX + board.busColW + 20)
            return -1
        const relY = p.y - board.headerH
        if (relY < 0)
            return -1
        const step = board.rowH + board.rowGap
        return Math.max(0, Math.min(count - 1, Math.floor(relY / step)))
    }
    readonly property int dragConnectHoverBus:
        root.dragConnectActive && root.dragConnectFromBus < 0
            ? root.busIndexAt(root.dragConnectPos) : -1
    // The inverse, for a bus-originated drag.
    readonly property int dragConnectHoverSource:
        root.dragConnectActive && root.dragConnectFromBus >= 0
            ? root.sourceIndexAt(root.dragConnectFrom, root.dragConnectPos) : -1
    function dropAllowed(kindName, srcIndex, busIndex) {
        return BusListModel.canConnect(kindName, srcIndex, busIndex)
    }
    readonly property bool dragConnectCompat: {
        if (root.dragConnectFromBus >= 0) {
            if (root.dragConnectHoverSource < 0)
                return false
            return root.dropAllowed(root.dragConnectFrom, root.dragConnectHoverSource,
                                    root.dragConnectFromBus)
        }
        if (root.dragConnectHoverBus < 0)
            return false
        return root.dropAllowed(root.dragConnectFrom, root.dragConnectIndex,
                                root.dragConnectHoverBus)
    }

    Text {
        id: pageHeader
        x: Theme.space6
        y: Theme.space6
        text: qsTr("Audio & Video")
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXl
        font.weight: Font.DemiBold
    }

    // The board sits on the same checkerboard canvas Settings · Routing uses, so the two wiring
    // pages read as one family; the page scrolls inside it when the rosters grow long.
    Rectangle {
        id: canvas
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6 + Theme.space2
        anchors.top: pageHeader.bottom
        anchors.topMargin: Theme.space3
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6
        radius: Theme.radiusLg
        color: Theme.windowBg
        border.color: Theme.border
        border.width: 1
        clip: true

        Checkerboard {
            anchors.fill: parent
            tileSize: 13
        }
    }

    Flickable {
        id: flick
        anchors.fill: canvas
        anchors.margins: 1
        contentWidth: width
        contentHeight: layout.height + Theme.space4 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: !root.dragConnectActive

        Column {
            id: layout
            x: Theme.space5
            y: Theme.space4
            width: flick.width - Theme.space5 * 2
            spacing: Theme.space6

            // ---- Routing board ----
            Item {
                id: board
                objectName: "selfTestAvBoard"

                width: layout.width
                // Same gutter and card width cap as Settings · Routing, so the two wiring pages carry cards of one size.
                readonly property real gutterW: 44
                readonly property real columnsRoom: Math.max(240, board.width - board.gutterW * 2)
                readonly property real wantColW: 240
                readonly property real wantBusColW: 240
                // The video column is the SAME width as the header chip above it (it used to run 36px wider, past the chip); its thumbnail shrinks instead.
                readonly property real wantVidColW: 240
                readonly property real colScale: Math.min(1, columnsRoom / (wantColW + wantBusColW + wantVidColW))
                readonly property real colW: wantColW * colScale
                readonly property real busColW: wantBusColW * colScale
                readonly property real vidColW: wantVidColW * colScale
                readonly property int rowH: 64
                readonly property int vidRowH: board.rowH
                readonly property int rowGap: Theme.space3
                readonly property int headerBoxH: 40
                readonly property int headerH: headerBoxH + rowGap
                // The three columns are centred as a group (as Routing centres its own), so a wide
                // window grows the margins, not the cards.
                readonly property real groupLeft: Math.max(0, (board.width - (board.colW + board.busColW + board.vidColW + board.gutterW * 2)) / 2)
                readonly property real audioX: board.groupLeft
                readonly property real busX: board.audioX + board.colW + board.gutterW
                readonly property real videoX: board.busX + board.busColW + board.gutterW

                function audioPortY(i) {
                    return board.headerH + i * (board.rowH + board.rowGap) + board.rowH / 2
                }
                function busPortY(i) {
                    return board.headerH + i * (board.rowH + board.rowGap) + board.rowH / 2
                }
                function videoPortY(i) {
                    return board.headerH + i * (board.vidRowH + board.rowGap) + board.vidRowH / 2
                }
                function connectorPairs() {
                    root.modelsRev
                    const pairs = []
                    const busCount = BusListModel.rowCount()
                    const audioRows = AudioInputListModel.rowCount()
                    const videoRows = VideoSourceListModel.rowCount()
                    for (let b = 0; b < busCount; ++b) {
                        const bus = BusListModel.getBus(b)
                        const by = board.busPortY(b)
                        for (const ai of bus.routedAudioInputs) {
                            if (ai < 0 || ai >= audioRows)
                                continue
                            pairs.push({ x1: board.audioX + board.colW, y1: board.audioPortY(ai),
                                         x2: board.busX, y2: by, color: Theme.success,
                                         kind: "audio", busIndex: b, srcIndex: ai })
                        }
                        for (const vi of bus.routedVideoSources) {
                            if (vi < 0 || vi >= videoRows)
                                continue
                            pairs.push({ x1: board.videoX, y1: board.videoPortY(vi),
                                         x2: board.busX + board.busColW, y2: by,
                                         color: bus.type === "audio" ? Theme.danger : Theme.info,
                                         kind: "video", busIndex: b, srcIndex: vi })
                        }
                    }
                    return pairs
                }

                height: {
                    root.modelsRev
                    // Each column spans with its own row height; the board
                    // must fit the tallest.
                    return Math.max(board.headerH + AudioInputListModel.rowCount() * (board.rowH + board.rowGap),
                                    board.headerH + BusListModel.rowCount() * (board.rowH + board.rowGap),
                                    board.headerH + VideoSourceListModel.rowCount() * (board.vidRowH + board.rowGap),
                                    board.headerH + board.rowH)
                }

                Repeater {
                    model: board.connectorPairs()
                    delegate: Shape {
                        id: connector
                        required property var modelData
                        anchors.fill: parent
                        z: -1
                        preferredRendererType: Shape.CurveRenderer

                        readonly property bool isAudio: connector.modelData.kind === "audio"
                        readonly property real srcLevel: {
                            if (!connector.isAudio)
                                return 0
                            void root.audioMeterList
                            void root.modelsRev
                            const data = AudioInputListModel.getInput(connector.modelData.srcIndex)
                            if (!data)
                                return 0
                            return root.inputMixLevel(connector.modelData.srcIndex, data.sublabel)
                        }
                        readonly property bool busMuted: {
                            void root.modelsRev
                            const b = BusListModel.getBus(connector.modelData.busIndex)
                            return !!b && b.muted === true
                        }
                        // An audio wire flows whenever its input is delivering ANY signal (a hair above silence), not only
                        // once it is loud: the dots used to appear only past a 1% level, so a quiet voice or a distant mic
                        // showed a dead wire. How strongly they read follows the level (see dotColor / dotWidth).
                        readonly property bool flows: !connector.busMuted
                            && (connector.isAudio ? connector.srcLevel > 0.0008
                                                  : root.videoLive(connector.modelData.srcIndex))
                        // 0 (barely there) .. 1 (loud). The meter is perceptual, so a gentle gain already reads well.
                        readonly property real strength: connector.isAudio ? Math.min(1, connector.srcLevel * 5) : 1
                        // Low level = a faint but VISIBLE dot (never below ~38% alpha), full brightness when loud.
                        readonly property color dotColor: {
                            const base = Qt.lighter(connector.modelData.color, 1.55)
                            return Qt.rgba(base.r, base.g, base.b, 0.38 + 0.62 * connector.strength)
                        }
                        readonly property real dotWidth: connector.isAudio ? 2.6 + 1.4 * connector.strength : 2.2
                        readonly property real period: connector.isAudio ? 3.6 : 10
                        readonly property int cycleMs: connector.isAudio ? 800 : 700

                        ShapePath {
                            strokeColor: connector.modelData.color
                            strokeWidth: 2
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            startX: connector.modelData.x1
                            startY: connector.modelData.y1
                            PathCubic {
                                x: connector.modelData.x2
                                y: connector.modelData.y2
                                control1X: connector.modelData.x1 + (connector.modelData.x2 - connector.modelData.x1) * 0.5
                                control1Y: connector.modelData.y1
                                control2X: connector.modelData.x1 + (connector.modelData.x2 - connector.modelData.x1) * 0.5
                                control2Y: connector.modelData.y2
                            }
                        }

                        ShapePath {
                            strokeColor: connector.flows ? connector.dotColor : "transparent"
                            strokeWidth: connector.dotWidth
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            strokeStyle: ShapePath.DashLine
                            dashPattern: connector.isAudio ? [0.05, 3.55] : [6, 4]
                            dashOffset: 0
                            NumberAnimation on dashOffset {
                                from: connector.period
                                to: 0
                                duration: connector.cycleMs
                                loops: Animation.Infinite
                                running: connector.flows
                            }
                            startX: connector.modelData.x1
                            startY: connector.modelData.y1
                            PathCubic {
                                x: connector.modelData.x2
                                y: connector.modelData.y2
                                control1X: connector.modelData.x1 + (connector.modelData.x2 - connector.modelData.x1) * 0.5
                                control1Y: connector.modelData.y1
                                control2X: connector.modelData.x1 + (connector.modelData.x2 - connector.modelData.x1) * 0.5
                                control2Y: connector.modelData.y2
                            }
                        }
                    }
                }

                // ---- Line hit-testing — ONE layer for ALL lines, sitting
                MouseArea {
                    id: lineHitLayer
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    cursorShape: Qt.ArrowCursor

                    onClicked: (mouse) => root.lineClickedAt(mouse.x, mouse.y)
                }

                // ---- Audio Inputs column ----
                Column {
                    x: board.audioX
                    y: 0
                    width: board.colW
                    spacing: board.rowGap

                    Item {
                        width: board.colW
                        height: board.headerBoxH

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusMd
                            color: Theme.inset
                            border.width: 1
                            border.color: Theme.border
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("AUDIO INPUTS")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                        }

                        AppButton {
                            objectName: "selfTestAvAddAudio"
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            text: "+"
                            fontSize: 20
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddSource("audio")
                        }
                    }

                    Repeater {
                        id: audioCards
                        model: AudioInputListModel

                        delegate: Rectangle {
                            id: inRow
                            required property int index
                            required property string name
                            required property string sublabel
                            required property real level
                            required property bool muted

                            InViewProbe {
                                id: inRowProbe
                                target: inRow
                                onInViewChanged: root.setRowInView("audio", inRow.index, inRowProbe.inView)
                            }

                            width: board.colW
                            height: board.rowH
                            radius: Theme.radiusMd
                            color: root.sourceDragHover("audio", inRow.index)
                                   ? (root.dragConnectCompat
                                      ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
                                      : Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15))
                                   : Theme.inset
                            border.width: (root.sourceDragHover("audio", inRow.index)
                                           || root.sourceFlashOn("audio", inRow.index)) ? 1.5 : 0
                            border.color: root.sourceFlashOn("audio", inRow.index)
                                          ? (root.flashSourceOk ? Theme.success : Theme.danger)
                                          : root.dragConnectCompat ? Theme.accent : Theme.danger

                            // Right-click: Edit / Duplicate / Delete — the
                            // same menu every row in this board gets.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    rowMenu.openAt(inRow, mouse.x, mouse.y, root.Window.contentItem)
                                    rowMenu.rowKind = "audio"
                                    rowMenu.rowIndex = inRow.index
                                    rowMenu.rowName = inRow.name
                                }
                            }

                            StatusDot {
                                connected: root.rowRouted("audio", inRow.index)
                                accent: Theme.success
                            }

                            Text {
                                x: 28; y: 8
                                width: board.colW - 100
                                text: inRow.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            // No LIVE pill: the dot, the meter and the wire already say it is live. Only the
                            // exception is written — a muted input says so (mute itself is in Edit).
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                visible: inRow.muted
                                text: qsTr("MUTED")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }

                            Text {
                                x: 28; y: 28
                                width: 150
                                visible: inRow.sublabel !== ""
                                text: inRow.sublabel
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                elide: Text.ElideRight
                            }

                            LevelTrack {
                                x: 28; y: 48
                                width: board.colW - 90
                                value: {
                                    if (inRow.sublabel === "")
                                        return 0
                                    return Math.min(100, root.inputMixLevel(inRow.index, inRow.sublabel) * 100)
                                }
                                // Color follows the VALUE (VU zones) — the
                                // old hardcoded green read "safe" at 100.
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 8
                                text: qsTr("Edit")
                                color: Theme.accentLight
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Medium

                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openEditAudio(inRow.index)
                                }
                            }

                            PortDot {
                                id: inPortDot
                                objectName: "selfTestAudioPort_" + inRow.index
                                anchors.right: parent.right
                                anchors.rightMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.success
                                dragActive: root.dragConnectActive && root.dragConnectFrom === "audio"
                                            && root.dragConnectIndex === inRow.index
                                onConnectStarted: (x, y) => root.startDragConnect("audio", inRow.index, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(x, y)
                                onConnectFinished: (x, y) => root.finishDragConnect("audio", inRow.index, x, y)
                            }
                        }
                    }
                }

                // ---- Buses & Routing column ----
                Column {
                    x: board.busX
                    y: 0
                    width: board.busColW
                    spacing: board.rowGap

                    Item {
                        width: board.busColW
                        height: board.headerBoxH

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusMd
                            color: Theme.inset
                            border.width: 1
                            border.color: Theme.border
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("BUSES & ROUTING")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                        }

                        AppButton {
                            objectName: "selfTestAvAddBus"
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            text: "+"
                            fontSize: 20
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddBus()
                        }
                    }

                    Repeater {
                        id: busCards
                        model: BusListModel

                        delegate: Rectangle {
                            id: busRow
                            objectName: "selfTestBusCard_" + busRow.index
                            required property int index
                            required property string name
                            required property string type
                            required property real level
                            required property bool muted
                            required property var routedAudioInputs
                            required property var routedVideoSources

                            readonly property color typeColor: busRow.type === "audio" ? Theme.success
                                                              : busRow.type === "video" ? Theme.info
                                                              : busRow.type === "template" ? Theme.wireStyle
                                                              : Theme.accent
                            readonly property string typeLabel: busRow.type === "audio" ? qsTr("AUDIO")
                                                               : busRow.type === "video" ? qsTr("VIDEO")
                                                               : busRow.type === "template" ? qsTr("TEMPLATE")
                                                               : qsTr("BOTH")
                            readonly property int srcCount: busRow.routedAudioInputs.length + busRow.routedVideoSources.length

                            width: board.busColW
                            height: board.rowH
                            radius: Theme.radiusMd
                            color: {
                                if (root.dragConnectActive && root.dragConnectHoverBus === busRow.index)
                                    return root.dragConnectCompat
                                           ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
                                           : Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                                return busRow.muted ? Theme.chip : Theme.inset
                            }
                            border.width: (root.dragConnectActive && root.dragConnectHoverBus === busRow.index)
                                          || root.flashBusIndex === busRow.index ? 1.5 : 0
                            border.color: root.flashBusIndex === busRow.index
                                          ? (root.flashBusOk ? Theme.success : Theme.danger)
                                          : root.dragConnectCompat ? Theme.accent : Theme.danger
                            opacity: busRow.muted ? 0.7 : 1
                            Behavior on opacity { NumberAnimation { duration: 120 } }

                            // Right-click: Edit / Duplicate / Delete.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    rowMenu.openAt(busRow, mouse.x, mouse.y, root.Window.contentItem)
                                    rowMenu.rowKind = "bus"
                                    rowMenu.rowIndex = busRow.index
                                    rowMenu.rowName = busRow.name
                                }
                            }

                            Pill {
                                x: 12; y: 8
                                text: busRow.typeLabel
                                baseColor: busRow.typeColor
                                lightColor: busRow.typeColor
                                tint: true
                                tintAlpha: 0.18
                                fontSize: 9
                            }

                            Text {
                                x: 12; y: 30
                                width: board.busColW - 90
                                text: busRow.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            // No LIVE pill (the wires and the sources feeding the bus show it); a muted bus
                            // says so, next to its source count.
                            Text {
                                id: busSrcCount
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                text: busRow.srcCount + qsTr(" src")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                            Text {
                                anchors.right: busSrcCount.left
                                anchors.rightMargin: 10
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                visible: busRow.muted
                                text: qsTr("MUTED")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }

                            // (No level meter on a bus card: the sources feeding it and the wires into it
                            // already move when there is signal, so a third bar said nothing new.)

                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 8
                                text: qsTr("Edit")
                                color: Theme.accentLight
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Medium

                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openEditBus(busRow.index)
                                }
                            }

                            PortDot {
                                id: busAudioPortDot
                                objectName: "selfTestBusAudioPort_" + busRow.index
                                anchors.left: parent.left
                                anchors.leftMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.success
                                alwaysColored: true
                                onConnectStarted: (x, y) => root.startDragConnectFromBus("audio", busRow.index, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(x, y)
                                onConnectFinished: (x, y) => root.finishDragConnectFromBus("audio", busRow.index, x, y)
                            }
                            PortDot {
                                id: busVideoPortDot
                                objectName: "selfTestBusVideoPort_" + busRow.index
                                visible: busRow.type !== "audio"
                                anchors.right: parent.right
                                anchors.rightMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.info
                                alwaysColored: true
                                onConnectStarted: (x, y) => root.startDragConnectFromBus("video", busRow.index, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(x, y)
                                onConnectFinished: (x, y) => root.finishDragConnectFromBus("video", busRow.index, x, y)
                            }
                        }
                    }
                }

                // ---- Video Sources column ----
                Column {
                    x: board.videoX
                    y: 0
                    width: board.colW
                    spacing: board.rowGap

                    Item {
                        width: board.colW
                        height: board.headerBoxH

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusMd
                            color: Theme.inset
                            border.width: 1
                            border.color: Theme.border
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("VIDEO SOURCES")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                        }

                        AppButton {
                            objectName: "selfTestAvAddVideo"
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            text: "+"
                            fontSize: 20
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddSource("video")
                        }
                    }

                    Repeater {
                        id: videoCards
                        model: VideoSourceListModel

                        delegate: Rectangle {
                            id: vidRow
                            required property int index
                            required property string name
                            required property string sublabel
                            required property string kind
                            required property bool muted
                            required property real level

                            readonly property bool hasAudio: vidRow.kind === "media"
                                                             || vidRow.kind === "ndi"
                            readonly property url liveThumb:
                                vidThumb.liveCam && vidThumb.activeTapLabel === vidRow.sublabel
                                ? "image://videopreview/" + encodeURIComponent(vidRow.sublabel)
                                  + "?n=" + vidThumb.thumbNonce : ""

                            // Delivering frames right now (a live thumbnail), or a media row that is not
                            // paused — what the wire into the bus reads to decide whether to animate.
                            readonly property bool feedLive: !vidRow.muted
                                && (vidRow.kind === "media" || vidThumb.thumbLive)
                            onFeedLiveChanged: root.setVideoLive(vidRow.index, vidRow.feedLive)
                            Component.onCompleted: root.setVideoLive(vidRow.index, vidRow.feedLive)
                            Component.onDestruction: root.setVideoLive(vidRow.index, false)

                            width: board.vidColW
                            height: board.vidRowH
                            radius: Theme.radiusMd
                            color: root.sourceDragHover("video", vidRow.index)
                                   ? (root.dragConnectCompat
                                      ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
                                      : Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15))
                                   : Theme.inset
                            border.width: (root.sourceDragHover("video", vidRow.index)
                                           || root.sourceFlashOn("video", vidRow.index)) ? 1.5 : 0
                            border.color: root.sourceFlashOn("video", vidRow.index)
                                          ? (root.flashSourceOk ? Theme.success : Theme.danger)
                                          : root.dragConnectCompat ? Theme.accent : Theme.danger

                            // Right-click: Edit / Duplicate / Delete.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    rowMenu.openAt(vidRow, mouse.x, mouse.y, root.Window.contentItem)
                                    rowMenu.rowKind = "video"
                                    rowMenu.rowIndex = vidRow.index
                                    rowMenu.rowName = vidRow.name
                                }
                            }

                            StatusDot {
                                connected: root.rowRouted("video", vidRow.index)
                                accent: Theme.info
                            }

                            Text {
                                x: 28; y: 12
                                width: vidThumb.x - 28 - 12
                                text: vidRow.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            Text {
                                x: 28; y: 30
                                width: vidThumb.x - 28 - 12
                                visible: vidRow.sublabel !== ""
                                text: vidRow.sublabel
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                elide: Text.ElideRight
                            }

Text {
                                anchors.right: vidThumb.left
                                anchors.rightMargin: 14
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                // A paused camera/screen, or a muted media/NDI row — only the exception shows.
                                visible: vidRow.muted
                                text: vidRow.hasAudio ? qsTr("MUTED") : qsTr("PAUSED")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }

                            LevelTrack {
                                objectName: "selfTestVideoRowMeter_" + vidRow.index
                                x: 28
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 14
                                visible: vidRow.hasAudio
                                // Stop short of the Edit link's hover area.
                                width: vidThumb.x - 28 - 44
                                value: {
                                    void root.audioMeterList
                                    return root.videoMixLevel(vidRow.index, vidRow.sublabel,
                                                              vidRow.kind, vidRow.muted,
                                                              vidRow.level)
                                }
                                fillColor: Theme.info
                            }

                            // ---- Preview thumb — every video source has
                            Rectangle {
                                id: vidThumb
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.top: parent.top
                                anchors.topMargin: 12
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 12
                                width: Math.min(80, Math.round((height) * 16 / 9))
                                radius: Theme.radiusSm
                                color: Theme.windowBg
                                border.width: 1
                                border.color: Theme.border
                                clip: true
                                InViewProbe {
                                    id: vidProbe
                                    target: vidThumb
                                    onInViewChanged: {
                                        root.setRowInView("video", vidRow.index, vidProbe.inView)
                                        vidThumb.recomputeLiveCam()
                                    }
                                }

                                Image {
                                    id: frameImg
                                    anchors.fill: parent
                                    source: vidRow.liveThumb
                                    visible: (vidRow.kind === "camera" || vidRow.kind === "screen"
                                              || vidRow.kind === "ndi")
                                             && status === Image.Ready && sourceSize.width > 1
                                             && (vidRow.kind === "ndi" || !vidRow.muted)
                                    fillMode: vidRow.kind === "screen" ? Image.PreserveAspectFit
                                                                       : Image.PreserveAspectCrop
                                    cache: false
                                }
                                // WHETHER THIS ROW HOLDS A LIVE TAP. It used to be a binding over the row's kind,
                                // sublabel, mute, the platform capabilities and the in-view probe, and QML kept
                                // reporting "Binding loop detected for property liveCam": the probe's change
                                // handler, the tap start and the model's own change signals all read it back
                                // while it was being re-evaluated. A plain property that is recomputed
                                // explicitly (below) on exactly those changes cannot loop.
                                property bool liveCam: false
                                function recomputeLiveCam() {
                                    const kind = vidRow.kind
                                    const caps = EngineBridge.platformCapabilities
                                    const want = (kind === "camera" || kind === "screen" || kind === "ndi")
                                        && vidRow.sublabel !== ""
                                        && (kind === "ndi"
                                            || (kind === "camera" && caps.videoCapture === true)
                                            || (kind === "screen" && caps.screenCapture === true))
                                        && (kind === "ndi" || !vidRow.muted)
                                        && vidProbe.inView
                                    if (vidThumb.liveCam !== want)
                                        vidThumb.liveCam = want
                                }
                                readonly property bool thumbLive:
                                    frameImg.status === Image.Ready && frameImg.sourceSize.width > 1
                                readonly property string tapFailure: {
                                    void vidThumb.thumbNonce
                                    if (!vidThumb.liveCam || vidThumb.thumbLive)
                                        return ""
                                    return EngineBridge.previewFailure(vidRow.sublabel)
                                }
                                property int thumbNonce: 0
                                property string activeTapLabel: ""
                                property string activeTapKind: ""
                                function syncTap() {
                                    const want = vidThumb.liveCam ? vidRow.sublabel : ""
                                    if (want === activeTapLabel)
                                        return
                                    const old = activeTapLabel
                                    const oldKind = activeTapKind
                                    if (old !== "") {
                                        if (oldKind === "screen")
                                            EngineBridge.stopScreenPreview(old, "board")
                                        else if (oldKind === "ndi")
                                            EngineBridge.stopNdiPreview(old, "board")
                                        else
                                            EngineBridge.stopVideoPreview(old, "board")
                                    }
                                    activeTapLabel = ""
                                    activeTapKind = ""
                                    if (want === "") return

                                    let accepted = false
                                    if (vidRow.kind === "screen")
                                        accepted = EngineBridge.startScreenPreview(want, "board")
                                    else if (vidRow.kind === "ndi")
                                        accepted = EngineBridge.startNdiPreview(want, "board")
                                    else
                                        accepted = EngineBridge.startVideoPreview(want, "", "board")
                                    // Screen rosters can still be loading on first entry.
                                    // Leave the request unclaimed so devicesChanged can retry.
                                    if (accepted) {
                                        activeTapLabel = want
                                        activeTapKind = vidRow.kind
                                    }
                                }
                                Connections {
                                    target: EngineBridge
                                    function onDevicesChanged() {
                                        vidThumb.recomputeLiveCam()   // the capabilities arrive with the device roster
                                        Qt.callLater(vidThumb.syncTap)
                                    }
                                }
                                Timer {
                                    interval: 125   // 8 fps thumbnail pump
                                    repeat: true
                                    running: vidThumb.liveCam && root.visible
                                    onTriggered: vidThumb.thumbNonce++
                                }
                                Component.onCompleted: {
                                    vidThumb.recomputeLiveCam()
                                    vidThumb.syncTap()
                                }
                                onLiveCamChanged: vidThumb.syncTap()
                                // kind / sublabel / muted live on the delegate (vidRow), not on this
                                // Rectangle — observe them explicitly.
                                Connections {
                                    target: vidRow
                                    function onKindChanged() { vidThumb.recomputeLiveCam() }
                                    function onSublabelChanged() {
                                        vidThumb.recomputeLiveCam()
                                        Qt.callLater(vidThumb.syncTap)   // a re-pointed row keeps liveCam but changes its tap
                                    }
                                    function onMutedChanged() { vidThumb.recomputeLiveCam() }
                                }

                                // Camera — a lens ring + center dot (warm-up only).
                                Item {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "camera" && !vidThumb.thumbLive
                                             && vidThumb.tapFailure === ""

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18; height: 18; radius: 9
                                        color: "transparent"
                                        border.width: 1.6
                                        border.color: Theme.textMuted
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 6; height: 6; radius: 3
                                        color: Theme.textMuted
                                    }
                                }

                                Item {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "ndi" && !vidThumb.thumbLive

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 5; height: 5; radius: 2.5
                                        color: Theme.textMuted
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 16; height: 16; radius: 8
                                        color: "transparent"
                                        border.width: 1.4
                                        border.color: Theme.textMuted
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 26; height: 26; radius: 13
                                        color: "transparent"
                                        border.width: 1.2
                                        border.color: Theme.textMuted
                                    }
                                }

                                Item {
                                    anchors.centerIn: parent
                                    width: 22; height: 16
                                    visible: vidRow.kind === "screen" && !vidThumb.thumbLive
                                             && vidThumb.tapFailure === ""

                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.top: parent.top
                                        width: 22; height: 14; radius: 2
                                        color: "transparent"
                                        border.width: 1.6
                                        border.color: Theme.textMuted
                                    }
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.bottom: parent.bottom
                                        width: 10; height: 2; radius: 1
                                        color: Theme.textMuted
                                    }
                                }

                                Item {
                                    anchors.centerIn: parent
                                    visible: vidThumb.tapFailure !== ""
                                    width: 22; height: 22

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 22; height: 22; radius: 11
                                        color: Qt.rgba(1, 0.42, 0.38, 0.10)
                                        border.width: 1
                                        border.color: Qt.rgba(1, 0.42, 0.38, 0.28)
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18; height: 2
                                        rotation: 45
                                        radius: 1
                                        color: Theme.dangerLight
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 3
                                    width: parent.width - 8
                                    visible: vidThumb.tapFailure !== ""
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    text: qsTr("preview failed — %1").arg(vidThumb.tapFailure)
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily; font.pixelSize: 8
                                }
                                // Media — the file's own picture (the same engine-made thumbnail the Media pane
                                // shows: a frame from the video / the still itself), loaded off the interface thread
                                // and only while the row is on screen. The play triangle below is what a file with no
                                // picture yet (still being made, or unreadable) shows.
                                Image {
                                    id: mediaImg
                                    anchors.fill: parent
                                    asynchronous: true
                                    cache: true
                                    fillMode: Image.PreserveAspectCrop
                                    source: vidRow.kind === "media" && vidRow.sublabel !== "" && vidProbe.inView
                                            ? "image://mediathumb/250/-1/" + encodeURIComponent(vidRow.sublabel) : ""
                                    // The provider answers "no picture" with a 1x1 transparent image.
                                    visible: vidRow.kind === "media" && status === Image.Ready && sourceSize.width > 1
                                }
                                // Media with no picture — a play triangle (Shapes is already imported for the
                                // connector lines).
                                Shape {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "media" && !mediaImg.visible
                                    width: 14; height: 16
                                    preferredRendererType: Shape.CurveRenderer

                                    ShapePath {
                                        fillColor: Theme.textMuted
                                        strokeColor: "transparent"
                                        startX: 0; startY: 0
                                        PathLine { x: 0; y: 16 }
                                        PathLine { x: 14; y: 8 }
                                        PathLine { x: 0; y: 0 }
                                    }
                                }
                            }

                            Text {
                                anchors.right: vidThumb.left
                                anchors.rightMargin: 14
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 10
                                text: qsTr("Edit")
                                color: Theme.accentLight
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Medium

                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openEditVideo(vidRow.index)
                                }
                            }

                            PortDot {
                                id: vidPortDot
                                objectName: "selfTestVideoPort_" + vidRow.index
                                anchors.left: parent.left
                                anchors.leftMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.info
                                dragActive: root.dragConnectActive && root.dragConnectFrom === "video"
                                            && root.dragConnectIndex === vidRow.index
                                onConnectStarted: (x, y) => root.startDragConnect("video", vidRow.index, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(x, y)
                                onConnectFinished: (x, y) => root.finishDragConnect("video", vidRow.index, x, y)
                            }
                        }
                    }
                }

                // ---- Drag-connect ghost line ----
                Shape {
                    visible: root.dragConnectActive
                    anchors.fill: parent
                    z: 10
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        strokeColor: {
                            const over = root.dragConnectFromBus >= 0
                                       ? root.dragConnectHoverSource >= 0
                                       : root.dragConnectHoverBus >= 0
                            if (!over)
                                return Theme.textMuted
                            return root.dragConnectCompat ? Theme.success : Theme.danger
                        }
                        strokeWidth: 2
                        strokeStyle: ShapePath.DashLine
                        dashPattern: [ 4, 4 ]
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        startX: root.dragConnectStart.x
                        startY: root.dragConnectStart.y
                        PathCubic {
                            x: root.dragConnectPos.x
                            y: root.dragConnectPos.y
                            control1X: root.dragConnectStart.x + (root.dragConnectPos.x - root.dragConnectStart.x) * 0.5
                            control1Y: root.dragConnectStart.y
                            control2X: root.dragConnectStart.x + (root.dragConnectPos.x - root.dragConnectStart.x) * 0.5
                            control2Y: root.dragConnectPos.y
                        }
                    }
                }
            }
        }
    }

    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: canvas.y
        height: canvas.height
        flickable: flick
    }

    // ---- Row context menu — one shared instance, retargeted per right-
    MenuCatcher {
        menu: rowMenu
    }

    DropdownPanel {
        id: rowMenu
        z: 50
        visible: false
        // "audio" | "video" | "bus" — set by the invoking card.
        property string rowKind: ""
        property int rowIndex: -1
        property string rowName: ""

        model: [
            { label: qsTr("Edit") },
            { label: qsTr("Duplicate") },
            { divider: true },
            { label: qsTr("Delete"), danger: true }
        ]
        onItemActivated: (label) => {
            const idx = rowMenu.rowIndex
            switch (label) {
            case qsTr("Edit"):
                if (rowMenu.rowKind === "audio") root.openEditAudio(idx)
                else if (rowMenu.rowKind === "video") root.openEditVideo(idx)
                else if (rowMenu.rowKind === "bus") root.openEditBus(idx)
                break
            case qsTr("Duplicate"):
                if (rowMenu.rowKind === "audio") AudioInputListModel.duplicateInput(idx)
                else if (rowMenu.rowKind === "video") VideoSourceListModel.duplicateSource(idx)
                else if (rowMenu.rowKind === "bus") BusListModel.duplicateBus(idx)
                break
            case qsTr("Delete"):
                root.requestDelete(rowMenu.rowKind, idx, rowMenu.rowName)
                break
            }
            rowMenu.visible = false
        }
    }

    // ---- Delete confirm — one dialog serves all three columns.
    ConfirmDialog {
        id: rowDeleteDialog
        // Sits over an open source/bus editor when Delete is pressed there.
        z: 30010
        shown: root.deleteTargetIndex >= 0
        title: root.deleteTargetType === "bus" ? qsTr("Delete bus?")
             : root.deleteTargetType === "video" ? qsTr("Delete source?")
             : qsTr("Delete input?")
        message: root.deleteTargetIndex < 0 ? ""
             : qsTr("\u201C%1\u201D will be removed%2.").arg(root.deleteTargetName)
               .arg(root.deleteTargetType === "bus"
                    ? qsTr(" — anything routed into it stops flowing")
                    : qsTr(" from the routing board"))
        confirmLabel: qsTr("Delete")
        onConfirmed: root.confirmDelete()
        onDismissed: root.deleteTargetIndex = -1
    }

    // ---- The Kind row, on the form's label rail ---------------------------
    component KindRailRow: Item {
        id: kindRow
        // Which chip is lit, and the roster of {"key", "label"} choices.
        required property string current
        required property var choices
        signal picked(string key)
        // The label the other fields in these dialogs use for this row.
        property string label: qsTr("Kind")
        property string railLabel: label

        width: parent ? parent.width : 0
        height: 26

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: kindRow.railLabel
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 76
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space2

            Repeater {
                model: kindRow.choices

                delegate: SelectableChip {
                    required property var modelData
                    label: modelData.label
                    selected: kindRow.current === modelData.key
                    onPicked: kindRow.picked(modelData.key)
                }
            }
        }
    }

    // ---- Edit Audio Input dialog ----
    ModalCard {
        id: editAudioDialog
        shown: root.editAudioIndex >= 0
        title: qsTr("Edit Audio Input")
        cardWidth: 640
        saveText: qsTr("Save Changes")
        onCancelled: {
            root.editAudioIndex = -1
            root.syncAudioMeters()
        }
        onAccepted: root.saveEditAudio()

        Row {
            width: parent.width
            spacing: Theme.space4

            // The dial is gone from this dialog — the pro-audio form's
            // Volume row (ProAudioForm, below the fields) owns level now.

            Column {
                anchors.top: parent.top
                width: parent.width
                spacing: Theme.space4

                Item { width: 1; height: Theme.space2 }

                KindRailRow {
                    width: parent.width
                    current: root.editAudioKind
                    choices: root.audioKinds || []
                    onPicked: (key) => {
                        const buf = Object.assign({}, root.editAudioSublabelByKind || ({}))
                        buf[root.editAudioKind] = root.editAudioSublabel
                        root.editAudioSublabelByKind = buf
                        root.editAudioKind = key
                        root.editAudioSublabel = buf[key] !== undefined ? buf[key] : ""
                    }
                }

                NdiRuntimeNotice {
                    width: parent.width
                    active: root.editAudioKind === "ndi"
                }

                ProAudioForm {
                    width: parent.width
                    nameText: root.editAudioName
                    onNameEdited: (t) => root.editAudioName = t
                    sourceLabel: qsTr("Source")
                    sourceValue: root.editAudioSublabel
                    sourceOptions: root.audioSourceOptions(root.editAudioKind)
                    onSourcePicked: (v) => {
                        root.editAudioSublabel = root.resolveAudioSourcePick(v, root.editAudioSublabel, "editAudio")
                    }
                    delayMs: root.editAudioDelayMs
                    volume: root.editAudioLevel
                    muted: root.editAudioMuted
                    channels: root.editAudioEffectiveChannels
                    // Read shared meter telemetry only when Media has already
                    // opened a tap for this device. Settings never starts one.
                    meterLevels: root.audioMeterLevels
                    meterLayout: root.audioMeterLayout
                    // Per-channel gain knobs read the model row; a moved knob
                    // writes straight through (persisted, debounced).
                    gainFor: function(i) {
                        return root.editAudioGains.length > i ? root.editAudioGains[i] : 1.0
                    }
                    channelTrimFor: function(i) {
                        return AudioInputListModel.channelTrim(root.editAudioIndex, i)
                    }
                    onChannelGainEdited: (i, g) => {
                        AudioInputListModel.setChannelGain(root.editAudioIndex, i, g)
                        root.rememberEditAudioGain(i, g)
                    }
                    channelOnFor: function(i) {
                        void root.modelsRev
                        return !Db.isChannelOff(AudioInputListModel.channelGain(root.editAudioIndex, i))
                    }
                    onChannelToggled: (i, on, gain) => {
                        AudioInputListModel.setChannelGain(root.editAudioIndex, i, gain)
                        root.rememberEditAudioGain(i, gain)
                    }
                    onDelayEdited: (ms) => root.editAudioDelayMs = ms
                    onVolumeEdited: (v) => root.editAudioLevel = v
                    onChannelsEdited: (n) => root.editAudioChannels = n
                    onRoutingClicked: {
                        // Edit: load straight off the model row.
                        const rt = AudioInputListModel.getRouting(root.editAudioIndex)
                        routingModalFor = "edit"
                        routingModal.channelCount = root.editAudioEffectiveChannels
                        routingModal.inputName = root.editAudioName
                        routingModal.autoRoute = rt.auto !== undefined ? rt.auto : false
                        const map = {}
                        const lists = rt.routes !== undefined ? rt.routes : []
                        for (let c = 0; c < lists.length; c++) map[c] = lists[c]
                        routingModal.routes = map
                        routingModal.open()
                    }
                }
            }
        }

        AudioEffectsPanel {
            id: editFxPanel
            width: parent.width
            effects: root.editAudioEffects
            selectedKey: root.editAudioSelectedEffect
            // A selection grows the panel below the fold — scroll the just-
            // opened editor into view, or "nothing happened" is the read.
            onEffectSelected: (key) => {
                root.editAudioSelectedEffect = key
                if (key !== "") editAudioDialog.revealItem(editFxPanel)
            }
            onEffectToggled: (key) => {
                const rootRef = root
                Qt.callLater(() => {
                    if (rootRef.editAudioIndex < 0) return
                    const cur = rootRef.editAudioEffects.find((e) => e.key === key)
                    AudioInputListModel.setEffectEnabled(rootRef.editAudioIndex, key, cur ? !cur.enabled : true)
                })
            }
            onEffectValueMoved: (key, v) => {
                if (root.editAudioIndex < 0) return
                AudioInputListModel.setEffectValue(root.editAudioIndex, key, v)
            }
        }

        Row {
            width: parent.width
            AppButton {
                text: qsTr("Delete Input")
                variant: "danger"
                onClicked: {
                    root.removeAudioFixingRoutes(root.editAudioIndex)
                    root.editAudioIndex = -1
                    root.syncAudioMeters()   // release the deleted row's hold
                }
            }
        }
    }

    // ---- Edit Video Source dialog ----
    ModalCard {
        id: editVideoDialog
        shown: root.editVideoIndex >= 0
        title: qsTr("Edit Video Source")
        cardWidth: 640
        saveText: qsTr("Save Changes")
        onCancelled: root.editVideoIndex = -1
        onAccepted: root.saveEditVideo()

        // Extra breathing room under the header, exactly like the audio dialog
        // (that one has no subtitle either).
        Item { width: 1; height: Theme.space2 }

        KindRailRow {
            width: parent.width
            current: root.editVideoKind
            choices: root.videoKinds || []
            onPicked: (key) => {
                // Remember this kind's pick, restore the new kind's last one —
                // see editAudioSublabelByKind.
                const buf = Object.assign({}, root.editVideoSublabelByKind || ({}))
                buf[root.editVideoKind] = root.editVideoSublabel
                root.editVideoSublabelByKind = buf
                root.editVideoKind = key
                root.editVideoSublabel = buf[key] !== undefined ? buf[key] : ""
                root.editVideoMode = ""
            }
        }

        NdiRuntimeNotice {
            width: parent.width
            active: root.editVideoKind === "ndi"
        }

        ProAudioForm {
            width: parent.width
            nameText: root.editVideoName
            onNameEdited: (t) => root.editVideoName = t
            sourceLabel: qsTr("Source")
            sourceValue: root.editVideoSublabel
            sourceOptions: root.videoSourceOptions(root.editVideoKind)
            onSourcePicked: (v) => {
                root.editVideoSublabel = root.resolveVideoSourcePick(v, root.editVideoSublabel, "editVideo")
                root.editVideoMode = ""
            }
            // A video row has no latency stepper, and only Media/NDI kinds
            // carry audio (see the gate).
            showDelay: false
            showVolume: root.videoKindHasAudio(root.editVideoKind)
            showChannels: root.videoKindHasAudio(root.editVideoKind)
            volume: root.editVideoLevel
            muted: root.editVideoMuted
            channels: 2
            meterLevels: {
                void root.audioMeterList
                void root.modelsRev
                return root.editVideoIndex >= 0
                    ? root.videoChannelLevels(root.editVideoIndex, root.editVideoSublabel,
                                              root.editVideoKind, root.editVideoMuted,
                                              root.editVideoLevel)
                    : root.videoLiveLevels(root.editVideoSublabel, root.editVideoKind,
                                           root.editVideoMuted, root.editVideoLevel)
            }
            meterLayout: root.meterLayoutFor(root.editVideoSublabel, "stereo")
            // ---- the row's channel KNOBS, wired to the model ------------------
            gainFor: function(i) {
                void root.modelsRev
                return root.editVideoIndex >= 0
                    ? VideoSourceListModel.channelGain(root.editVideoIndex, i) : 1.0
            }
            // The trim a closed side comes back at — the model's own answer, the
            // same one the card's pill reopens with (models/ChannelTrim.h).
            channelTrimFor: function(i) {
                void root.modelsRev
                return VideoSourceListModel.channelTrim(root.editVideoIndex, i)
            }
            // The channel checkbox reads the MODEL (gain 0 ≡ off), so a side
            // closed from the card's L/R pill shows as off here too.
            channelOnFor: function(i) {
                void root.modelsRev
                return !Db.isChannelOff(VideoSourceListModel.channelGain(root.editVideoIndex, i))
            }
            onChannelToggled: (i, on, gain) => {
                if (root.editVideoIndex < 0)
                    return
                VideoSourceListModel.setChannelGain(root.editVideoIndex, i, gain)
            }
            onChannelGainEdited: (i, g) => {
                if (root.editVideoIndex < 0)
                    return
                VideoSourceListModel.setChannelGain(root.editVideoIndex, i, g)
            }
            onVolumeEdited: (v) => root.editVideoLevel = v
            onMutedToggled: root.editVideoMuted = !root.editVideoMuted
        }

        // ---- Preview (the one section audio has no counterpart for) — the
        VideoPreviewPane {
            width: parent.width
            kind: root.editVideoKind
            // Camera/screen/NDI go live (a real tap); media stays decorative.
            previewLabel: root.editVideoKind === "camera" || root.editVideoKind === "screen"
                          || root.editVideoKind === "ndi"
                          ? root.editVideoSublabel : ""
            // A media source plays its file here, at the dialog's volume (see VideoPreviewPane).
            mediaSource: root.editVideoKind === "media" ? root.editVideoSublabel : ""
            mediaVolume: root.editVideoLevel
            active: editVideoDialog.shown
            muted: root.editVideoMuted
            mode: root.editVideoMode
            modes: root.editVideoKind === "camera"
                   ? root.videoModesFor(root.editVideoSublabel)
                   : root.videoModes
            maxFps: root.editVideoKind === "camera"
                    ? root.videoMaxFpsFor(root.editVideoSublabel) : 0
            onMutedToggled: root.editVideoMuted = !root.editVideoMuted
            onModePicked: (m) => root.editVideoMode = m
        }

        Row {
            width: parent.width
            AppButton {
                text: qsTr("Delete Source")
                variant: "danger"
                onClicked: {
                    root.removeVideoFixingRoutes(root.editVideoIndex)
                    root.editVideoIndex = -1
                }
            }
        }
    }

    // ---- Bus dialog (Add + Edit) ----
    ModalCard {
        id: editBusDialog
        objectName: "selfTestBusDialog"
        shown: root.busDialogOpen
        title: root.busDialogIsAdd ? qsTr("Add Bus") : qsTr("Edit Bus")
        cardWidth: 500
        saveText: root.busDialogIsAdd ? qsTr("Add Bus") : qsTr("Save Changes")
        onCancelled: root.busDialogOpen = false
        onAccepted: root.saveBusDialog()

        SettingsField {
            width: parent.width
            label: qsTr("Name")
            text: root.editBusName
            onTextEdited: (t) => root.editBusName = t
        }

        Column {
            width: parent.width
            spacing: Theme.space2
            // A template bus keeps its kind; ordinary buses can be changed to
            // Template and assigned a design below.
            visible: root.busDialogIsAdd || root.editBusOriginalType !== "template"
            Text {
                text: qsTr("Type")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
            Row {
                spacing: Theme.space2
                Repeater {
                    // A template bus composes a Buses-category design; its
                    // windows are assigned in Edit · Templates.
                    model: [
                        { key: "audio", label: qsTr("Audio") },
                        { key: "video", label: qsTr("Video") },
                        { key: "both", label: qsTr("Both") },
                        { key: "template", label: qsTr("Template") }
                    ]
                    delegate: SelectableChip {
                        required property var modelData
                        label: modelData.label
                        selected: root.editBusType === modelData.key
                        onPicked: {
                            root.editBusType = modelData.key
                            root.editBusTemplateLocked = false
                            if (modelData.key === "template")
                                root.editBusTemplateId = root.defaultBusTemplateId()
                        }
                    }
                }
            }
        }

        // ---- TEMPLATE (Add Bus only): a DROPDOWN, wired to the Buses category's
        Column {
            width: parent.width
            spacing: Theme.space2
            visible: root.editBusType === "template"

            Text {
                text: root.busDialogIsAdd ? qsTr("Choose a bus template") : qsTr("Bus template")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            SelectField {
                objectName: "selfTestAvBusTemplateChoice"
                width: parent.width
                showLabel: false
                placeholder: qsTr("Choose a bus template")
                value: root.busTemplateName(root.editBusTemplateId)
                options: root.busTemplates.map((d) => ({ label: String(d.name || ""),
                                                         value: String(d.name || "") }))
                onValuePicked: (label) => {
                    const id = root.busTemplateIdForName(label)
                    if (id !== "")
                        root.editBusTemplateId = id
                    root.editBusTemplateLocked = false
                }
            }

            // What the picked design is: its windows, so the choice says what the bus will compose.
            Text {
                width: parent.width
                readonly property var design: {
                    root.templateRev
                    const all = root.busTemplates
                    for (let i = 0; i < all.length; ++i)
                        if (String(all[i].id) === root.editBusTemplateId)
                            return all[i]
                    return null
                }
                visible: design !== null
                text: design ? qsTr("%n window(s) — each one shows the bus its item names.", "", root.busTemplateWindows(design)) : ""
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                visible: root.busTemplates.length === 0
                text: qsTr("No bus templates yet. Make one in Edit · Templates under Buses: place the bus items that are its windows, then give each window the bus it shows.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                visible: root.editBusTemplateLocked
                text: qsTr("Pick a bus template first.")
                color: Theme.dangerLight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }
        }

        // ---- Channel strip (ProAudioForm) — the bus is a real channel:
        ProAudioForm {
            width: parent.width
            showNameSource: false
            showDelay: false
            showChannelSwitches: false
            nameLabel: qsTr("Master")
            sourceLabel: qsTr("Sources")
            sourceValue: qsTr("%1 routed").arg(root.busRoutedAudio().length)
            sourceOptions: []
            delayMs: 0
            volume: root.editBusLevel
            muted: root.editBusMuted
            channels: 2
            meterLevels: {
                void root.audioMeterList
                void root.modelsRev
                return root.editBusIndex >= 0
                    ? root.busChannelLevels(root.editBusIndex) : [0, 0]
            }
            meterLayout: "stereo"
            channelsEditable: false
            gainFor: function(i) {
                void root.modelsRev
                return 1.0
            }
            channelOnFor: function(i) {
                void root.modelsRev
                return root.busChannelOpen(root.editBusIndex, i)
            }
            onVolumeEdited: (v) => root.editBusLevel = v
            onMutedToggled: root.editBusMuted = !root.editBusMuted
        }

        // ---- SOURCES: what is wired into this bus right now, each with its own Disconnect — the
        // dialog says what the bus is made of instead of leaving that to the board's wires.
        Column {
            width: parent.width
            spacing: Theme.space2
            visible: !root.busDialogIsAdd

            Text {
                text: qsTr("Sources")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Repeater {
                id: busSourceRows
                model: root.busDialogOpen ? root.editBusSources() : []

                delegate: ValueRow {
                    required property var modelData
                    width: parent.width
                    implicitHeight: 40
                    hasSwatch: true
                    swatchColor: modelData.kind === "audio" ? Qt.alpha(Theme.success, 0.25)
                                                            : Qt.alpha(Theme.info, 0.25)
                    text: modelData.name !== "" ? modelData.name : qsTr("Untitled")

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.kind === "audio" ? qsTr("AUDIO") : qsTr("VIDEO")
                        color: modelData.kind === "audio" ? Theme.success : Theme.info
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.6
                    }
                    PillButton {
                        text: qsTr("Disconnect")
                        tone: "muted"
                        onClicked: root.disconnectRoute(modelData.kind, root.editBusIndex, modelData.index)
                    }
                }
            }

            ValueRow {
                width: parent.width
                implicitHeight: 40
                visible: busSourceRows.count === 0
                dim: true
                text: qsTr("Nothing is routed into this bus yet — drag from a source's port to its card.")
            }
        }

        Row {
            width: parent.width
            visible: !root.busDialogIsAdd
            AppButton {
                text: qsTr("Delete Bus")
                variant: "danger"
                onClicked: {
                    BusListModel.deleteBus(root.editBusIndex)   // the operator's delete: its notice offers the Undo
                    root.busDialogOpen = false
                }
            }
        }
    }

    // ---- Add Source dialog (VGRPresenter · Settings · Audio & Video · Add) ----
    ModalCard {
        id: addSourceDialog
        objectName: "selfTestAddSourceDialog"
        shown: root.addSourceShown
        title: qsTr("Add Source")
        cardWidth: 640
        saveText: qsTr("Add Source")
        // Same cancel-leaks-the-tap bug as Edit Audio's dialog above — see
        // its comment.
        onCancelled: {
            root.addSourceShown = false
            root.syncAudioMeters()   // Cancel must release the dialog's tap
        }
        onAccepted: root.submitAddSource()

        // Audio/Video type toggle — presets which column's + Add opened it,
        // stays switchable here (the reference's tab pair).
        Row {
            width: parent.width
            spacing: Theme.space2

            Repeater {
                model: [
                    { key: "audio", label: qsTr("Audio") },
                    { key: "video", label: qsTr("Video") }
                ]
                delegate: SelectableChip {
                    required property var modelData
                    label: modelData.label
                    selected: root.addSourceType === modelData.key
                    onPicked: {
                        root.addSourceType = modelData.key
                        root.addSourceKind = modelData.key === "audio" ? "device" : "camera"
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space4
            visible: root.addSourceType === "audio"

            Column {
                width: parent.width
                spacing: Theme.space4

                KindRailRow {
                    width: parent.width
                    current: root.addSourceKind
                    choices: root.audioKinds || []
                    onPicked: (key) => {
                        // Remember this kind's pick, restore the new
                        // kind's last one — see editAudioSublabelByKind.
                        const buf = Object.assign({}, root.addSourceSublabelByKind || ({}))
                        buf[root.addSourceKind] = root.addSourceSublabel
                        root.addSourceSublabelByKind = buf
                        root.addSourceKind = key
                        root.addSourceSublabel = buf[key] !== undefined ? buf[key] : ""
                    }
                }

                // Pro-audio form — the reference layout (Name / Source on
                // the shared label rail, Delay, Volume, Channels).
                NdiRuntimeNotice {
                    width: parent.width
                    active: root.addSourceKind === "ndi"
                }

                ProAudioForm {
                    width: parent.width
                    nameText: root.addSourceName
                    onNameEdited: (t) => root.addSourceName = t
                    sourceLabel: qsTr("Source")
                    sourceValue: root.addSourceSublabel
                    sourceOptions: root.audioSourceOptions(root.addSourceKind)
                    onSourcePicked: (v) => {
                        root.addSourceSublabel = root.resolveAudioSourcePick(v, root.addSourceSublabel, "addSourceAudio")
                    }
                    delayMs: root.addSourceDelayMs
                    volume: root.addSourceLevel
                    muted: root.addSourceMuted
                    channels: root.addSourceEffectiveChannels
                    meterLevels: root.audioMeterLevels
                    meterLayout: root.audioMeterLayout
                    // Add dialog: knobs live in the buffered state until the
                    // row is submitted (same contract as every other field).
                    gainFor: function(i) {
                        return root.addSourceGains.length > i ? root.addSourceGains[i] : 1.0
                    }
                    onChannelGainEdited: (i, g) => {
                        while (root.addSourceGains.length <= i)
                            root.addSourceGains.push(1.0)
                        root.addSourceGains[i] = g
                        // An OPEN level is this side's trim (a close is not): the
                        // same rule the models' setChannelGain applies.
                        if (!Db.isChannelOff(g))
                            root.rememberAddSourceTrim(i, g)
                    }
                    channelTrimFor: function(i) {
                        return root.addSourceTrims.length > i ? root.addSourceTrims[i] : 0.0
                    }
                    channelOnFor: function(i) {
                        return !Db.isChannelOff(root.addSourceGains.length > i
                                                ? root.addSourceGains[i] : 1.0)
                    }
                    onChannelToggled: (i, on, gain) => {
                        const next = root.addSourceGains.slice()
                        while (next.length <= i)
                            next.push(1.0)
                        next[i] = gain
                        root.addSourceGains = next
                        if (!Db.isChannelOff(gain))
                            root.rememberAddSourceTrim(i, gain)
                    }
                    onDelayEdited: (ms) => root.addSourceDelayMs = ms
                    onVolumeEdited: (v) => root.addSourceLevel = v
                    onChannelsEdited: (n) => root.addSourceChannels = n
                    onRoutingClicked: {
                        // Add: no row yet — load the buffered state.
                        routingModalFor = "add"
                        routingModal.channelCount = root.addSourceEffectiveChannels
                        routingModal.inputName = root.addSourceName !== ""
                                ? root.addSourceName : qsTr("New source")
                        routingModal.autoRoute = root.addSourceRoutingAuto
                        routingModal.routes = root.addSourceRoutes
                        routingModal.open()
                    }
                }
            }
        }

        // Video-only fields (same fields, no meter/dial/effects).
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: root.addSourceType === "video"

            KindRailRow {
                width: parent.width
                current: root.addSourceKind
                choices: root.videoKinds || []
                onPicked: (key) => {
                    // Remember this kind's pick, restore the new
                    // kind's last one — see editAudioSublabelByKind.
                    const buf = Object.assign({}, root.addSourceSublabelByKind || ({}))
                    buf[root.addSourceKind] = root.addSourceSublabel
                    root.addSourceSublabelByKind = buf
                    root.addSourceKind = key
                    root.addSourceSublabel = buf[key] !== undefined ? buf[key] : ""
                    root.addSourceVideoMode = ""
                }
            }

            NdiRuntimeNotice {
                width: parent.width
                active: root.addSourceKind === "ndi"
            }

            ProAudioForm {
                width: parent.width
                nameText: root.addSourceName
                onNameEdited: (t) => root.addSourceName = t
                namePlaceholder: qsTr("e.g. “Cam 3 · Aisle”")
                sourceLabel: qsTr("Source")
                sourceFieldObjectName: "selfTestAddSourceSourceField"
                sourcePlaceholder: root.addSourceKind === "camera" && root.engineVideoDevices.length === 0
                                   ? qsTr("No camera found")
                                   : qsTr("Pick a source")
                sourceValue: root.addSourceSublabel
                sourceOptions: root.videoSourceOptions(root.addSourceKind)
                onSourcePicked: (v) => {
                    root.addSourceSublabel = root.resolveVideoSourcePick(v, root.addSourceSublabel, "addSourceVideo")
                    // A new device is a new capability set — the old mode
                    // pick may not exist on it.
                    root.addSourceVideoMode = ""
                }
                showDelay: false
                showVolume: root.videoKindHasAudio(root.addSourceKind)
                showChannels: root.videoKindHasAudio(root.addSourceKind)
                volume: root.addSourceLevel
                muted: root.addSourceMuted
                channels: 2
                meterLevels: {
                    void root.audioMeterList
                    return root.videoLiveLevels(root.addSourceSublabel, root.addSourceKind,
                                                root.addSourceMuted, root.addSourceLevel)
                }
                meterLayout: root.meterLayoutFor(root.addSourceSublabel, "stereo")
                onVolumeEdited: (v) => root.addSourceLevel = v
                onMutedToggled: root.addSourceMuted = !root.addSourceMuted
            }

            // ---- Preview — the SHARED VideoPreviewPane component (the
            VideoPreviewPane {
                width: parent.width
                showHint: false
                paneObjectName: "selfTestPreviewPane"   // the ADD dialog's pane — the grab target
                kind: root.addSourceKind
                // Camera/screen/NDI go live (a real tap); media stays
                // decorative (no live tap exists for a file pick).
                previewLabel: root.addSourceKind === "camera" || root.addSourceKind === "screen"
                              || root.addSourceKind === "ndi"
                              ? root.addSourceSublabel : ""
                muted: root.addSourceMuted
                mode: root.addSourceVideoMode
                modes: root.videoModesFor(root.addSourceSublabel)
                maxFps: root.videoMaxFpsFor(root.addSourceSublabel)
                onMutedToggled: root.addSourceMuted = !root.addSourceMuted
                onModePicked: (m) => root.addSourceVideoMode = m
            }
        }

        // Effects rack — audio only, shared component, same as Edit's.
        AudioEffectsPanel {
            id: addFxPanel
            width: parent.width
            visible: root.addSourceType === "audio"
            effects: root.addSourceEffects
            selectedKey: root.addSourceSelectedEffect
            // Same reveal-on-select as the Edit dialog.
            onEffectSelected: (key) => {
                root.addSourceSelectedEffect = key
                if (key !== "") addSourceDialog.revealItem(addFxPanel)
            }
            onEffectToggled: (key) => {
                const rootRef = root
                Qt.callLater(() => {
                    rootRef.addSourceEffects = rootRef.addSourceEffects.map((e) =>
                        e.key === key ? Object.assign({}, e, { enabled: !e.enabled }) : e)
                })
            }
            onEffectValueMoved: (key, v) => {
                root.addSourceEffects = root.addSourceEffects.map((e) =>
                    e.key === key ? Object.assign({}, e, { value: v }) : e)
            }
        }
    }
}
