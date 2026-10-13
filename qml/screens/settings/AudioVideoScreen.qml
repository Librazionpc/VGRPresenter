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
// Audio routing is many-to-many; video sources can feed multiple buses, while
// each bus renders one video frame. Ducking settings and a real audio/
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
        // A wholesale roster replacement (session restore, a data-protection
        // restore) resets the model instead: the row signals never fire, so
        // without this the board's meters and meter holds would keep pointing
        // at the pre-restore rows.
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
    // dialog's video-device pick without poking pixels. This changes the
    // selection state only; Settings does not start audio capture.
    // kind selects the row type: "camera" (default) or "screen" — screens
    // sweep displays AND open windows through the same pane.
    function selfTestPickVideoSource(label, kind) {
        root.addSourceType = "video"
        root.addSourceKind = kind || "camera"
        root.addSourceSublabel = label
        root.addSourceShown = true
    }

    // ---- Live input metering (the board rows' + dialogs' real VU feed) ----
    // This screen DISPLAYS live levels — every board row's LevelTrack and the
    // audio dialogs' channel strips — so it holds a WASAPI tap for exactly the
    // devices it is showing and releases those when they leave. The bridge's
    // taps are REFCOUNTED per device label, so a hold taken here never
    // disturbs — and is never disturbed by — the Media pane's own hold on the
    // same microphone.
    //
    // Deliberately NOT stopAllInputMeters() on the way out: an earlier version
    // did that, and closing Settings silently killed the meters the Media pane
    // still had on screen — which is why this screen's metering was then
    // stripped out altogether. Only the labels in heldAudioMeters are released.
    readonly property var audioMeterList: EngineBridge.inputLevels
    function meterSnapshotFor(label) {
        const want = label === undefined ? "" : String(label).trim()
        // "NO DEVICE CHOSEN" IS NOT A DEVICE. The bridge refuses an empty-label
        // tap now (EngineBridge::resolveAndStartMeter), but a snapshot for ""
        // must ALSO never be displayed here: the empty label is the state of a
        // dialog/row the user has not picked a source for, and it used to match
        // whatever the bridge had published under "" — so a strip (the Add
        // dialog's, a board row's) animated off a tap started somewhere else
        // entirely while its own Source field read empty. Every reader goes
        // through this function, so one guard covers the board and both dialogs.
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
    // The gain-aware, per-channel-mute-aware live level for one audio input
    // row — the loudest of its channels, each scaled by that channel's own
    // gain (0 when a channel's L/R button muted it in the Media pane).
    // Every "how loud is this input right now" reading (the board row's own
    // meter, a bus's mix, an Edit Bus routed-source row) MUST go through
    // this, not a bare peaks[0]: reading channel 0 only meant muting the
    // RIGHT channel from its own card never showed anywhere else — the
    // bus and every downstream reading kept reporting the untouched left
    // channel as if nothing had changed.
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
    // The gain-aware live level of one VIDEO row's own audio — the exact shape
    // and rules inputChannelLevels uses (a mono file's single channel feeds both
    // sides; each side scaled by its own pill's trim and by the row's fader, and
    // a muted row is silence), read from the SAME snapshot list.
    // `muted`/`level` are the CALLER's truth: a board reading its routed rows
    // passes the stored row state, an open Edit dialog passes its own buffered
    // (unsaved) fader so moving it moves the bars.
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
    // The live level of a video source that has NO row yet (the Add dialog's
    // pick): the snapshot alone, gated on the state that dialog holds. Same
    // rules as videoChannelLevels otherwise — a mono file's single channel
    // feeds both sides, and a muted row reads as silence.
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
    // The ONE number a video row's own LevelTrack wants: the same live,
    // gain/mute-aware reading the Edit dialog's strips use (videoChannelLevels),
    // collapsed to its loudest side and handed over as the 0..100 linear
    // percentage LevelTrack takes.
    //
    // The row's bar used to be handed the STORED volume (`vidRow.level`), so it
    // sat at a fixed fill that tracked the Volume slider and never moved — the
    // "media audio is stuck" report, and the exact OPPOSITE of the Edit dialog,
    // which reads the stream tap: a row whose file is gone (or simply silent
    // right now) drew a level on the card and -inf dB in the dialog. A meter
    // reads the signal, not a setting. With no tap — an unrouted row, a missing
    // file, a muted row, a camera/screen feed — there is no audio in it, so this
    // rests at zero the same way the dialog's strips do.
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

    // The incoming live source mix, after each source fader and channel gain.
    // ProAudioForm applies the bus fader and mute once, producing the actual
    // post-bus level for the Edit Bus master meters.
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
        // A VIDEO row routed into this bus carries its own audio (a media file's
        // track, NDI's embedded audio) and the engine feeds the bus's mix from
        // it — so the bus's own meters read it too. Without this the strip sat
        // at zero for a bus whose only source was a video row (the "I connected
        // the video source to the bus and the bus never animates" report).
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
    // Is one side of the bus OPEN, in the SAME sense an input row's channel is
    // (gain 0 ≡ closed)? A bus has no per-channel trim of its own — the engine's
    // VolumeControl carries gain/mute/solo/pan, not per-channel gains — so its
    // two channel rows are a READ-THROUGH of what feeds them: a side is open
    // when at least one routed, unmuted source has that channel open. Nothing
    // routed reads as open, exactly like a fresh input row. This is what makes
    // the Edit Bus strip render a closed channel the way Edit Audio Input
    // renders one: same checkbox, same dimmed row, same dark bar.
    function busChannelOpen(busIndex, channel) {
        if (busIndex < 0) return true
        const b = BusListModel.getBus(busIndex)
        const routed = b.routedAudioInputs
        if (routed.length === 0) return true
        for (const r of routed) {
            const data = AudioInputListModel.getInput(r)
            if (!data || data.muted) continue
            // A MONO source feeds both sides with its single channel (the same
            // rule the meters and the input dialog's own rows use), so channel
            // 2 must not read closed just because the device is mono.
            const count = Number(data.channels) > 0 ? Number(data.channels) : 2
            const sourceChannel = count === 1 ? 0 : channel
            if (!Db.isChannelOff(AudioInputListModel.channelGain(r, sourceChannel)))
                return true
        }
        return false
    }
    // The dialog feed: the device the open audio dialog is pointed at (see
    // audioMeterTargets below — the dialog's own hold keeps that device's tap
    // live, whether or not a Media card also meters it).
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
    // A roster row holds its device only while that row is really visible (see
    // InViewProbe): a board listing twenty inputs must not run twenty captures
    // to feed meters nobody is looking at, however long the roster has been set
    // up — the moment a row scrolls out of view its device is released. The
    // delegates report their own state here; the target lists below read the map
    // (plus the rev counter, the same reactivity bridge the rest of this screen
    // uses for Q_INVOKABLE model reads).
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

    // Every device label this screen must be metering right now.
    //
    // THE BOARD METERS THE INPUTS IT IS SET UP WITH, NEVER THE EMPTY ONES.
    // This used to hold a tap for EVERY device-kind row on the board, whether
    // or not it had a device at all, so merely opening Settings - Audio & Video
    // opened a real WASAPI capture session on each input with nothing
    // selected: the machine's microphone went live, and every strip that reads
    // a published snapshot animated off it — the monitor preview included, via
    // its tapped-input fallback. That is the "I did not click on any audio
    // source and the WASAPI is being opened" report.
    //
    // The rule is the one the Media pane's card toggle already follows (see
    // MediaLibraryPane::toggleAudioMeter): a tap belongs to what the user
    // actually turned on. Two gestures count here: CHOOSING A DEVICE on a
    // board row (the row's Source field — a card is set up with a real input,
    // and its meter is the only proof the mic works), and opening a dialog (the
    // row click below stamps editAudioIndex), which must meter its own device
    // even before a row exists for it — the Add dialog's pick is not a roster
    // row yet, so it needs a hold of its own.
    //
    // An EMPTY label is deliberately NOT metered, and the bridge refuses it too
    // (EngineBridge::startInputMeter): metering "" opened a capture session on
    // the machine's DEFAULT input the moment a dialog appeared with nothing
    // picked — the channel strips animated off the default microphone and the
    // monitor wall's audio button lit up (user call: "it is animating without
    // any device selected … so the WASAPI is being opened"). A tap belongs to a
    // device the user actually chose.
    //
    // modelsRev is the mandatory dependency: rowCount()/getInput() are plain
    // Q_INVOKABLE reads, so without it this never recomputes when the roster
    // changes (the same reactivity-bridge pattern the screen uses everywhere).
    readonly property var audioMeterTargets: {
        void root.modelsRev
        const labels = []
        const add = (value) => {
            const label = value === undefined || value === null ? "" : String(value).trim()
            if (label === "" || labels.indexOf(label) >= 0)
                return
            labels.push(label)
        }
        // Every device-kind board row that HAS a device chosen. A row's Source
        // field is the gesture: picking "Microphone (Realtek(R) Audio)" on a
        // card is the user turning that input on, so its card meters like its
        // own Edit dialog does. A row with an EMPTY Source is still NOT metered
        // (see the note above) — that is the state the "I did not click on any
        // audio source and the WASAPI is being opened" report was in.
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
    // Idempotent diff of the holds against the target list: release what left,
    // start what arrived, leave everything else untouched. Any surface can call
    // it after moving a dialog — a no-op when nothing moved.
    function syncAudioMeters() {
        const wanted = root.audioMeterTargets
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
    Component.onCompleted: {
        root.syncAudioMeters()
        root.syncVideoMeters()   // the screen's first stream-meter reconcile
    }

    // ---- Video/NDI rows' OWN audio (the bridge's STREAM taps) -------------
    // A media file or an NDI stream carries audio no capture device can reach,
    // so the bridge's stream tap is its carrier — and it publishes in the same
    // inputLevels list under the row's own sublabel, which is why every reader
    // below needed no new lookup. The hold discipline is exactly the device
    // meters': this screen holds a tap for every roster row whose feed carries
    // audio (media/ndi — a camera or a screen capture has none) plus whichever
    // row an open video dialog is pointed at, and releases only its own holds.
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
        for (let r = 0; r < VideoSourceListModel.rowCount(); ++r) {
            // On screen only, exactly like the audio rows above: a media/NDI
            // row's stream tap is a DECODER, and a row scrolled out of view has
            // nobody watching the strip it feeds.
            if (!root.rowInView("video", r))
                continue
            const data = VideoSourceListModel.getSource(r)
            if (data) add(data.sublabel, data.kind)
        }
        if (root.editVideoIndex >= 0)
            add(root.editVideoSublabel, root.editVideoKind)
        // The Add dialog's pick is not a roster row yet, so it needs its own
        // hold — the same reason the Add AUDIO dialog appears in
        // audioMeterTargets.
        if (root.addSourceShown && root.addSourceType === "video")
            add(root.addSourceSublabel, root.addSourceKind)
        return out
    }
    // The stream labels this screen holds (its own bookkeeping — never a
    // reflection of what the rest of the app meters).
    property var heldVideoMeters: []
    // Idempotent diff, same contract as syncAudioMeters: release what left,
    // start what arrived, leave the rest running (a media tap is a DECODER —
    // tearing down and rebuilding one on every roster edit would restart the
    // row's audio for no reason).
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
    // Which dialog opened it decides where Apply lands: edit writes the
    // model row directly; add writes the buffered pre-row state (consumed
    // by submitAddSource).
    property string routingModalFor: ""   // "add" | "edit"
    RoutingMatrixModal {
        id: routingModal
        // ABOVE the Edit/Add dialogs (declared before them, and QML siblings
        // stack in declaration order — the modal opened UNDER the edit dialog
        // and looked like "nothing happened"). z beats declaration order.
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

    // ---- Media Library picker — shared by all four Source "Browse..."
    // spots (Edit/Add × Audio/Video). Reuses the Edit screen's own "Select
    // Media" grid (MediaSourceModal) instead of jumping straight to a bare
    // native file dialog, restricted to one kind via fixedKind. Which
    // dialog's sublabel gets the pick is tracked by mediaPickTarget, since
    // opening a modal is async — resolveAudioSourcePick/
    // resolveVideoSourcePick can't just return the answer synchronously
    // the way the old native-dialog call did.
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
            else if (root.mediaPickTarget === "editVideo") root.editVideoSublabel = path
            else if (root.mediaPickTarget === "addSourceVideo") root.addSourceSublabel = path
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
                else if (root.mediaPickTarget === "editVideo") root.editVideoSublabel = file
                else if (root.mediaPickTarget === "addSourceVideo") root.addSourceSublabel = file
            }
            root.mediaPickTarget = ""
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
    property real editAudioLevel: 75
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

    // Keep the dialog's seed array in step with the model after this dialog
    // writes a channel's gain (a dragged knob, or the channel's on/off). The
    // knob reads gainFor() from here on re-render, and a stale value would
    // show a trim the model does not have.
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
        // Real metering for a device row — the tap's live channel layout
        // (mono/stereo/multi) is what re-renders the channel rows below, and
        // the strips' levels come from the same snapshot.
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
    property real addSourceLevel: 75
    property bool addSourceMuted: false
    // Pro-audio form state — Delay / Channels, written through the setters
    // after addInput() like every other collected value.
    property int addSourceDelayMs: 0
    property int addSourceChannels: 2
    property var addSourceGains: []   // buffered per-channel faders (unity default)
    // The trim each buffered side was closed at. The Add dialog's row does not
    // exist yet, so the BUFFER is its store — the same policy the models apply
    // (models/ChannelTrim.h): a side closed at -6 dB in this dialog reopens at
    // -6 dB in it, and the trim is handed to the row on submit.
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
    // AUDIO's own Media-kind source list — a single action that opens the
    // shared Select Media grid (settingsMediaPicker, fixedKind: "audio"),
    // not a flat list of every track's real filename inline in the
    // dropdown (that's what the grid itself is for) and not the old
    // placeholder labels either (those were never wired to anything —
    // picking one just stored its literal caption as the sublabel).
    readonly property var audioMediaSourceOptions: [
        { label: qsTr("Select Playlist…"), value: "__browse_audio_file__" }
    ]
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
    function resolveAudioSourcePick(picked, previous, target) {
        if (picked !== "__browse_audio_file__")
            return picked
        // Async — opens the shared Select Media grid; its applied/cancelled
        // handler (see settingsMediaPicker) writes the real value later.
        root.mediaPickTarget = target
        return previous
    }
    // VIDEO's own Media-kind source list — same single-action treatment as
    // audioMediaSourceOptions above (opens settingsMediaPicker, fixedKind:
    // "video", instead of listing every file inline).
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
        // Device rosters are populated at boot and refreshed on hot-plug.
        // Picker refreshes are asynchronous, so newly connected devices appear
        // without blocking this dialog or the event loop.
        if (type === "audio") EngineBridge.refreshAudioDevices()
        else EngineBridge.refreshDevices()
        // The dialog is open now: its device (the DEFAULT input until a Source
        // pick re-targets it) must be metering, or the form's channel strips
        // sit dead — the exact "Edit Audio Input meters never move" report.
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
            // The trims the buffered sides were closed at (setChannelGain above
            // only records the sides that are OPEN), so a side closed in the Add
            // dialog comes back at its trim on the new row too.
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
        root.editVideoName = data.name
        root.editVideoSublabelByKind = ({})   // fresh buffer per row
        root.editVideoKind = data.kind
        root.editVideoSublabel = data.sublabel
        root.editVideoMode = data.mode !== undefined ? data.mode : ""
        root.editVideoMuted = data.muted
        root.editVideoLevel = data.level
        // Ask for a current roster; Media Foundation probing runs off-thread.
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
    // that and rewired the wrong sources — so the disconnect below is by the
    // row's own id through the model, never by shifting row numbers.) Both the
    // context menu's Delete and the Edit dialogs' Delete buttons go through
    // these.
    // THE LINES GO WITH THE CARD, and they go the way the line's OWN click
    // takes them off: every bus this row is routed to is disconnected FIRST,
    // while the row still exists (BusListModel.disconnectAudioRoute — the
    // remove-only half of the very toggle a click on a line uses). It has to
    // happen before the removal: a route is keyed on the row's STABLE id, a row
    // that is already gone has no id, and the same call made afterwards is a
    // silent no-op — which is exactly how a curved line used to outlive the
    // card it was drawn for, with its own click unable to remove it any more
    // ("delete the card first and the line refuses to be deleted"), until a
    // restart rebuilt the roster. The model's precise cut and the ghost sweep
    // below stay as the guarantee for edges this walk cannot see (a route whose
    // cached row list was never refreshed); the walk is what makes the reported
    // case deterministic.
    function removeAudioFixingRoutes(index) {
        for (let b = BusListModel.rowCount() - 1; b >= 0; --b)
            if (BusListModel.getBus(b).routedAudioInputs.indexOf(index) >= 0)
                BusListModel.disconnectAudioRoute(b, index)
        AudioInputListModel.removeInput(index)
        // ...and then the guarantee, on top of the model's own precise cut: any
        // "asrc:<id>" node the roster no longer lists is a leftover, and a
        // leftover edge is exactly a line that outlives the card it was drawn
        // for ("the lines should remove with source card"). Cheap no-op when
        // the cut above was complete.
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
            BusListModel.removeBus(root.deleteTargetIndex)
        root.deleteTargetIndex = -1
    }

    // ---- Drag-to-connect ----
    // Press a source card's port dot, drag onto a bus card, release: the
    // route is added (never removed — right-click a line to remove, or use
    // the bus's Edit & Routing dialog). Video sources can fan out to buses;
    // each bus still accepts one video provider.
    property bool dragConnectActive: false
    property string dragConnectFrom: ""   // "audio" | "video"
    property int dragConnectIndex: -1
    property point dragConnectStart
    property point dragConnectPos

    // Ports report SCENE coordinates (PortDot maps from the MouseArea that took
    // the event, not from the dot) — this is the one place that turns one into
    // board space, the space busIndexAt/sourceIndexAt measure in.
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
    // Bus-initiated drag: the SAME gesture, reversed — press a bus's own
    // port and drag out to a source card instead of only source-to-bus.
    // Shares dragConnectActive/dragConnectPos/dragConnectStart (the drawn
    // line reads those regardless of which end started the drag);
    // dragConnectFromBus (-1 = not a bus-originated drag) is the only new
    // piece of state, keeping dropAllowed/disconnectRoute's kind+srcIndex+
    // busIndex contract identical either direction.
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
        if (s < 0)
            return
        const refusal = BusListModel.connectRefusal(kindName, s, busIndex)
        if (refusal !== "") {
            // The card under the pointer is the SOURCE here (the bus is where the
            // drag started), so that is where the refusal has to land — and the
            // toast carries the very code that turned it red.
            root.refuseDrop(kindName, s, busIndex, true, refusal)
            return
        }
        // Add-only and deferred, exactly like the source->bus drop: it runs
        // inside the port's release dispatch, and a synchronous route write
        // rebuilds the rows under the mouse. The flash reports what the model
        // answered, not what we hoped — and it lands on the SOURCE card, the end
        // the drop was aimed at.
        const rootRef = root
        Qt.callLater(() => {
            rootRef.flashSource(kindName, s, rootRef.addRoute(kindName, busIndex, s))
        })
    }
    // THE board's connect call — add-only and idempotent, so a drop can never
    // remove a route (a line's right-click, the bus dialog's toggle list and the
    // Edit input dialog's Routing matrix are where routes come off). The drop
    // used to call `disconnectRoute`, a TOGGLE: a connection the board's cached
    // list didn't know about (a restored board, a row shift) was in the graph
    // already, so the drag that was meant to add it deleted it instead.
    function addRoute(kindName, busIndex, srcIndex) {
        return kindName === "audio"
                ? BusListModel.connectAudioRoute(busIndex, srcIndex)
                : BusListModel.connectVideoRoute(busIndex, srcIndex)
    }
    // Which source card (if any) a board-space point lands on, for the kind
    // being dragged — the inverse of busIndexAt, same geometry contract
    // (the column's own x-range, row rhythm, row count).
    //
    // The band a row answers to is its card PLUS half the gap below it: a
    // release the eye reads as "on the card" must not be refused because the
    // pointer sat on the card's last few pixels, and the half-gap is the
    // midpoint rule between two cards (dropping between two rows belongs to the
    // one the pointer is nearer). It used to be the bare 64px card, which left
    // a dead strip between every pair of cards.
    // WHICH CARD a point is really over — the actual delegate, mapped through
    // Qt, not the row the layout constants predict.
    //
    // The constants describe the steady-state board, and a card that is laid out
    // even slightly outside that rhythm (a row added in this session, before a
    // restart puts everything back on the grid) had its drop refused, or read as
    // the neighbouring row — "the moment the app restarts the connectivity will
    // work, but if I just create it, it won't connect". Mapping through the
    // delegate itself removes the guess: the row IS the card under the pointer.
    // The small margin covers a card's own port dot, which pokes past its edge.
    function cardIndexAt(repeater, rowCount, p) {
        if (!repeater)
            return -1
        for (let i = 0; i < repeater.count; ++i) {
            const card = repeater.itemAt(i)
            if (!card || !card.visible)
                continue
            // A delegate can outlive its model row for a frame or two, and a
            // Repeater's count is not the model's count. `index` is the only
            // identity that is never stale, and an index past the roster is not
            // a card — it is the row number of a row that is gone, which every
            // policy call below reads as "no such source".
            if (i >= rowCount || card.index !== i)
                continue
            // p is BOARD space (everything the gesture measures is, see
            // dragPoint), so the card's rect has to be mapped back through the
            // BOARD. This used to map through the scene (mapFromItem(null,...))
            // while being handed board coordinates — the same thing only while
            // the board sits at the window origin. It does not in the Settings
            // modal, so the hit test answered with the wrong card (or none at
            // all) and a drop landed on a row the pointer was nowhere near.
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
        // The band covers the card AND its own port dot (the port pokes 4px
        // past the edge and its grab area reaches ~14px beyond), because that
        // dot is where the drag line ends and therefore where the pointer is
        // when the button comes up. A release 1px outside the card used to be
        // refused in complete silence.
        if (p.x < colX - 20 || p.x > colX + colW + 20)
            return -1
        const relY = p.y - board.headerH
        if (relY < 0)
            return -1
        const step = rowH + board.rowGap
        // NEAREST row, not "the row whose card is under the pointer": between
        // two cards, or on the gap below the last one, the drop belongs to the
        // row it is nearest to. A drop inside the column is never refused for
        // being a few pixels off a card — that is what made the gesture feel
        // like it "doesn't connect easily".
        return Math.max(0, Math.min(count - 1, Math.floor(relY / step)))
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
        // Deferred like the connector's right-click: this runs inside the
        // port MouseArea's release dispatch, and a synchronous route write
        // rebuilds the rows, destroying the very delegate dispatching the
        // event. Capture the plain values, mutate after dispatch settles.
        // Already-connected is not a special case any more (addRoute answers
        // true for it): the old early return existed because the call was a
        // toggle that would have REMOVED the route.
        const rootRef = root
        Qt.callLater(() => {
            rootRef.flashBus(b, rootRef.addRoute(kindName, b, srcIndex))
        })
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
        onTriggered: {
            root.flashBusIndex = -1
            root.flashSourceIndex = -1
        }
    }

    // The same answer on the SOURCE side of a gesture. A source->bus drag lands
    // on a bus card and flashes THAT; a bus->source drag lands on a source card,
    // and until this existed the card under the pointer stayed inert whether the
    // drop was accepted or refused (the report: "when i drag the line from the
    // bus to the source it doesn't red that it's not allowed").
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

    // The WORDING of one refusal code (the decision lives in the model). Split
    // out so a drop can hand over the very code it acted on instead of asking
    // the model a second time: the card's colour and the sentence the operator
    // reads then come from one answer, never two that could disagree.
    function dropRefusalTextFor(code, kindName, srcIndex, busIndex) {
        const busName = (BusListModel.getBus(busIndex) || {}).name ?? ""
        const src = kindName === "audio" ? (AudioInputListModel.getInput(srcIndex) || {})
                                         : (VideoSourceListModel.getSource(srcIndex) || {})
        const srcName = src.name ?? ""
        switch (code) {
        case "bus-has-video":
            return qsTr("\u201C%1\u201D already carries a video source. Remove that route first \u2014 a bus shows one video source at a time.").arg(busName)
        case "already-connected":
            return qsTr("\u201C%1\u201D is already routed into \u201C%2\u201D.").arg(srcName).arg(busName)
        case "audio-only-bus":
            return qsTr("\u201C%1\u201D is an audio-only bus \u2014 it has no video to route into.").arg(busName)
        case "engine-refused":
            return qsTr("The engine refused that route into \u201C%1\u201D (signal type or a loop).").arg(busName)
        case "no-source-row":
            // Not a generic shrug any more: this is the one refusal the operator
            // cannot act on, and it used to be reported as "that source can no
            // longer be routed" for a row that was on the board all along.
            return qsTr("The board moved under the drag \u2014 that row is not where the drop landed. Drag again.")
        default:
            return qsTr("That route was refused (%1).").arg(code === "" ? qsTr("no reason reported") : code)
        }
    }
    // A refused drop, told on both surfaces the gesture has: the card's red pulse
    // and a toast naming the reason. `code` is optional — a caller that already
    // holds the refusal passes it, so the toast cannot disagree with the red.
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
    // Which bus card (if any) a board-space point lands on — geometry from
    // the board's own layout constants, so it can't drift from the cards.
    // Buses stack with the standard rowH rhythm (busPortY).
    function busIndexAt(p) {
        const count = BusListModel.rowCount()
        const hit = root.cardIndexAt(busCards, count, p)
        if (hit >= 0)
            return hit
        if (count === 0)
            return -1
        // Same band and the same nearest-row rule as sourceIndexAt: the bus's
        // own port dot sits just outside the card's left edge and is where the
        // drag line ends, so a release there (or a couple of pixels below a
        // card) must still land on that bus rather than being refused.
        if (p.x < board.busX - 20 || p.x > board.busX + board.busColW + 20)
            return -1
        const relY = p.y - board.headerH
        if (relY < 0)
            return -1
        const step = board.rowH + board.rowGap
        return Math.max(0, Math.min(count - 1, Math.floor(relY / step)))
    }
    // Only meaningful for a SOURCE-originated drag (dragConnectFromBus < 0)
    // — a bus-originated drag hovers the SOURCE column instead, see
    // dragConnectHoverSource below.
    readonly property int dragConnectHoverBus:
        root.dragConnectActive && root.dragConnectFromBus < 0
            ? root.busIndexAt(root.dragConnectPos) : -1
    // The inverse, for a bus-originated drag.
    readonly property int dragConnectHoverSource:
        root.dragConnectActive && root.dragConnectFromBus >= 0
            ? root.sourceIndexAt(root.dragConnectFrom, root.dragConnectPos) : -1
    // The one compatibility rule, shared by drag feedback and drop handling:
    // audio fits every bus; video fits only video/both buses. A video source
    // may fan out to multiple buses, while a bus accepts one video provider.
    // BusListModel.canConnect is the policy answer used by the whole board.
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
                objectName: "selfTestAvBoard"

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
                // The room the three columns really have once their two gutters
                // are reserved (floored away from zero so a still-narrower pane
                // degrades instead of producing negative widths).
                readonly property real columnsRoom: Math.max(240, board.width - board.gutterW * 2)
                // What each column WANTS: proportional, with a readable minimum.
                // Video cards carry a preview thumb the other columns don't, so
                // they get their own slightly wider column — the extra room lets a
                // full-width name sit beside the thumb instead of eliding to
                // "Audience C…". Audio/bus keep colW; the -36 keeps the three
                // columns' combined width the same as before (video borrowed it).
                readonly property real wantColW: Math.max(200, columnsRoom * 0.30)
                readonly property real wantBusColW: Math.max(184, columnsRoom * 0.34 - 36)
                readonly property real wantVidColW: Math.max(200, columnsRoom * 0.30) + 36
                // ...and all three are scaled back TOGETHER when those minimums do
                // not fit the room. The minimums used to win outright, which made
                // the board WIDER than the Flickable holding it on a narrow
                // window: at 820x560 (columns room 454) the columns ran to 684
                // inside a 518px board, so 166px of the right-hand column — the
                // video side of the buses and their routing — sat outside the pane.
                // This Flickable never scrolls horizontally, so that part was
                // simply unreachable: "resizing cuts off the video bus". At the
                // design size the scale is exactly 1, so nothing moves there.
                // Measured by the AVBOARD line of the modal-fit scenario (see
                // tools/selftest/run_modal_fit_test.sh).
                readonly property real colScale: Math.min(1, columnsRoom / (wantColW + wantBusColW + wantVidColW))
                readonly property real colW: wantColW * colScale
                readonly property real busColW: wantBusColW * colScale
                readonly property real vidColW: wantVidColW * colScale
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
                    const audioRows = AudioInputListModel.rowCount()
                    const videoRows = VideoSourceListModel.rowCount()
                    for (let b = 0; b < busCount; ++b) {
                        const bus = BusListModel.getBus(b)
                        const by = board.busPortY(b)
                        for (const ai of bus.routedAudioInputs) {
                            // A route naming a row that is not there draws a line
                            // to NOBODY — or, worse, to whichever card slid up
                            // into that number. Nothing can be hit-tested or
                            // clicked off in that state, so it is not drawn at
                            // all; the model's cached lists are re-derived on
                            // every roster change, and this is the guard that
                            // makes a stale one harmless on screen.
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
                            objectName: "selfTestAvAddAudio"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
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

                            // This row holds its device only while it is really
                            // on screen (see rowsInView): scrolling the board
                            // past a configured input gives its capture back.
                            InViewProbe {
                                id: inRowProbe
                                target: inRow
                                // The handler belongs ON the probe: a bare
                                // `onInViewChanged` beside it would be read as a
                                // signal of the ROW (there is none) — the
                                // "Cannot assign to non-existent property" the
                                // build caught.
                                onInViewChanged: root.setRowInView("audio", inRow.index, inRowProbe.inView)
                            }

                            width: board.colW
                            height: board.rowH
                            radius: Theme.radiusMd
                            // Tinted while it is the TARGET of a bus-originated
                            // drag, then pulsed with the drop's own answer — the
                            // same dressing the bus card wears in the other
                            // direction, so both ends of one gesture read alike
                            // ("if i drag from the bus to the source the source
                            // should red").
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

                            // LIVE board meter — the row's own device tap (held by
                            // audioMeterTargets while the row is on the board),
                            // gain/mute-aware through inputChannelLevels. Media/NDI
                            // rows have no WASAPI device to tap: no tap, no fake
                            // signal, nothing to animate from.
                            LevelTrack {
                                x: 28; y: 48
                                width: board.colW - 90
                                value: {
                                    // A row with NO source chosen meters nothing: there is
                                    // no device to tap, so the bar rests at zero.
                                    // This used to fall back to the row's stored VOLUME,
                                    // so a freshly added input (Source field still empty,
                                    // default level 75) drew a 75% bar on its card — a
                                    // brand-new, silent row reading as a loud live source.
                                    // A meter reads the signal; a fader is not a signal.
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

                            // Port dot feeding a connector line toward the
                            // bus column, poking past the card's own edge.
                            // PRESS-DRAG from it onto a bus card to connect.
                            // Grey at rest, lights while hovered/dragging —
                            // the affordance, never the status.
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
                            objectName: "selfTestAvAddBus"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
                            variant: "ghost"
                            horizontalPadding: Theme.space3
                            onClicked: root.openAddBus()
                        }
                    }

                    // The manual "Engine audio on/off" toggle that used to sit
                    // here was removed by request (a better-integrated trigger
                    // is planned), and the engine-audio path behind it
                    // (EngineBridge.setAudioRender()/audioRenderActive) has now
                    // been removed too (2026-10): unreachable code that, on a
                    // meter tick, silently opened a WASAPI capture session on
                    // every routed input. A real playout path has to be built
                    // on purpose — see the removal note in EngineBridge.cpp.

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

                            // LIVE BUS METER — sum routed live input levels
                            // after their source faders/channel gains, then
                            // apply this bus's fader and mute.
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
                                // Continuous level gradient, same as every
                                // other meter now — buses used to tint by
                                // TYPE (a flat categorical color) instead of
                                // showing real level.
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
                            // audio bus hides the video port. Both are ALSO
                            // drag sources now (press-drag out to a source
                            // card) — the reverse of the source-to-bus drag,
                            // sharing the same dropAllowed/disconnectRoute
                            // contract via startDragConnectFromBus.
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
                            objectName: "selfTestAvAddVideo"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("+ Add")
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

                            // Mute exists on EVERY video row: media mute
                            // silences its audio, camera/screen mute blacks
                            // the feed out at the bus it feeds.
                            readonly property bool hasAudio: vidRow.kind === "media"
                                                             || vidRow.kind === "ndi"
                            // The row's live thumbnail source (nonce-pumped
                            // by the thumb itself — see vidThumb). Cameras
                            // AND screens: the monitor label resolves through
                            // the same videopreview provider. NDI: the SAME
                            // URL — the provider serves the engine receiver's
                            // converted frames for NDI taps (startNdiPreview).
                            readonly property url liveThumb:
                                vidThumb.liveCam
                                ? "image://videopreview/" + encodeURIComponent(vidRow.sublabel)
                                  + "?n=" + vidThumb.thumbNonce : ""

                            width: board.vidColW
                            height: board.vidRowH
                            radius: Theme.radiusMd
                            // Same dressing as the audio card: tinted while it is
                            // the target of a bus-originated drag, pulsed with
                            // the drop's answer, red when the move will not be
                            // allowed (a bus that already carries a video
                            // source, or the same source twice).
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
                                objectName: "selfTestVideoRowMeter_" + vidRow.index
                                x: 28
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 14
                                visible: vidRow.hasAudio
                                // Stop short of the Edit link's hover area.
                                width: vidThumb.x - 28 - 44
                                // The row's REAL audio — the bridge's stream tap
                                // on the file/stream it names, read through the
                                // same helper the Edit dialog's channel strips
                                // bind (see videoMixLevel). The stored volume
                                // used to be drawn here, which made this a
                                // frozen "level" that disagreed with the dialog
                                // about the very same row.
                                value: {
                                    void root.audioMeterList
                                    return root.videoMixLevel(vidRow.index, vidRow.sublabel,
                                                              vidRow.kind, vidRow.muted,
                                                              vidRow.level)
                                }
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
                                // The lens/monitor pane is what SHOWS this row's
                                // camera, so the tap follows it: on screen → the
                                // row's feed may run, scrolled out of the board →
                                // the camera light goes off (liveCam below).
                                InViewProbe {
                                    id: vidProbe
                                    target: vidThumb
                                    // Handler ON the probe (see the audio row's
                                    // twin): syncTap re-decides the row's own
                                    // tap, which is what makes the camera open
                                    // and close with this pane's visibility.
                                    onInViewChanged: {
                                        root.setRowInView("video", vidRow.index, vidProbe.inView)
                                        vidThumb.syncTap()
                                    }
                                }

                                // LIVE FRAME for camera rows — the tap's newest
                                // JPEG, nonce-pumped slower than the dialogs
                                // (a thumbnail doesn't need 15 fps; 8 reads
                                // smooth).
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
                                // The row's tap + pump, reconciled like the
                                // dialog pane (VideoPreviewPane.syncTap) and
                                // sharing its KEEP-ALIVE pool discipline: a
                                // device change STARTS the new tap and leaves
                                // the old one running — no per-switch stop
                                // (repeated close/re-open exhausts virtual-cam
                                // drivers; the screen-wide stopAllVideoPreviews
                                // at teardown is the only release). NDI joins
                                // the same discipline: the ENGINE receiver
                                // (startNdiPreview/stopNdiPreview) is started
                                // for the row's source and released at teardown.
                                readonly property bool liveCam:
                                    (vidRow.kind === "camera" || vidRow.kind === "screen"
                                     || vidRow.kind === "ndi")
                                    && vidRow.sublabel !== ""
                                    // NDI's MUTE pill is AUDIO-only (the feed
                                    // carries embedded audio; muting must not
                                    // tear the receiver down — camera/screen
                                    // mute IS the feed pause, NDI's isn't).
                                    && (vidRow.kind === "ndi" || !vidRow.muted)
                                    // ON SCREEN ONLY: a camera selected on a row
                                    // the operator has scrolled past is not being
                                    // used, so it is not open (the report this
                                    // exists for: "dont expect to see my camera
                                    // open until it been used even though it has
                                    // been selected").
                                    && vidProbe.inView
                                // Frames actually flowing — the warm-up test that
                                // retires the decorative glyphs (same rule as the
                                // dialogs' pane): a live thumb must not wear the
                                // lens/monitor art on top of real pixels.
                                readonly property bool thumbLive:
                                    frameImg.status === Image.Ready && frameImg.sourceSize.width > 1
                                // WHY this row's tap can never show a frame (""
                                // while it is fine — which includes warming up).
                                // The warm-up glyphs above retire on frames ALONE,
                                // so a tap whose device never opened kept its lens/
                                // monitor art for ever with nothing anywhere to say
                                // why (see IVideo::PreviewFailure). Read through
                                // thumbNonce: EngineBridge's answer is not a
                                // property, so the pump is what re-asks it.
                                readonly property string tapFailure: {
                                    void vidThumb.thumbNonce
                                    if (!vidThumb.liveCam || vidThumb.thumbLive)
                                        return ""
                                    return EngineBridge.previewFailure(vidRow.sublabel)
                                }
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
                                        else if (oldKind === "ndi")
                                            EngineBridge.stopNdiPreview(old, "board")
                                        else
                                            EngineBridge.stopVideoPreview(old, "board")
                                    }
                                    if (want !== "") {
                                        if (vidRow.kind === "screen")
                                            EngineBridge.startScreenPreview(want, "board")
                                        else if (vidRow.kind === "ndi")
                                            EngineBridge.startNdiPreview(want, "board")
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

                                // NDI — broadcast ripples (center dot +
                                // two rings) for network sources, warm-up
                                // only: they retire once real frames flow
                                // (same rule as the camera lens and screen
                                // outline — never painted over live pixels).
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

                                // Screen — a monitor outline + stand (warm-up
                                // only, same as the lens above). The wrapper
                                // needs REAL dimensions: children anchor to it,
                                // and an implicit 0x0 Item stacks them at the
                                // center point.
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

                                // TAP FAILED — the ring/glyphs above are for a tap
                                // that is WARMING UP; this one is dead (activation
                                // refused, device busy, the window's device context
                                // gone), so instead of pulsing for ever the card
                                // names the reason the platform gave.
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
                // Drawn last (on top of the cards) while a drag is live,
                // from the source port to the cursor. Green when hovering a
                // compatible bus, red over an incompatible one.
                Shape {
                    visible: root.dragConnectActive
                    anchors.fill: parent
                    z: 10
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        // Green over a bus that will take it, red over one that
                        // will not — in EITHER direction. A bus-originated drag
                        // hovers a source card, not a bus, so asking only about
                        // dragConnectHoverBus left the line grey through a
                        // refusal ("it doesn't red that it's not allowed").
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
    // ONE declaration of this row for every dialog in the screen (the two Edit
    // dialogs and the Add dialog's two tabs). It used to be hand-built per
    // dialog, and the copies had drifted apart: the audio dialogs put the label
    // on ProAudioForm's 76px rail in textSm, while the video ones stacked a
    // textXs label above an unindented chip row — two designs for the same
    // control (live report: "the app has two designs for the audio and video
    // edit popup — make it only one").
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

        // Same type as ProAudioForm's own rail label (that one is a component
        // local to its file, so this is the same face, declared here) — the
        // Kind label must sit on the form's rail exactly like Name/Source do.
        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: kindRow.railLabel
            color: "#8a94a6"
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
        // Cancel must release the dialog's tap too — saveEditAudio() already
        // did, but Cancel skipped it, so backing out of an edit (instead of
        // saving) left the device metering forever (nothing ever stopped
        // it short of tearing the whole screen down): reported live as
        // "just opening Settings > Audio & Video, the source in Media is
        // already active" — a tap orphaned by an earlier cancelled edit,
        // still running from a previous visit to this screen.
        onCancelled: {
            root.editAudioIndex = -1
            root.syncAudioMeters()
        }
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
                // label and options below follow this selection. Same shared
                // component the video and Add dialogs use (see KindRailRow).
                KindRailRow {
                    width: parent.width
                    current: root.editAudioKind
                    choices: root.audioKinds
                    onPicked: (key) => {
                        // Remember this kind's pick before leaving it, restore
                        // the new kind's last pick if it had one — a bare reset
                        // here permanently blanked Device's pick the moment you
                        // so much as glanced at Media and came back.
                        const buf = Object.assign({}, root.editAudioSublabelByKind)
                        buf[root.editAudioKind] = root.editAudioSublabel
                        root.editAudioSublabelByKind = buf
                        root.editAudioKind = key
                        root.editAudioSublabel = buf[key] !== undefined ? buf[key] : ""
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
                    // What a side switched back on comes back at: the MODEL's
                    // remembered trim, so a side closed from the Media pane's
                    // L/R pill reopens here at the same level (and vice versa).
                    channelTrimFor: function(i) {
                        return AudioInputListModel.channelTrim(root.editAudioIndex, i)
                    }
                    onChannelGainEdited: (i, g) => {
                        AudioInputListModel.setChannelGain(root.editAudioIndex, i, g)
                        root.rememberEditAudioGain(i, g)
                    }
                    // The channel checkbox reads the MODEL (gain 0 ≡ off), so a
                    // channel switched off from the Media pane's L/R pill shows
                    // as off here too.
                    channelOnFor: function(i) {
                        void root.modelsRev
                        return !Db.isChannelOff(AudioInputListModel.channelGain(root.editAudioIndex, i))
                    }
                    // ...and switching one off/on WRITES it, which is the half
                    // that was missing: the checkbox used to grey the row out
                    // locally and stop there, so the left channel "turned off"
                    // in this dialog kept its full level on the board, in the
                    // Media pane, on the monitor tile and in the engine graph.
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
                // Deferred model write: the write rebuilds this rack (the
                // panel is bound to the model through modelsRev), and a
                // synchronous rebuild inside the chip's own click event
                // destroys the dispatching delegate mid-event. Same hazard
                // as the routing board's line hit-testing — never mutate
                // the model that rebuilds your own delegate from within its
                // event. (Deferring here is the other half of the panel's
                // emit-now contract; see AudioEffectsPanel.qml.)
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
                    // Routes-by-index must be compacted before the row goes
                    // (same path as the context menu's Delete). Same
                    // release-the-tap fix as Cancel above — deleting the row
                    // out from under a running meter tap orphaned it too.
                    root.removeAudioFixingRoutes(root.editAudioIndex)
                    root.editAudioIndex = -1
                    root.syncAudioMeters()   // release the deleted row's hold
                }
            }
        }
    }

    // ---- Edit Video Source dialog ----
    // THE SAME DESIGN AS Edit Audio Input: Kind on the form's label rail, then
    // ProAudioForm carrying Name and Source on that same rail, then the ONE
    // thing video has and audio does not — the preview pane — and, for the
    // kinds that carry audio (Media/NDI), the very same Volume/Channels
    // sections the audio dialog shows. Delete sits where the audio dialog's
    // sits. The gate is videoKindHasAudio: a camera or a screen has no audio to
    // trim, so those two sections are hidden rather than shown empty.
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
            choices: root.videoKinds
            onPicked: (key) => {
                // Remember this kind's pick, restore the new kind's last one —
                // see editAudioSublabelByKind.
                const buf = Object.assign({}, root.editVideoSublabelByKind)
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
            // The row's REAL audio — the bridge's stream tap on the media file
            // or NDI stream this row names (see startStreamMeter), read through
            // the same snapshot lookup a microphone's strip uses. These two bars
            // used to mirror the stored Volume ("no live audio TAP exists for a
            // video-tab row"), which read as an always-partly-filled meter that
            // tracked the fader instead of the sound — the "media animation is
            // not correct" report. A row whose source has no tap yet (a file
            // that isn't there, an NDI stream with no receiver) now reads
            // silence, which is its truth.
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
            // A media or NDI row (the kinds that carry audio — the gate above)
            // used to show knobs that MOVED NOTHING: with no gainFor, no
            // channelOnFor and no channel callbacks, the form fell back to its
            // defaults (every channel at unity, every channel on) and a dragged
            // knob wrote to nothing, so the row could not be trimmed or closed
            // here at all. The video CARD's pills were wired all along, which
            // made the same row settable in the Media pane and inert in its own
            // dialog.
            //
            // Read-through, with modelsRev as the reactivity bridge — the same
            // pattern the card's pills use, and one source of truth instead of a
            // second snapshot to keep in step (rowCount/getSource/channelGain
            // are plain Q_INVOKABLE reads, so nothing recomputes without it).
            // Like the audio dialog's channels, a knob writes the model as it is
            // dragged (the pills' contract: the trim IS the row's state now), not
            // on Save.
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
            // ...and toggling WRITES it, exactly as the card's pill does: closing
            // stores gain 0, and reopening hands back the trim the side was
            // closed with — the form computed that `gain` through the shared
            // reopen policy, so it is never unity by accident.
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
        // SHARED VideoPreviewPane component: 16:9 pane, kind glyphs,
        // MUTE/PAUSED pill, Resolution mode pill and the kind hint live there
        // ONCE; both video dialogs bind their own state onto it.
        VideoPreviewPane {
            width: parent.width
            kind: root.editVideoKind
            // Camera/screen/NDI go live (a real tap); media stays decorative.
            previewLabel: root.editVideoKind === "camera" || root.editVideoKind === "screen"
                          || root.editVideoKind === "ndi"
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
                // Explicit reactivity dependencies — busMixLevel's own live
                // level read happens inside nested function calls
                // (inputMixLevel → meterSnapshotFor → audioMeterList), and
                // a plain-function call chain like that has not reliably
                // propagated as a binding dependency elsewhere in this
                // file either (see every other "void root.x" in it) —
                // these rows sat permanently silent without this.
                void root.audioMeterList
                void root.modelsRev
                return root.editBusIndex >= 0
                    ? root.busChannelLevels(root.editBusIndex) : [0, 0]
            }
            meterLayout: "stereo"
            // Same channel-row rendering as an input row, reading the bus's own
            // incoming state: a side closed on every routed source shows here
            // as a closed row (the read-through in busChannelOpen). Nothing is
            // writable — see channelsEditable in ProAudioForm: the engine has no
            // per-channel bus trim to write.
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
            id: busVideoList
            width: parent.width
            spacing: Theme.space2
            visible: !root.busDialogIsAdd && root.editBusType !== "audio"
            // ONE VIDEO SOURCE PER BUS (the board's rule): at most one of these
            // rows can be on. The routed one stays switchable — unticking it is
            // how a route comes OFF — and every other row is inert and dimmed
            // while it is on, with the line below saying why. Before this, the
            // model refused the second one and the toggle simply sprang back.
            readonly property bool videoSourceRouted: root.busRoutedVideo().length > 0
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
                    readonly property bool routedHere: root.busRoutedVideo().indexOf(index) >= 0
                    readonly property bool takenByOther: !routedHere && busVideoList.videoSourceRouted
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
                        enabled: !takenByOther
                        opacity: takenByOther ? 0.4 : 1
                        checked: routedHere
                        onToggled: BusListModel.toggleVideoRoute(root.editBusIndex, index)
                    }
                }
            }
            Text {
                width: parent.width
                visible: busVideoList.videoSourceRouted
                text: qsTr("A bus shows one video source at a time \u2014 untick the routed one before choosing another.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
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
        objectName: "selfTestAddSourceDialog"
        shown: root.addSourceShown
        title: qsTr("Add Source")
        subtitle: qsTr("Add an audio input or video source, set its level, and dial in its effects rack.")
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
                // label and options below follow this selection. The SHARED
                // KindRailRow component, the same one the two Edit dialogs
                // and this dialog's video tab use — this was the last
                // hand-built copy of the row, and its label face had drifted
                // from the shared one (see KindRailRow's own comment).
                KindRailRow {
                    width: parent.width
                    current: root.addSourceKind
                    choices: root.audioKinds
                    onPicked: (key) => {
                        // Remember this kind's pick, restore the new
                        // kind's last one — see editAudioSublabelByKind.
                        const buf = Object.assign({}, root.addSourceSublabelByKind)
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
                        // An OPEN level is this side's trim (a close is not): the
                        // same rule the models' setChannelGain applies.
                        if (!Db.isChannelOff(g))
                            root.rememberAddSourceTrim(i, g)
                    }
                    // The buffered trim, in the same shape the models answer
                    // channelTrim() with — so the Add dialog and a saved row
                    // reopen a side identically.
                    channelTrimFor: function(i) {
                        return root.addSourceTrims.length > i ? root.addSourceTrims[i] : 0.0
                    }
                    // Add dialog: the row does not exist yet, so the channel's
                    // on/off state is the buffered gain (0 ≡ off) that
                    // submitAddSource writes to the new row — same convention as
                    // the Edit dialog and the Media pane's pills.
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
                        // ProAudioForm computed `gain` through the same reopen
                        // policy (its trimFor), so an opening side's gain IS the
                        // trim to keep.
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

            // THE SAME SHAPE AS Edit Video Source: Kind on the form's label
            // rail, then ProAudioForm carrying Name and Source on that rail,
            // then the preview. This tab used to be a stacked-label
            // SettingsField/SelectField pair — the one place the video fields
            // still had a second design.
            KindRailRow {
                width: parent.width
                current: root.addSourceKind
                choices: root.videoKinds
                onPicked: (key) => {
                    // Remember this kind's pick, restore the new
                    // kind's last one — see editAudioSublabelByKind.
                    const buf = Object.assign({}, root.addSourceSublabelByKind)
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

            // Name / Source on the shared 76px rail, then Volume/Channels for
            // the kinds that carry audio — the identical form the Edit Video
            // dialog uses. Source is REAL hardware for the camera kind (the
            // engine's Media Foundation roster), screen/media option sets
            // otherwise; the field keeps its test hook so the modal-fit sweep
            // can still click it and measure the popup that opens.
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
                // The picked source's REAL audio while the dialog is open (its
                // own stream tap is held through videoMeterTargets) — never the
                // stored fader mirrored as a level.
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
            // same instance the Edit Video dialog uses): pane, glyphs,
            // MUTE/PAUSED pill, mode pill live there once. No hint here —
            // the Add column is tighter.
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
                // Same deferred write as the Edit dialog — the reassignment
                // rebuilds the rack, and the dispatching chip must survive
                // its own click event. (Reassigning the whole var array is
                // also what notifies QML's var-property change detection;
                // plain in-place mutation wouldn't.)
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
