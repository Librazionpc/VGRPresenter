import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

// Settings · Audio & Video — the mixing/routing board (reference:
// InspirationOrResources/1BBTIwaya/VGRPresenter_Settings_Audio_Video.qml).
// Three rosters (AudioInputListModel / VideoSourceListModel / BusListModel,
// all C++ singletons) laid out as Audio Inputs | Buses & Routing | Video
// Sources, with curved lines drawn from each bus's stored routing.
//
// Full CRUD (Add/Edit/Duplicate/Delete, right-click menu on every card) plus
// live drag-to-connect: press-drag a card's port dot onto a bus to route it,
// right-click a line (true point-to-curve hit-testing, see lineClickedAt) to
// remove it, or use a bus's Edit dialog's route-toggle list — all three
// write through the same BusListModel toggle calls, so they can't drift.
// Audio routing is many-to-many; video is strictly 1:1 (dropAllowed) since a
// bus renders exactly one video frame. Ducking settings and a real audio/
// video engine (levels/effects are stored values, not live DSP) are the
// only pieces still deferred.
Item {
    id: root

    // Same reactivity-bridge pattern as every other settings screen this
    // session (StylesScreen, OutputsScreen): plain Q_INVOKABLE reads
    // (rowCount(), getBus(), …) aren't tracked by QML's binding system, so
    // this counter is the honest dependency, bumped by all three models.
    property int modelsRev: 0
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
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
    // dialog's device pick without poking pixels — sets the same state the
    // SelectField's onValuePicked handler sets, so the pane's live chain
    // (start tap → pump → frames) runs exactly as a real user's pick.
    // kind selects the row type: "camera" (default) or "screen" — screens
    // sweep displays AND open windows through the same pane.
    function selfTestPickVideoSource(label, kind) {
        root.addSourceType = "video"
        root.addSourceKind = kind || "camera"
        root.addSourceSublabel = label
        root.addSourceShown = true
        syncAudioMeter()
    }

    // ---- Live input metering (the channel rows' real VU feed) -------------
    // Taps are a SET keyed by device: the board keeps every device row
    // metered while this screen exists, and the dialogs just read their own
    // device's snapshot from the published list. onInputLevelsChanged is NOT
    // wired to re-resolution — resolution happens inside the bridge on every
    // enumeration, so a hot-plugged device re-binds by itself.
    readonly property var audioMeterList: EngineBridge.inputLevels
    function meterSnapshotFor(label) {
        const want = label === undefined ? "" : String(label).trim()
        const list = root.audioMeterList
        for (let i = 0; i < list.length; i++) {
            const s = list[i]
            // Empty label ↔ the bridge's default-input entry (it stores "").
            if ((s.label || "") === want)
                return s
        }
        return null
    }
    // The gain-aware, per-channel-mute-aware live level for one audio input
    // row — the loudest of its channels, each scaled by that channel's own
    // gain (0 when a channel's L/R button muted it in the Media pane).
    // Every "how loud is this input right now" reading (the board row's own
    // meter, a bus's mix, an Edit Bus routed-source row) MUST go through
    // this, not a bare peaks[0]: reading channel 0 only meant muting the
    // RIGHT channel from its own card never showed anywhere else — the
    // bus and every downstream reading kept reporting the untouched left
    // channel as if nothing had changed.
    function inputMixLevel(rowIndex, sublabel) {
        const s = root.meterSnapshotFor(sublabel)
        if (!s || s.peaks === undefined || s.peaks.length === 0)
            return 0
        let mix = 0
        for (let c = 0; c < s.peaks.length; c++) {
            const gain = AudioInputListModel.channelGain(rowIndex, c)
            const lvl = Math.max(0, Math.min(1, s.peaks[c])) * gain
            if (lvl > mix) mix = lvl
        }
        return mix
    }
    // A bus's own live post-fader mix — the loudest of its routed sources'
    // gain-aware channel levels (same "loudest wins" rule the routing
    // board's own bus meter uses), scaled by the bus's own fader. Feeds the
    // Edit Bus dialog's stereo master row pair (both channels mirror this;
    // a bus has no independent L/R capture of its own).
    function busMixLevel(busIndex) {
        if (busIndex < 0) return 0
        const b = BusListModel.getBus(busIndex)
        let mix = 0
        for (const r of b.routedAudioInputs) {
            const data = AudioInputListModel.getInput(r)
            if (!data || data.muted) continue
            const raw = root.inputMixLevel(r, data.sublabel)
            if (raw > mix) mix = raw
        }
        return mix * (b.level / 100)
    }
    // The dialog feed: the device being edited (Add dialog with no pick yet
    // meters the default input — the empty label).
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

    function openAudioMeter(label) {
        EngineBridge.startInputMeter(label === undefined ? "" : String(label))
    }
    function closeAudioMeter(label) {
        EngineBridge.stopInputMeter(label === undefined ? "" : String(label))
    }
    // Ensure the taps the CURRENT dialog state wants (device rows only —
    // media/NDI carry no WASAPI device). Taps are idempotent per device, so
    // re-calling after a re-pick just adds/releases the difference.
    function syncAudioMeter() {
        if (root.editAudioIndex >= 0 && root.editAudioKind === "device")
            root.openAudioMeter(root.editAudioSublabel)
        else if (root.addSourceShown && root.addSourceType === "audio"
                 && root.addSourceKind === "device")
            root.openAudioMeter(root.addSourceSublabel)   // empty pick → default input
    }
    // The whole screen's taps — Component.onDestruction (a screen can be
    // destroyed while the app lives) and shutdown both land here.
    function closeAllAudioMeters() {
        EngineBridge.stopAllInputMeters()
        EngineBridge.stopAllVideoPreviews()   // camera taps too (board + dialogs)
    }
    Component.onDestruction: root.closeAllAudioMeters()

    // ---- Routing matrix modal — shared by the Add + Edit audio dialogs.
    // Which dialog opened it decides where Apply lands: edit writes the
    // model row directly; add writes the buffered pre-row state (consumed
    // by submitAddSource).
    property string routingModalFor: ""   // "add" | "edit"
    RoutingMatrixModal {
        id: routingModal
        // ABOVE the Edit/Add dialogs (declared before them, and QML siblings
        // stack in declaration order — the modal opened UNDER the edit dialog
        // and looked like "nothing happened"). z beats declaration order.
        z: 50
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

    // ---- Edit Audio Input dialog ----
    property int editAudioIndex: -1
    property string editAudioName: ""
    property string editAudioKind: "device"
    property string editAudioSublabel: ""
    // Per-kind memory for the Source field — switching Kind away and back
    // (Device → Media → Device) used to permanently blank the sublabel
    // every time, even round-tripping back to a kind you'd already picked
    // something for. See the Kind chips' onPicked below.
    property var editAudioSublabelByKind: ({})
    property real editAudioLevel: 0   // silence by default — meter reflects it
    property bool editAudioMuted: false
    // Pro-audio form state — Delay / Channels (the reference mock's rows),
    // persisted on the model like level/muted.
    property int editAudioDelayMs: 0
    property int editAudioChannels: 2
    property var editAudioGains: []   // per-channel faders, live from the model
    // Effective channel rows shown: the ENGINE's per-device truth (WASAPI
    // mix format) wins when the row names a known device; the stored count
    // covers media rows and unknown devices.
    readonly property int editAudioEffectiveChannels: {
        const n = root.editAudioKind === "device" ? root.deviceChannels(root.editAudioSublabel) : 0
        return n > 0 ? n : root.editAudioChannels
    }
    // Which rack chip is open in the effects editor.
    property string editAudioSelectedEffect: ""
    // The selected input's effect list, live from the model — effects edits
    // apply immediately (same justification as the bus routing toggles:
    // cheap to reverse, and the panel redraws as you work), so this reads
    // THROUGH the model via modelsRev instead of a deferred snapshot.
    readonly property var editAudioEffects: {
        root.modelsRev
        return root.editAudioIndex < 0 ? [] : AudioInputListModel.getInput(root.editAudioIndex).effects
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
        // Real metering for device rows — the tap's live channel layout
        // (mono/stereo/multi) re-renders the channel rows below.
        root.syncAudioMeter()
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
        if (root.editAudioKind === "device")
            root.closeAudioMeter(root.editAudioSublabel)
    }

    // ---- Add Source dialog ----
    // One dialog for both types (the reference's Audio/Video tab): the type
    // chips pick which roster the row lands in, the kind chips follow the
    // type, and the meter/dial/effects rack are audio-only — video has no
    // level or effects in this app's data model, and the reference doesn't
    // show them for video either. All state is collected BEFORE the row
    // exists, then written through the setters against rowCount()-1 after
    // addInput()/addSource() (every setter is independently idempotent).
    property bool addSourceShown: false
    property string addSourceType: "audio"
    property string addSourceName: ""
    property string addSourceKind: "device"
    property string addSourceSublabel: ""
    // Per-kind memory for the Source field — see editAudioSublabelByKind.
    property var addSourceSublabelByKind: ({})
    property string addSourceVideoMode: ""   // video rows' capture mode
    property real addSourceLevel: 0   // silence by default — meter reflects it
    property bool addSourceMuted: false
    // Pro-audio form state — Delay / Channels, written through the setters
    // after addInput() like every other collected value.
    property int addSourceDelayMs: 0
    property int addSourceChannels: 2
    property var addSourceGains: []   // buffered per-channel faders (unity default)
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

    // Shared kind taxonomies — the same lists the Edit dialogs render, so
    // they can't drift apart. Audio: a real device (mic/line/system — the
    // specific hardware is the row's own identity, not its kind) or media.
    readonly property var audioKinds: [
        { key: "device", label: qsTr("Device") },
        { key: "media", label: qsTr("Media") },
        { key: "ndi", label: qsTr("NDI") }
    ]

    // Device/media pick lists for the audio dialogs' second field — the
    // field's LABEL follows the kind ("Device" vs "Media") and so do its
    // options. Device options come from the ENGINE's real platform
    // enumeration (EngineBridge.audioDevices — the PAL's WinMM IAudio,
    // live-refreshed on hot-plug events); the static fallback only covers a
    // system where the PAL reports nothing. Media rows name a content
    // source.
    readonly property var engineAudioDevices: {
        const list = EngineBridge.audioDevices.filter(d => d.isInput)
        return list.map(d => ({ label: d.label, value: d.value,
                                channels: d.channels !== undefined ? d.channels : 0,
                                sampleRateHz: d.sampleRateHz !== undefined ? d.sampleRateHz : 0 }))
    }
    // Real channel count of the picked device (0 = unknown/no pick): the
    // dialogs' Channels rows follow it — the engine's WASAPI query is the
    // source of truth, not a UI stepper.
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
    // AUDIO's own Media-kind source list — real audio tracks the Media
    // Library has indexed (MediaLibraryService.items(), kind === "audio"),
    // not the placeholder labels above (those were never wired to
    // anything: picking one just stored its literal caption as the
    // sublabel). "Browse for file..." opens the native picker via
    // AudioInputListModel.pickAudioFile() — see onSourcePicked below.
    // "Stream Capture" (per-app/stream loopback capture) is dropped here
    // rather than kept as a still-fake button: there's no spec yet for
    // what it should capture, and a button that LOOKS wired but does
    // nothing is worse than one that isn't offered.
    readonly property var audioMediaSourceOptions: {
        void MediaLibraryService.totalCount   // reactivity dependency — items()
                                               // itself is an untracked read
        const list = []
        const items = MediaLibraryService.items()
        for (let i = 0; i < items.length; i++) {
            if (items[i].kind === "audio")
                list.push({ label: items[i].name, value: items[i].path })
        }
        if (list.length === 0)
            list.push({ label: qsTr("No audio tracks in the Media Library yet"), disabled: true })
        list.push({ label: qsTr("Browse for file…"), value: "__browse_audio_file__" })
        return list
    }
    readonly property var videoKinds: [
        { key: "camera", label: qsTr("Camera") },
        { key: "screen", label: qsTr("Screen") },
        { key: "media", label: qsTr("Media") },
        { key: "ndi", label: qsTr("NDI") }
    ]    // Capture modes for the video dialogs' Resolution picker. The ENGINE
    // owns the list now: modes come from the picked device's real Media
    // Foundation capability set (EngineBridge.videoDevices), and its maxFps
    // greys any offered mode the hardware can't reach. These statics only
    // cover non-camera kinds (screen/media) and the no-device fallback.
    readonly property var videoModes: [
        "4Kp29.97", "2560x1440p29.97", "1080p60", "1080p29.97",
        "1280x960p29.97", "960x540p29.97", "720p29.97", "640x480p29.97",
        "640x360p29.97"
    ]

    // Real video-capture roster from the engine (Media Foundation).
    readonly property var engineVideoDevices: EngineBridge.videoDevices
    function videoDevice(name) {
        if (!name) return null
        const hit = engineVideoDevices.find(d => d.value === name)
        return hit || null
    }
    // Device-select options for a video row's Source: REAL cameras for the
    // camera kind, the ENGINE's discovered NDI sources for the ndi kind;
    // screen capture and media keep their own option sets.
    readonly property var cameraOptions: engineVideoDevices.length > 0
        ? engineVideoDevices
        : [{ label: qsTr("No camera found") }]
    // Screens: REAL displays from the engine's monitor PAL (EngineBridge
    // enumerates them at boot and on hot-plug), labelled with the
    // resolution each one runs at. Value stays the display's readable name
    // (what a row stores as its sublabel). No displays = one honest dimmed
    // row, never a mock roster.
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
    // The Source field's "Browse for file..." option is a terminal ACTION
    // (open the native picker), not a real sublabel value — resolve it here
    // so both the Edit and Add dialogs' onSourcePicked handlers share the
    // one path. A cancelled picker keeps the previous pick (empty return
    // means "no change", not "clear the source").
    function resolveAudioSourcePick(picked, previous) {
        if (picked !== "__browse_audio_file__")
            return picked
        const file = AudioInputListModel.pickAudioFile()
        return file !== "" ? file : previous
    }
    // VIDEO's own Media-kind source list — real video files the Media
    // Library has indexed (MediaLibraryService.items(), kind === "video"),
    // same real-data treatment as audioMediaSourceOptions above (see its
    // comment for why the old placeholder labels were dropped).
    readonly property var videoMediaSourceOptions: {
        void MediaLibraryService.totalCount
        const list = []
        const items = MediaLibraryService.items()
        for (let i = 0; i < items.length; i++) {
            if (items[i].kind === "video")
                list.push({ label: items[i].name, value: items[i].path })
        }
        if (list.length === 0)
            list.push({ label: qsTr("No video files in the Media Library yet"), disabled: true })
        list.push({ label: qsTr("Browse for file…"), value: "__browse_video_file__" })
        return list
    }
    function videoSourceOptions(kind) {
        if (kind === "camera") return cameraOptions
        if (kind === "screen") return screenOptions
        if (kind === "ndi") return ndiOptions
        if (kind === "media") return videoMediaSourceOptions
        return mediaSourceOptions
    }
    // Same "Browse for file..." resolution as resolveAudioSourcePick, for
    // the video dialogs' Source field.
    function resolveVideoSourcePick(picked, previous) {
        if (picked !== "__browse_video_file__")
            return picked
        const file = VideoSourceListModel.pickVideoFile()
        return file !== "" ? file : previous
    }
    // Kinds whose feed carries audio — media files and NDI streams (NDI
    // embeds audio with its video frames). These get the Volume slider and
    // a MUTE pill; camera/screen feeds are silent and pause instead.
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
        root.addSourceLevel = 0
        root.addSourceMuted = false
        root.addSourceDelayMs = 0
        root.addSourceChannels = 2
        root.addSourceGains = []
        root.addSourceRoutingAuto = false
        root.addSourceRoutes = ({})
        // Seed the rack from the model's template (no row exists yet to read
        // from — defaultEffectsTemplate() is the pre-row snapshot).
        root.addSourceEffects = AudioInputListModel.defaultEffectsTemplate()
        root.addSourceSelectedEffect = ""
        root.addSourceShown = true
        // Real roster, fresh from the engine (a camera plugged in since boot
        // shows up the moment the dialog opens).
        EngineBridge.refreshDevices()
        // Audio + device kind meters the default input until a Source pick
        // re-targets the tap (syncAudioMeter is the single switchboard).
        root.syncAudioMeter()
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
        if (root.addSourceType === "audio" && root.addSourceKind === "device")
            root.closeAudioMeter(root.addSourceSublabel)   // the dialog's tap only
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
        root.editVideoName = data.name
        root.editVideoSublabelByKind = ({})   // fresh buffer per row
        root.editVideoKind = data.kind
        root.editVideoSublabel = data.sublabel
        root.editVideoMode = data.mode !== undefined ? data.mode : ""
        root.editVideoMuted = data.muted
        root.editVideoLevel = data.level
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
        root.editVideoIndex = -1
    }

    // ---- Bus dialog (Add + Edit share one form) ----
    // Name/type/level/muted are deferred (edited copy, written on Save) —
    // same Cancel/Save contract as every other dialog. Routing checkboxes
    // are the one exception: they call BusListModel.toggleAudioRoute/
    // toggleVideoRoute directly and take effect immediately, same as a
    // Style row's inline-editable name commits immediately while its other
    // fields go through the dialog's Save — a routing toggle is cheap to
    // reverse and benefits from the line redrawing live as you check boxes.
    // Add mode reuses the same state and fields; the row is created on Save
    // (create-then-apply, same shape as submitAddSource) and the routing
    // sections are hidden — there is no row to route into until it exists.
    property bool busDialogOpen: false
    property bool busDialogIsAdd: false
    property int editBusIndex: -1
    property string editBusName: ""
    property string editBusType: "audio"
    property real editBusLevel: 75
    property bool editBusMuted: false

    function openEditBus(index) {
        const data = BusListModel.getBus(index)
        root.editBusIndex = index
        root.editBusName = data.name
        root.editBusType = data.type
        root.editBusLevel = data.level
        root.editBusMuted = data.muted
        root.busDialogOpen = true
        root.busDialogIsAdd = false
    }
    function openAddBus() {
        root.editBusIndex = -1
        root.editBusName = ""
        root.editBusType = "audio"
        root.editBusLevel = 75
        root.editBusMuted = false
        root.busDialogOpen = true
        root.busDialogIsAdd = true
    }
    function saveBusDialog() {
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
            BusListModel.setLevel(root.editBusIndex, root.editBusLevel)
            BusListModel.setMuted(root.editBusIndex, root.editBusMuted)
        }
        root.busDialogOpen = false
    }
    function busRoutedAudio() {
        root.modelsRev
        return root.editBusIndex < 0 ? [] : BusListModel.getBus(root.editBusIndex).routedAudioInputs
    }
    function busRoutedVideo() {
        root.modelsRev
        return root.editBusIndex < 0 ? [] : BusListModel.getBus(root.editBusIndex).routedVideoSources
    }

    // Has this row been routed into ANY bus? Drives the status dot
    // (accent = connected, grey = unconnected — the one connection
    // indicator, see StatusDot). Reactive via modelsRev.
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
    // Routes key on each row's STABLE id inside the engine graph, and the
    // roster models cut the removed row's edges themselves and re-derive the
    // buses' cached row numbers afterwards (BusListModel::refreshRoutes) — so
    // removal is just removal. (The old index-compaction toggling here fought
    // that and rewired the wrong sources.) Both the context menu's Delete and
    // the Edit dialogs' Delete buttons go through these.
    function removeAudioFixingRoutes(index) {
        AudioInputListModel.removeInput(index)
    }
    function removeVideoFixingRoutes(index) {
        VideoSourceListModel.removeSource(index)
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
            BusListModel.removeBus(root.deleteTargetIndex)
        root.deleteTargetIndex = -1
    }

    // ---- Drag-to-connect ----
    // Press a source card's port dot, drag onto a bus card, release: the
    // route is added (never removed — right-click a line to remove, or use
    // the bus's Edit & Routing dialog).
    property bool dragConnectActive: false
    property string dragConnectFrom: ""   // "audio" | "video"
    property int dragConnectIndex: -1
    property point dragConnectStart
    property point dragConnectPos

    function startDragConnect(kindName, srcIndex, fromItem, mx, my) {
        root.dragConnectFrom = kindName
        root.dragConnectIndex = srcIndex
        root.dragConnectStart = fromItem.mapToItem(board, mx, my)
        root.dragConnectPos = root.dragConnectStart
        root.dragConnectActive = true
    }
    function updateDragConnect(fromItem, mx, my) {
        root.dragConnectPos = fromItem.mapToItem(board, mx, my)
    }
    // One place for the route-toggle branch — called synchronously from
    // the line hit-layer (which no model rebuild can destroy) and deferred
    // from finishDragConnect (which runs inside the port's release
    // dispatch, where a synchronous toggle would rebuild the ROW delegate
    // under the mouse).
    function lineClickedAt(x, y) {
        // TRUE point-to-curve hit-testing: sample each cubic at 17 points
        // (t = 0..1) and take the line whose nearest sample is closest to
        // the click. The earlier midpoint-box version only caught clicks
        // near a line's center — on the video side the lines bundle at both
        // ports, so clicks on the end segments (the natural place to aim)
        // fell outside every box and nothing deleted. 17 samples ≈ 5px
        // spacing worst-case on the longest line; a 12px catch radius
        // comfortably bridges the gaps. Lives on the root (not the hit
        // MouseArea's own context) so it survives any rebuild.
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

    function finishDragConnect(kindName, srcIndex, fromItem, mx, my) {
        root.dragConnectActive = false
        const p = fromItem.mapToItem(board, mx, my)
        const b = root.busIndexAt(p)
        if (b < 0)
            return
        if (!root.dropAllowed(kindName, srcIndex, b)) {
            root.flashBus(b, false)   // refused — say so, don't just snap back
            return
        }
        const routes = kindName === "audio"
                       ? BusListModel.getBus(b).routedAudioInputs
                       : BusListModel.getBus(b).routedVideoSources
        if (routes.indexOf(srcIndex) >= 0) {
            root.flashBus(b, true)    // already connected — confirm, don't no-op silently
            return
        }
        // Deferred like the connector's right-click: this runs inside the
        // port MouseArea's release dispatch, and a synchronous route toggle
        // rebuilds the rows, destroying the very delegate dispatching the
        // event. Capture the plain values, mutate after dispatch settles.
        root.flashBus(b, true)
        Qt.callLater(() => root.disconnectRoute(kindName, b, srcIndex))
    }
    // Drop feedback: pulse the bus card's border — green when the drop
    // connected (or was already connected), red when refused. Without this
    // a duplicate or refused drop looks like the board ignored you.
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
        onTriggered: root.flashBusIndex = -1
    }
    // Which bus card (if any) a board-space point lands on — geometry from
    // the board's own layout constants, so it can't drift from the cards.
    // Buses stack with the standard rowH rhythm (busPortY).
    function busIndexAt(p) {
        if (p.x < board.busX - 8 || p.x > board.busX + board.busColW + 8)
            return -1
        const relY = p.y - board.headerH
        if (relY < 0)
            return -1
        const row = Math.floor(relY / (board.rowH + board.rowGap))
        const bottom = relY - row * (board.rowH + board.rowGap)
        if (row >= BusListModel.rowCount() || bottom > board.rowH)
            return -1
        return row
    }
    readonly property int dragConnectHoverBus:
        root.dragConnectActive ? root.busIndexAt(root.dragConnectPos) : -1
    // The one compatibility rule, shared by the drag-connect hover
    // highlight and the drop handler so they can never disagree: AUDIO
    // fits every bus (mixing is frame-free — embedded with the feed on
    // video buses). VIDEO is strictly 1:1 — a bus renders exactly ONE
    // source and a source renders on exactly ONE bus, so a bus that
    // already holds a different source refuses the drop (red hover + red
    // flash): two sources on one output would be two frames competing to
    // render it.
    function dropAllowed(kindName, srcIndex, busIndex) {
        const bus = BusListModel.getBus(busIndex)
        if (kindName === "audio")
            return true
        if (bus.type === "audio")
            return false
        const routes = bus.routedVideoSources
        return routes.length === 0 || (routes.length === 1 && routes[0] === srcIndex)
    }
    readonly property bool dragConnectCompat: {
        if (root.dragConnectHoverBus < 0)
            return false
        return root.dropAllowed(root.dragConnectFrom, root.dragConnectIndex,
                                root.dragConnectHoverBus)
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + Theme.space6 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // A connect-drag owns the gesture end-to-end: hard-freeze the page
        // scroll while one is live (belt to the ports' preventStealing
        // suspenders — the Flickable can't scroll at all mid-drag).
        interactive: !root.dragConnectActive

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space6
            width: flick.width - Theme.space6
            spacing: Theme.space6

            Column {
                spacing: Theme.space1
                Text {
                    text: qsTr("Audio & Video")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Sources and routing — send audio & video to buses, add inputs and sources, and wire what feeds each bus from its Edit dialog.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.WordWrap
                    width: layout.width
                }
            }

            // ---- Routing board ----
            Item {
                id: board

                // Sized to the actual available width, not a fixed pixel
                // budget — a fixed 270+60+300+60+270=960px board clipped
                // straight through the video sources column (its "Edit"
                // link included) inside the Settings modal's ~820px content
                // area (ModalShell's 1080 implicitWidth minus NavRail's
                // 206), since this Flickable never scrolls horizontally.
                // Proportional column widths mean the board always fits
                // whatever room it's actually given.
                width: layout.width
                readonly property real gutterW: 32
                // Video cards carry a preview thumb the other columns don't,
                // so they get their own slightly wider column — the extra
                // room lets a full-width name sit beside the thumb instead
                // of eliding to "Audience C…". Audio/bus keep colW.
                readonly property real vidColW: Math.max(200, (board.width - board.gutterW * 2) * 0.30) + 36
                readonly property real colW: Math.max(200, (board.width - board.gutterW * 2) * 0.30)
                // The -36 keeps the three columns' combined width the same as
                // before (video borrowed it); the floor moves down with it.
                readonly property real busColW: Math.max(184, (board.width - board.gutterW * 2) * 0.34 - 36)
                readonly property int rowH: 64
                // Video rows share the row rhythm — a taller video-only row
                // broke the board's visual alignment (three columns must
                // read as one grid). The thumb fits a 64px row by sizing
                // off the pane's own height (see vidThumb below); vidColW
                // is what buys the names room, not extra height.
                readonly property int vidRowH: board.rowH
                readonly property int rowGap: Theme.space3
                readonly property int headerBoxH: 40
                readonly property int headerH: headerBoxH + rowGap

                readonly property real audioX: 0
                readonly property real busX: board.colW + board.gutterW
                readonly property real videoX: board.busX + board.busColW + board.gutterW

                // Per-kind port centers — each column stacks with its own
                // row height, so a connector's endpoint y must use the
                // row height of the column the port lives in.
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
                    for (let b = 0; b < busCount; ++b) {
                        const bus = BusListModel.getBus(b)
                        const by = board.busPortY(b)
                        for (const ai of bus.routedAudioInputs) {
                            pairs.push({ x1: board.audioX + board.colW, y1: board.audioPortY(ai),
                                         x2: board.busX, y2: by, color: Theme.success,
                                         kind: "audio", busIndex: b, srcIndex: ai })
                        }
                        for (const vi of bus.routedVideoSources) {
                            pairs.push({ x1: board.videoX, y1: board.videoPortY(vi),
                                         x2: board.busX + board.busColW, y2: by, color: Theme.info,
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

                // Connector lines — drawn behind the columns so each card's
                // opaque background clips the line right at its port dot.
                // One Shape per connector (not a Repeater generating
                // ShapePath directly inside a single Shape) — Repeater needs
                // Item-derived delegates, and Shape itself is an Item, so
                // this is the safe, standard-issue Repeater usage already
                // used everywhere else in this file.
                Repeater {
                    model: board.connectorPairs()
                    delegate: Shape {
                        id: connector
                        required property var modelData
                        anchors.fill: parent
                        z: -1
                        preferredRendererType: Shape.CurveRenderer
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

                    }
                }

                // ---- Line hit-testing — ONE layer for ALL lines, sitting
                // OUTSIDE the connector Repeater (above the lines, below the
                // cards, which are later siblings). An earlier design put a
                // right-click MouseArea on each connector delegate, and it
                // crashed: the route toggle rebuilds this Repeater, so the
                // delegate under the cursor died mid-event-dispatch — and
                // with overlapping hit boxes (video lines converge on one
                // bus port, audio routes spread across buses — hence "video
                // crashes, audio doesn't") a second delegate could die while
                // a queued closure rooted in its context still pended. With
                // a single non-delegate layer there is nothing to destroy
                // under the cursor, so the toggle can run synchronously.
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

                        Column {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("AUDIO INPUTS")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                            Text {
                                text: qsTr("devices · media")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }

                        AppButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddSource("audio")
                        }
                    }

                    Repeater {
                        model: AudioInputListModel

                        delegate: Rectangle {
                            id: inRow
                            required property int index
                            required property string name
                            required property string sublabel
                            required property real level
                            required property bool muted

                            width: board.colW
                            height: board.rowH
                            radius: Theme.radiusMd
                            color: Theme.inset

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

                            // Click-to-mute — the pill IS the mute toggle
                            // for audio rows (any kind can be muted).
                            MutePill {
                                id: inMutePill
                                anchors.right: parent.right
                                anchors.top: parent.top
                                muted: inRow.muted
                                accent: Theme.success
                                accentLight: Theme.successLight
                                onToggleRequested: AudioInputListModel.setMuted(inRow.index, !inRow.muted)
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

                            // LIVE board meter — the row's own device tap
                            // (started below, once per device row), PRE-fader:
                            // a board row meters its SOURCE (the dialogs'
                            // channel strips meter the post-fader chain). The
                            // old post-fader math made a row with a low stored
                            // level (e.g. 10%) invisible even with strong
                            // signal — raw × 0.1 reads as dead. Media/NDI rows
                            // keep the stored fader value: no device, no tap,
                            // no fake signal.
                            LevelTrack {
                                x: 28; y: 48
                                width: board.colW - 90
                                value: {
                                    if (inRow.sublabel === "")
                                        return inRow.level   // media/NDI row
                                    const raw = root.inputMixLevel(inRow.index, inRow.sublabel)
                                    return inRow.muted ? 0 : Math.min(100, raw * 100)
                                }
                                // Color follows the VALUE (VU zones) — the
                                // old hardcoded green read "safe" at 100.
                            }
                            // This row's device tap — ensure-on-row-created.
                            // Idempotent in the bridge; the empty-sublabel
                            // case (media/NDI) never calls it.
                            Component.onCompleted: {
                                if (inRow.sublabel !== "")
                                    root.openAudioMeter(inRow.sublabel)
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

                            // Port dot feeding a connector line toward the
                            // bus column, poking past the card's own edge.
                            // PRESS-DRAG from it onto a bus card to connect.
                            // Grey at rest, lights while hovered/dragging —
                            // the affordance, never the status.
                            PortDot {
                                id: inPortDot
                                anchors.right: parent.right
                                anchors.rightMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.success
                                dragActive: root.dragConnectActive && root.dragConnectFrom === "audio"
                                            && root.dragConnectIndex === inRow.index
                                onConnectStarted: (x, y) => root.startDragConnect("audio", inRow.index, inPortDot, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(inPortDot, x, y)
                                onConnectFinished: (x, y) => root.finishDragConnect("audio", inRow.index, inPortDot, x, y)
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

                        Column {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("BUSES & ROUTING")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                            Text {
                                text: qsTr("audio + video mix")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }

                        AppButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddBus()
                        }
                    }

                    Repeater {
                        model: BusListModel

                        delegate: Rectangle {
                            id: busRow
                            required property int index
                            required property string name
                            required property string type
                            required property real level
                            required property bool muted
                            required property var routedAudioInputs
                            required property var routedVideoSources

                            readonly property color typeColor: busRow.type === "audio" ? Theme.success
                                                              : busRow.type === "video" ? Theme.info
                                                              : Theme.accent
                            readonly property string typeLabel: busRow.type === "audio" ? qsTr("AUDIO")
                                                               : busRow.type === "video" ? qsTr("VIDEO")
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

                            // The pill IS the mute toggle — same shared
                            // component as every other card. Muting a bus
                            // silences its whole output mix.
                            MutePill {
                                id: busMutePill
                                anchors.right: parent.right
                                anchors.top: parent.top
                                muted: busRow.muted
                                accent: busRow.typeColor
                                accentLight: busRow.typeColor
                                onToggleRequested: BusListModel.setMuted(busRow.index, !busRow.muted)
                            }

                            Text {
                                anchors.right: busMutePill.left
                                anchors.rightMargin: 8
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                text: busRow.srcCount + qsTr(" src")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }

                            // LIVE BUS METER — the bus SUMS its routed
                            // audio sources' pre-fader tap levels (a real
                            // mix bus's meter shows the mix feeding it),
                            // then applies the bus's own fader + mute:
                            // post-fader on the bus, pre-fader on the
                            // sources. Empty/unrouted buses sit dark —
                            // the truth, never a synthetic wiggle.
                            LevelTrack {
                                x: 12; y: 48
                                width: board.busColW - 90
                                value: {
                                    let mix = 0
                                    const rows = busRow.routedAudioInputs
                                    for (let i = 0; i < rows.length; i++) {
                                        const r = rows[i]
                                        const data = AudioInputListModel.getInput(r)
                                        if (!data || data.muted)
                                            continue
                                        const raw = root.inputMixLevel(r, data.sublabel)
                                        if (raw > mix) mix = raw   // loudest source wins (peak mix)
                                    }
                                    const post = mix * (busRow.level / 100)
                                    return busRow.muted ? 0 : Math.min(100, post * 100)
                                }
                                // Buses tint by TYPE (categorical), not by
                                // level — an explicit fixed color.
                                fixedColor: true
                                fillColor: busRow.typeColor
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
                                    onClicked: root.openEditBus(busRow.index)
                                }
                            }

                            // Left port (audio side) / right port (video
                            // side). Audio fits EVERY bus (embedded with the
                            // feed on video buses — see sourceFitsBus), so
                            // the audio port is always present; only a pure
                            // audio bus hides the video port. Bus ports are
                            // drop decorations, not drag sources —
                            // interactive: false.
                            PortDot {
                                anchors.left: parent.left
                                anchors.leftMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.success
                                interactive: false
                                alwaysColored: true
                            }
                            PortDot {
                                visible: busRow.type !== "audio"
                                anchors.right: parent.right
                                anchors.rightMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.info
                                interactive: false
                                alwaysColored: true
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

                        Column {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: qsTr("VIDEO SOURCES")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                            Text {
                                text: qsTr("cams · screen · media · ndi")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }

                        AppButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddSource("video")
                        }
                    }

                    Repeater {
                        model: VideoSourceListModel

                        delegate: Rectangle {
                            id: vidRow
                            required property int index
                            required property string name
                            required property string sublabel
                            required property string kind
                            required property bool muted
                            required property real level

                            // Mute exists on EVERY video row: media mute
                            // silences its audio, camera/screen mute blacks
                            // the feed out at the bus it feeds.
                            readonly property bool hasAudio: vidRow.kind === "media"
                                                             || vidRow.kind === "ndi"
                            // The row's live thumbnail source (nonce-pumped
                            // by the thumb itself — see vidThumb). Cameras
                            // AND screens: the monitor label resolves through
                            // the same videopreview provider.
                            readonly property url liveThumb:
                                vidThumb.liveCam
                                ? "image://videopreview/" + encodeURIComponent(vidRow.sublabel)
                                  + "?n=" + vidThumb.thumbNonce : ""

                            width: board.vidColW
                            height: board.vidRowH
                            radius: Theme.radiusMd
                            color: Theme.inset

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

                            // Name + sublabel run the full width up to the
                            // thumb — vidColW is what keeps a normal source
                            // name from eliding, not extra row height.
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

                            // The pill IS the mute toggle on EVERY video
                            // row. The top-right CORNER is owned by the
                            // preview thumb — the pill slots immediately
                            // left of it (anchoring to the card's right
                            // edge put it UNDER the thumb, invisible).
                            // Its audio LEVEL meter stays audio-only
                            // (camera/screen have no audio track to meter).
                            MutePill {
                                id: vidMutePill
                                anchors.right: vidThumb.left
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                muted: vidRow.muted
                                // A camera/screen feed isn't "muted" when
                                // off, it's paused — media and NDI carry
                                // audio to mute.
                                offLabel: vidRow.hasAudio ? qsTr("MUTE") : qsTr("PAUSED")
                                accent: Theme.info
                                accentLight: Theme.infoLight
                                onToggleRequested: VideoSourceListModel.setMuted(vidRow.index, !vidRow.muted)
                            }

                            LevelTrack {
                                x: 28
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 14
                                visible: vidRow.hasAudio
                                // Stop short of the Edit link's hover area.
                                width: vidThumb.x - 28 - 44
                                value: vidRow.level
                                fillColor: Theme.info
                            }

                            // ---- Preview thumb — every video source has
                            // one. LIVE for camera rows: the row's own MF tap
                            // (started below at the row's stored mode), nonce-
                            // pumped at thumb rate; other kinds keep the dark
                            // pane + glyph.
                            Rectangle {
                                id: vidThumb
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.top: parent.top
                                anchors.topMargin: 12
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 12
                                // Sized off this pane's OWN height (row minus
                                // its top/bottom margins), not the row height
                                // itself — using the row height directly for
                                // a 16:9 ratio made the thumb far wider than
                                // it is tall, eating most of the card width.
                                // The 96 cap keeps a taller row from growing
                                // the thumb into the name's room: past it the
                                // pane stops being 16:9 and letterboxes.
                                width: Math.min(96, Math.round((height) * 16 / 9))
                                radius: Theme.radiusSm
                                color: "#0d0f16"
                                border.width: 1
                                border.color: Theme.border
                                clip: true

                                // LIVE FRAME for camera rows — the tap's newest
                                // JPEG, nonce-pumped slower than the dialogs
                                // (a thumbnail doesn't need 15 fps; 8 reads
                                // smooth).
                                Image {
                                    id: frameImg
                                    anchors.fill: parent
                                    source: vidRow.liveThumb
                                    visible: (vidRow.kind === "camera" || vidRow.kind === "screen")
                                             && status === Image.Ready && sourceSize.width > 1
                                             && !vidRow.muted
                                    fillMode: vidRow.kind === "screen" ? Image.PreserveAspectFit
                                                                       : Image.PreserveAspectCrop
                                    cache: false
                                }
                                // The row's tap + pump, reconciled like the
                                // dialog pane (VideoPreviewPane.syncTap) and
                                // sharing its KEEP-ALIVE pool discipline: a
                                // device change STARTS the new tap and leaves
                                // the old one running — no per-switch stop
                                // (repeated close/re-open exhausts virtual-cam
                                // drivers; the screen-wide stopAllVideoPreviews
                                // at teardown is the only release).
                                readonly property bool liveCam:
                                    (vidRow.kind === "camera" || vidRow.kind === "screen")
                                    && vidRow.sublabel !== ""
                                    && !vidRow.muted
                                // Frames actually flowing — the warm-up test that
                                // retires the decorative glyphs (same rule as the
                                // dialogs' pane): a live thumb must not wear the
                                // lens/monitor art on top of real pixels.
                                readonly property bool thumbLive:
                                    frameImg.status === Image.Ready && frameImg.sourceSize.width > 1
                                property int thumbNonce: 0
                                property string activeTapLabel: ""
                                // The PREVIOUS tap's kind — the stop call must
                                // address the OLD label's kind, not the row's
                                // current one: after a camera→screen re-pick, a
                                // stop routed by the new kind would call
                                // stopScreenPreview for a camera label, and the
                                // camera's tap would stream on forever.
                                property string activeTapKind: ""
                                // PAUSE FREES THE DEVICE: a muted row's tap is
                                // stopped outright (camera light off) instead of
                                // pooled. liveCam folds the mute state in, so the
                                // reconcile below sees want=="" and releases.
                                function syncTap() {
                                    const want = vidThumb.liveCam ? vidRow.sublabel : ""
                                    if (want === activeTapLabel)
                                        return
                                    const old = activeTapLabel
                                    const oldKind = activeTapKind
                                    activeTapLabel = want
                                    activeTapKind = want !== "" ? vidRow.kind : ""
                                    if (old !== "") {
                                        if (oldKind === "screen")
                                            EngineBridge.stopScreenPreview(old, "board")
                                        else
                                            EngineBridge.stopVideoPreview(old, "board")
                                    }
                                    if (want !== "") {
                                        if (vidRow.kind === "screen")
                                            EngineBridge.startScreenPreview(want, "board")
                                        else
                                            EngineBridge.startVideoPreview(want, "", "board")
                                    }
                                }
                                Timer {
                                    interval: 125   // 8 fps thumbnail pump
                                    repeat: true
                                    running: vidThumb.liveCam && root.visible
                                    onTriggered: vidThumb.thumbNonce++
                                }
                                Component.onCompleted: vidThumb.syncTap()
                                onLiveCamChanged: vidThumb.syncTap()
                                // sublabel lives on the delegate (vidRow), not
                                // on this Rectangle — observe it explicitly.
                                Connections {
                                    target: vidRow
                                    function onSublabelChanged() { vidThumb.syncTap() }
                                }

                                // Camera — a lens ring + center dot (warm-up only).
                                Item {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "camera" && !vidThumb.thumbLive

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

                                // NDI — broadcast ripples (center dot +
                                // two rings) for network sources.
                                Item {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "ndi"

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

                                // Screen — a monitor outline + stand (warm-up
                                // only, same as the lens above). The wrapper
                                // needs REAL dimensions: children anchor to it,
                                // and an implicit 0x0 Item stacks them at the
                                // center point.
                                Item {
                                    anchors.centerIn: parent
                                    width: 22; height: 16
                                    visible: vidRow.kind === "screen" && !vidThumb.thumbLive

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

                                // Media — a play triangle (Shapes is already
                                // imported for the connector lines).
                                Shape {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "media"
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

                            // Edit link — bottom-right, same as every card.
                            // The thumb owns the corner, so it slots left of
                            // the thumb like the pill above it.
                            Text {
                                anchors.right: vidThumb.left
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

                            // Port dot on the LEFT edge — video sources sit
                            // to the right of the bus column, so their
                            // connector line runs right-to-left.
                            // PRESS-DRAG from it onto a bus card to connect.
                            // Same grey-at-rest treatment as the audio port.
                            PortDot {
                                id: vidPortDot
                                anchors.left: parent.left
                                anchors.leftMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                accent: Theme.info
                                dragActive: root.dragConnectActive && root.dragConnectFrom === "video"
                                            && root.dragConnectIndex === vidRow.index
                                onConnectStarted: (x, y) => root.startDragConnect("video", vidRow.index, vidPortDot, x, y)
                                onConnectMoved: (x, y) => root.updateDragConnect(vidPortDot, x, y)
                                onConnectFinished: (x, y) => root.finishDragConnect("video", vidRow.index, vidPortDot, x, y)
                            }
                        }
                    }
                }

                // ---- Drag-connect ghost line ----
                // Drawn last (on top of the cards) while a drag is live,
                // from the source port to the cursor. Green when hovering a
                // compatible bus, red over an incompatible one.
                Shape {
                    visible: root.dragConnectActive
                    anchors.fill: parent
                    z: 10
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        strokeColor: root.dragConnectHoverBus < 0 ? Theme.textMuted
                                   : root.dragConnectCompat ? Theme.success
                                   : Theme.danger
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
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }

    // ---- Row context menu — one shared instance, retargeted per right-
    // click (same pattern as OutputsScreen's card menu).
    // MenuCatcher sits under the menu (declared first) so any click or
    // scroll outside it dismisses instead of leaving it stuck over the
    // board; the menu's z raises it above the catcher.
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

    // ---- Edit Audio Input dialog ----
    // Effects here are immediate-apply (same rationale as the bus routing
    // toggles: cheap to reverse, benefits from live feedback) — the panel's
    // handlers call setEffectEnabled/setEffectValue directly against
    // root.editAudioIndex, and editAudioEffects re-reads through modelsRev.
    ModalCard {
        id: editAudioDialog
        shown: root.editAudioIndex >= 0
        title: qsTr("Edit Audio Input")
        cardWidth: 640
        saveText: qsTr("Save Changes")
        onCancelled: root.editAudioIndex = -1
        onAccepted: root.saveEditAudio()

        // (The signal pane was removed per design review — the dialog now
        // opens straight on the pro-audio form, matching the reference.
        // The mic mute button lived in that pane; muted stays in the model
        // and the channel meters still honor it, but there is no mute
        // control in this dialog until a row surface lands.)
        Row {
            width: parent.width
            spacing: Theme.space4

            // The dial is gone from this dialog — the pro-audio form's
            // Volume row (ProAudioForm, below the fields) owns level now.

            Column {
                anchors.top: parent.top
                width: parent.width
                spacing: Theme.space4

                // Extra breathing room under the header — this dialog has no
                // subtitle, so the Kind row sat right up against the title
                // with just the generic header gap, reading as crowded.
                Item { width: 1; height: Theme.space2 }

                // Kind row — FIRST, on the same rail: the Source row's
                // label and options below follow this selection.
                Item {
                    width: parent.width
                    height: 26

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Kind")
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
                            model: root.audioKinds
                            delegate: SelectableChip {
                                required property var modelData
                                label: modelData.label
                                selected: root.editAudioKind === modelData.key
                                onPicked: {
                                    // Remember this kind's pick before leaving it, restore
                                    // the new kind's last pick if it had one — a bare reset
                                    // here permanently blanked Device's pick the moment you
                                    // so much as glanced at Media and came back.
                                    const buf = Object.assign({}, root.editAudioSublabelByKind)
                                    buf[root.editAudioKind] = root.editAudioSublabel
                                    root.editAudioSublabelByKind = buf
                                    root.editAudioKind = modelData.key
                                    root.editAudioSublabel = buf[modelData.key] !== undefined
                                                              ? buf[modelData.key] : ""
                                    root.syncAudioMeter()   // non-device kinds un-meter
                                }
                            }
                        }
                    }
                }

                // Pro-audio form — the reference layout: Name / Source on
                // a shared 76px label rail, then Delay, Volume, Channels
                // (checkbox rows + pill meters + green gain knobs).
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
                        root.editAudioSublabel = root.resolveAudioSourcePick(v, root.editAudioSublabel)
                        root.syncAudioMeter()   // re-target the tap at the new device
                    }
                    delayMs: root.editAudioDelayMs
                    volume: root.editAudioLevel
                    muted: root.editAudioMuted
                    channels: root.editAudioEffectiveChannels
                    // REAL telemetry — the engine's WASAPI tap for this row's
                    // device, not a volume-slider echo. While metering, the
                    // tap's live layout (mono/stereo/multi) re-renders the
                    // channel rows to match the capture format.
                    meterLevels: root.audioMeterLevels
                    meterLayout: root.audioMeterLayout
                    // Per-channel gain knobs read the model row; a moved knob
                    // writes straight through (persisted, debounced).
                    gainFor: function(i) {
                        return root.editAudioGains.length > i ? root.editAudioGains[i] : 1.0
                    }
                    onChannelGainEdited: (i, g) =>
                        AudioInputListModel.setChannelGain(root.editAudioIndex, i, g)
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
                // Deferred model write: the write rebuilds this rack (the
                // panel is bound to the model through modelsRev), and a
                // synchronous rebuild inside the chip's own click event
                // destroys the dispatching delegate mid-event. Same hazard
                // as the routing board's line hit-testing — never mutate
                // the model that rebuilds your own delegate from within its
                // event. (Deferring here is the other half of the panel's
                // emit-now contract; see AudioEffectsPanel.qml.)
                Qt.callLater(() => {
                    if (root.editAudioIndex < 0) return
                    const cur = root.editAudioEffects.find((e) => e.key === key)
                    AudioInputListModel.setEffectEnabled(root.editAudioIndex, key, cur ? !cur.enabled : true)
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
                    // Routes-by-index must be compacted before the row goes
                    // (same path as the context menu's Delete).
                    root.removeAudioFixingRoutes(root.editAudioIndex)
                    root.editAudioIndex = -1
                }
            }
        }
    }

    // ---- Edit Video Source dialog ----
    // Preview-first flow (name → kind → preview [+ volume for media] →
    // device) — both previews sit right under the kind picker instead of
    // scrolled below the device field, and the pane itself is a real 16:9
    // instead of the flat 180px strip this started as.
    ModalCard {
        id: editVideoDialog
        shown: root.editVideoIndex >= 0
        title: qsTr("Edit Video Source")
        cardWidth: 640
        saveText: qsTr("Save Changes")
        onCancelled: root.editVideoIndex = -1
        onAccepted: root.saveEditVideo()

        SettingsField {
            width: parent.width
            label: qsTr("Name")
            text: root.editVideoName
            onTextEdited: (t) => root.editVideoName = t
        }            Column {
                width: parent.width
                spacing: Theme.space2
                Text {
                    text: qsTr("Kind")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                Row {
                    spacing: Theme.space2
                    Repeater {
                        model: root.videoKinds
                        delegate: SelectableChip {
                            required property var modelData
                            label: modelData.label
                            selected: root.editVideoKind === modelData.key
                            onPicked: {
                                // Remember this kind's pick, restore the new
                                // kind's last one — see editAudioSublabelByKind.
                                const buf = Object.assign({}, root.editVideoSublabelByKind)
                                buf[root.editVideoKind] = root.editVideoSublabel
                                root.editVideoSublabelByKind = buf
                                root.editVideoKind = modelData.key
                                root.editVideoSublabel = buf[modelData.key] !== undefined
                                                          ? buf[modelData.key] : ""
                                root.editVideoMode = ""
                            }
                        }
                    }
                }
            }

        // ---- Preview (all kinds) — the SHARED VideoPreviewPane component:
        // 16:9 pane, kind glyphs, MUTE/PAUSED pill, Resolution mode pill and
        // the kind hint live there ONCE; both video dialogs bind their own
        // state onto it.
        VideoPreviewPane {
            width: parent.width
            kind: root.editVideoKind
            previewLabel: root.editVideoKind === "camera" || root.editVideoKind === "screen"
                          ? root.editVideoSublabel : ""
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

        // Volume for kinds whose feed carries audio — media files and
        // NDI streams (NDI embeds audio with its video). A plain line
        // meter with its value, not the fuller dial + animated-bar
        // treatment the Edit Audio dialog gets: this is a supplementary
        // control on a VIDEO dialog, kept simple on purpose.
        LabeledSlider {
            width: parent.width
            visible: root.videoKindHasAudio(root.editVideoKind)
            label: qsTr("Volume")
            suffix: "%"
            value: root.editVideoLevel
            onMoved: (v) => root.editVideoLevel = v
        }

        NdiRuntimeNotice {
            width: parent.width
            active: root.editVideoKind === "ndi"
        }

        // Source — REAL hardware selects, kind-driven (camera = the
        // engine's Media Foundation roster). Picking a device resets the
        // mode pick: the old mode may not exist on the new device.
        SelectField {
            width: parent.width
            label: qsTr("Source")
            placeholder: root.editVideoKind === "camera" && root.engineVideoDevices.length === 0
                         ? qsTr("No camera found")
                         : qsTr("Pick a source")
            options: root.videoSourceOptions(root.editVideoKind)
            value: root.editVideoSublabel
            onValuePicked: (v) => {
                root.editVideoSublabel = root.resolveVideoSourcePick(v, root.editVideoSublabel)
                root.editVideoMode = ""
            }
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
        shown: root.busDialogOpen
        title: root.busDialogIsAdd ? qsTr("Add Bus") : qsTr("Edit Bus")
        subtitle: root.busDialogIsAdd
                  ? qsTr("Create a bus, then right-click it to route sources in.")
                  : qsTr("Route audio inputs and/or video sources into this bus.")
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
            Text {
                text: qsTr("Type")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
            Row {
                spacing: Theme.space2
                Repeater {
                    model: [
                        { key: "audio", label: qsTr("Audio") },
                        { key: "video", label: qsTr("Video") },
                        { key: "both", label: qsTr("Both") }
                    ]
                    delegate: SelectableChip {
                        required property var modelData
                        label: modelData.label
                        selected: root.editBusType === modelData.key
                        onPicked: root.editBusType = modelData.key
                    }
                }
            }
        }

        // ---- Channel strip (ProAudioForm) — the bus is a real channel:
        // master fader (gain-dB readout), mute via the strip's rows, and a
        // stereo L/R MASTER pair (the bus's own post-fader mix, same
        // detailed meter + glowing speedometer knob every input channel
        // row gets) above the per-source routed rows below. A bus has no
        // independent stereo capture of its own, so both channels mirror
        // the same live mix (the loudest routed, gain-aware source) — the
        // same simplification the Media pane's bus L/R buttons use.
        ProAudioForm {
            width: parent.width
            visible: root.editBusType !== "video"
            nameLabel: qsTr("Master")
            sourceLabel: qsTr("Sources")
            sourceValue: qsTr("%1 routed").arg(root.busRoutedAudio().length)
            sourceOptions: []
            delayMs: 0
            volume: root.editBusLevel
            muted: root.editBusMuted
            channels: 2
            meterLevels: {
                const lvl = root.editBusIndex >= 0 && !root.editBusMuted
                    ? root.busMixLevel(root.editBusIndex) : 0
                return [lvl, lvl]
            }
            meterLayout: "stereo"
            onVolumeEdited: (v) => root.editBusLevel = v
            onMutedToggled: root.editBusMuted = !root.editBusMuted
        }

        // The per-source routing list ("Audio inputs routed in" below) is the one place that manages which sources feed this bus — the Channel 1/Channel 2 rows above are the bus's own output, independently selectable there.

        Item {
            width: parent.width
            height: 34
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Muted")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            SettingsToggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: root.editBusMuted
                onToggled: root.editBusMuted = !root.editBusMuted
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2
            // Audio fits every bus, so this section is always available in
            // edit mode (sourceFitsBus is the one rule; see its comment).
            visible: !root.busDialogIsAdd
            Text {
                text: qsTr("Audio inputs routed in")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
            Repeater {
                model: AudioInputListModel
                delegate: Item {
                    required property int index
                    required property string name
                    width: parent.width
                    height: 30
                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: name
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                    SettingsToggle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.busRoutedAudio().indexOf(index) >= 0
                        onToggled: BusListModel.toggleAudioRoute(root.editBusIndex, index)
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2
            visible: !root.busDialogIsAdd && root.editBusType !== "audio"
            Text {
                text: qsTr("Video sources routed in")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }
            Repeater {
                model: VideoSourceListModel
                delegate: Item {
                    required property int index
                    required property string name
                    width: parent.width
                    height: 30
                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: name
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                    SettingsToggle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.busRoutedVideo().indexOf(index) >= 0
                        onToggled: BusListModel.toggleVideoRoute(root.editBusIndex, index)
                    }
                }
            }
        }

        Row {
            width: parent.width
            visible: !root.busDialogIsAdd
            AppButton {
                text: qsTr("Delete Bus")
                variant: "danger"
                onClicked: {
                    BusListModel.removeBus(root.editBusIndex)
                    root.busDialogOpen = false
                }
            }
        }
    }

    // ---- Add Source dialog (VGRPresenter · Settings · Audio & Video · Add) ----
    // One dialog for both types — the Audio/Video chips pick which roster
    // the row lands in. Audio gets the meter/dial/mute/effects rack; video
    // gets name/type/kind/device/mute only (no level or effects in the
    // video data model, matching the reference).
    ModalCard {
        id: addSourceDialog
        shown: root.addSourceShown
        title: qsTr("Add Source")
        subtitle: qsTr("Add an audio input or video source, set its level, and dial in its effects rack.")
        cardWidth: 640
        saveText: qsTr("Add Source")
        onCancelled: root.addSourceShown = false
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
                        root.syncAudioMeter()   // video rows never meter
                    }
                }
            }
        }

        // Audio fields — one column (the pro-audio form, then Kind).
        // CAUTION: this was once a Row with two full-width Columns — QML
        // pushed the second outside the dialog, clipping the whole form
        // into invisible blank space. (The signal pane above the form was
        // removed per design review; same mute note as the Edit dialog.)
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: root.addSourceType === "audio"

            Column {
                width: parent.width
                spacing: Theme.space4

                // Kind row — FIRST, on the same rail: the Source row's
                // label and options below follow this selection.
                Item {
                    width: parent.width
                    height: 26

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Kind")
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
                            model: root.audioKinds
                            delegate: SelectableChip {
                                required property var modelData
                                label: modelData.label
                                selected: root.addSourceKind === modelData.key
                                onPicked: {
                                    // Remember this kind's pick, restore the new
                                    // kind's last one — see editAudioSublabelByKind.
                                    const buf = Object.assign({}, root.addSourceSublabelByKind)
                                    buf[root.addSourceKind] = root.addSourceSublabel
                                    root.addSourceSublabelByKind = buf
                                    root.addSourceKind = modelData.key
                                    root.addSourceSublabel = buf[modelData.key] !== undefined
                                                              ? buf[modelData.key] : ""
                                    root.syncAudioMeter()
                                }
                            }
                        }
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
                        root.addSourceSublabel = root.resolveAudioSourcePick(v, root.addSourceSublabel)
                        root.syncAudioMeter()
                    }
                    delayMs: root.addSourceDelayMs
                    volume: root.addSourceLevel
                    muted: root.addSourceMuted
                    channels: root.addSourceEffectiveChannels
                    // REAL telemetry — same WASAPI tap as the Edit dialog
                    // (the two dialogs are mutually exclusive, so whichever
                    // opened last owns the singleton tap).
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

            SettingsField {
                width: parent.width
                label: qsTr("Name")
                placeholder: qsTr("e.g. “Cam 3 · Aisle”")
                text: root.addSourceName
                onTextEdited: (t) => root.addSourceName = t
            }

            Column {
                width: parent.width
                spacing: Theme.space2
                Text {
                    text: qsTr("Kind")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                Row {
                    spacing: Theme.space2
                    Repeater {
                        model: root.videoKinds
                        delegate: SelectableChip {
                            required property var modelData
                            label: modelData.label
                            selected: root.addSourceKind === modelData.key
                            onPicked: {
                                // Remember this kind's pick, restore the new
                                // kind's last one — see editAudioSublabelByKind.
                                const buf = Object.assign({}, root.addSourceSublabelByKind)
                                buf[root.addSourceKind] = root.addSourceSublabel
                                root.addSourceSublabelByKind = buf
                                root.addSourceKind = modelData.key
                                root.addSourceSublabel = buf[modelData.key] !== undefined
                                                          ? buf[modelData.key] : ""
                                root.addSourceVideoMode = ""
                            }
                        }
                    }
                }
            }

            NdiRuntimeNotice {
                width: parent.width
                active: root.addSourceKind === "ndi"
            }

            // Source — REAL hardware for the camera kind (the engine's Media
            // Foundation roster), screen/media option sets otherwise.
            SelectField {
                width: parent.width
                label: qsTr("Source")
                placeholder: root.addSourceKind === "camera" && root.engineVideoDevices.length === 0
                             ? qsTr("No camera found")
                             : qsTr("Pick a source")
                options: root.videoSourceOptions(root.addSourceKind)
                value: root.addSourceSublabel
                onValuePicked: (v) => {
                    root.addSourceSublabel = root.resolveVideoSourcePick(v, root.addSourceSublabel)
                    // A new device is a new capability set — the old mode
                    // pick may not exist on it.
                    root.addSourceVideoMode = ""
                }
            }

            // ---- Preview — the SHARED VideoPreviewPane component (the
            // same instance the Edit Video dialog uses): pane, glyphs,
            // MUTE/PAUSED pill, mode pill live there once. No hint here —
            // the Add column is tighter.
            VideoPreviewPane {
                width: parent.width
                showHint: false
                paneObjectName: "selfTestPreviewPane"   // the ADD dialog's pane — the grab target
                kind: root.addSourceKind
                previewLabel: root.addSourceKind === "camera" || root.addSourceKind === "screen"
                              ? root.addSourceSublabel : ""
                muted: root.addSourceMuted
                mode: root.addSourceVideoMode
                modes: root.videoModesFor(root.addSourceSublabel)
                maxFps: root.videoMaxFpsFor(root.addSourceSublabel)
                onMutedToggled: root.addSourceMuted = !root.addSourceMuted
                onModePicked: (m) => root.addSourceVideoMode = m
            }

            // Volume for audio-carrying kinds (media, NDI) — a plain
            // line meter with its value, not the fuller dial +
            // animated-bar treatment the audio side gets: kept simple
            // on this video dialog.
            LabeledSlider {
                width: parent.width
                visible: root.videoKindHasAudio(root.addSourceKind)
                label: qsTr("Volume")
                suffix: "%"
                value: root.addSourceLevel
                onMoved: (v) => root.addSourceLevel = v
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
                // Same deferred write as the Edit dialog — the reassignment
                // rebuilds the rack, and the dispatching chip must survive
                // its own click event. (Reassigning the whole var array is
                // also what notifies QML's var-property change detection;
                // plain in-place mutation wouldn't.)
                Qt.callLater(() => {
                    root.addSourceEffects = root.addSourceEffects.map((e) =>
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
