import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../components"

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

    // ---- Edit Audio Input dialog ----
    property int editAudioIndex: -1
    property string editAudioName: ""
    property string editAudioKind: "device"
    property string editAudioSublabel: ""
    property real editAudioLevel: 75
    property bool editAudioMuted: false
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
        root.editAudioLevel = data.level
        root.editAudioMuted = data.muted
        root.editAudioSelectedEffect = ""
        // SettingsToggle self-flips its `checked` on click, which breaks any
        // consumer binding after the first use — re-sync it per open so a
        // previously-clicked toggle can't show a stale state for this row.
        editAudioMuteToggle.checked = root.editAudioMuted
    }
    function saveEditAudio() {
        if (root.editAudioIndex < 0)
            return
        AudioInputListModel.renameInput(root.editAudioIndex, root.editAudioName)
        AudioInputListModel.setKind(root.editAudioIndex, root.editAudioKind)
        AudioInputListModel.setSublabel(root.editAudioIndex, root.editAudioSublabel)
        AudioInputListModel.setLevel(root.editAudioIndex, root.editAudioLevel)
        AudioInputListModel.setMuted(root.editAudioIndex, root.editAudioMuted)
        root.editAudioIndex = -1
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
    property real addSourceLevel: 75
    property bool addSourceMuted: false
    property var addSourceEffects: []
    property string addSourceSelectedEffect: ""

    // Shared kind taxonomies — the same lists the Edit dialogs render, so
    // they can't drift apart. Audio: a real device (mic/line/system — the
    // specific hardware is the row's own identity, not its kind) or media.
    readonly property var audioKinds: [
        { key: "device", label: qsTr("Device") },
        { key: "media", label: qsTr("Media") }
    ]

    // Device/media pick lists for the audio dialogs' second field — the
    // field's LABEL follows the kind ("Device" vs "Media") and so do its
    // options. Device rows name real hardware; media rows name a content
    // source. (Mock rosters until real QAudioDevice enumeration lands.)
    readonly property var deviceOptions: [
        { label: qsTr("Built-in Microphone") },
        { label: qsTr("External USB Microphone") },
        { label: qsTr("Line In · Rear Panel") },
        { label: qsTr("Stereo Mix") },
        { label: qsTr("Desktop Audio") }
    ]
    readonly property var mediaSourceOptions: [
        { label: qsTr("Playlists & tracks") },
        { label: qsTr("Media File") },
        { label: qsTr("Stream Capture") }
    ]
    readonly property var videoKinds: [
        { key: "camera", label: qsTr("Camera") },
        { key: "screen", label: qsTr("Screen") },
        { key: "media", label: qsTr("Media") }
    ]

    function openAddSource(type) {
        root.addSourceType = type
        root.addSourceName = ""
        root.addSourceKind = type === "audio" ? "device" : "camera"
        root.addSourceSublabel = ""
        root.addSourceLevel = 75
        root.addSourceMuted = false
        // Seed the rack from the model's template (no row exists yet to read
        // from — defaultEffectsTemplate() is the pre-row snapshot).
        root.addSourceEffects = AudioInputListModel.defaultEffectsTemplate()
        root.addSourceSelectedEffect = ""
        addSourceMuteToggle.checked = root.addSourceMuted
        root.addSourceShown = true
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
            for (const e of root.addSourceEffects) {
                AudioInputListModel.setEffectEnabled(idx, e.key, e.enabled)
                AudioInputListModel.setEffectValue(idx, e.key, e.value)
            }
        } else {
            VideoSourceListModel.addSourceWith(root.addSourceName, root.addSourceKind,
                                               root.addSourceSublabel,
                                               root.addSourceKind === "media" ? root.addSourceLevel : 75,
                                               root.addSourceMuted)
        }
        root.addSourceShown = false
    }

    // ---- Edit Video Source dialog ----
    property int editVideoIndex: -1
    property string editVideoName: ""
    property string editVideoKind: "camera"
    property string editVideoSublabel: ""
    property bool editVideoMuted: false
    property real editVideoLevel: 75

    function openEditVideo(index) {
        const data = VideoSourceListModel.getSource(index)
        root.editVideoIndex = index
        root.editVideoName = data.name
        root.editVideoKind = data.kind
        root.editVideoSublabel = data.sublabel
        root.editVideoMuted = data.muted
        root.editVideoLevel = data.level
    }
    function saveEditVideo() {
        if (root.editVideoIndex < 0)
            return
        VideoSourceListModel.renameSource(root.editVideoIndex, root.editVideoName)
        VideoSourceListModel.setKind(root.editVideoIndex, root.editVideoKind)
        VideoSourceListModel.setSublabel(root.editVideoIndex, root.editVideoSublabel)
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
    // Removal must FIX ROUTING before it removes: buses store routes by row
    // index, so deleting row i without compacting would silently rewire
    // every route pointing at rows after i. Rewriting the route lists via
    // toggles (remove the dead index, shift the rest down) keeps the stored
    // indices in step with the model. Both the context menu's Delete and
    // the Edit dialogs' Delete buttons go through these.
    function removeAudioFixingRoutes(index) {
        for (let b = 0; b < BusListModel.rowCount(); ++b) {
            const before = BusListModel.getBus(b).routedAudioInputs
            const desired = before.filter((i) => i !== index).map((i) => i > index ? i - 1 : i)
            for (const i of before)
                if (desired.indexOf(i) < 0)
                    BusListModel.toggleAudioRoute(b, i)
            for (const i of desired)
                if (before.indexOf(i) < 0)
                    BusListModel.toggleAudioRoute(b, i)
        }
        AudioInputListModel.removeInput(index)
    }
    function removeVideoFixingRoutes(index) {
        for (let b = 0; b < BusListModel.rowCount(); ++b) {
            const before = BusListModel.getBus(b).routedVideoSources
            const desired = before.filter((i) => i !== index).map((i) => i > index ? i - 1 : i)
            for (const i of before)
                if (desired.indexOf(i) < 0)
                    BusListModel.toggleVideoRoute(b, i)
            for (const i of desired)
                if (before.indexOf(i) < 0)
                    BusListModel.toggleVideoRoute(b, i)
        }
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
                                font.pixelSize: 10
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

                            LevelTrack {
                                x: 28; y: 48
                                width: board.colW - 90
                                value: inRow.level
                                fillColor: Theme.success
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
                                font.pixelSize: 10
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

                            LevelTrack {
                                x: 12; y: 48
                                width: board.busColW - 90
                                value: busRow.level
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
                                font.pixelSize: 10
                                font.weight: Font.Bold
                            }
                            Text {
                                text: qsTr("cams · screen · media")
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
                            readonly property bool isMedia: vidRow.kind === "media"

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
                            // Its audio LEVEL meter stays media-only
                            // (camera/screen have no audio track to meter).
                            MutePill {
                                id: vidMutePill
                                anchors.right: vidThumb.left
                                anchors.top: parent.top
                                anchors.topMargin: 10
                                muted: vidRow.muted
                                // A camera/screen feed isn't "muted" when
                                // off, it's paused — only media carries audio.
                                offLabel: vidRow.isMedia ? qsTr("MUTE") : qsTr("PAUSED")
                                accent: Theme.info
                                accentLight: Theme.infoLight
                                onToggleRequested: VideoSourceListModel.setMuted(vidRow.index, !vidRow.muted)
                            }

                            LevelTrack {
                                x: 28
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 14
                                visible: vidRow.isMedia
                                // Stop short of the Edit link's hover area.
                                width: vidThumb.x - 28 - 44
                                value: vidRow.level
                                fillColor: Theme.info
                            }

                            // ---- Preview thumb — every video source has
                            // one. Decorative (no video engine in this app,
                            // same mock convention as the level meters): a
                            // dark pane with the kind's glyph.
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

                                // Camera — a lens ring + center dot.
                                Item {
                                    anchors.centerIn: parent
                                    visible: vidRow.kind === "camera"

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

                                // Screen — a monitor outline + stand. The
                                // wrapper needs REAL dimensions: children
                                // anchor to it, and an implicit 0x0 Item
                                // stacks them at the center point.
                                Item {
                                    anchors.centerIn: parent
                                    width: 22; height: 16
                                    visible: vidRow.kind === "screen"

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

        // The meter only ticks while the dialog is actually open, not while
        // it merely exists (LevelMeterPreview.active gates its Timer).
        LevelMeterPreview {
            anchors.horizontalCenter: parent.horizontalCenter
            active: editAudioDialog.shown
        }

        Row {
            width: parent.width
            spacing: Theme.space4

            LevelDial {
                id: editAudioDial
                width: 120
                anchors.top: parent.top
                value: root.editAudioLevel
                minValue: 0
                maxValue: 100
                label: qsTr("Input level")
                onMoved: (v) => root.editAudioLevel = v
            }

            Column {
                anchors.top: parent.top
                width: parent.width - 120 - Theme.space4
                spacing: Theme.space4

                SettingsField {
                    width: parent.width
                    label: qsTr("Name")
                    text: root.editAudioName
                    onTextEdited: (t) => root.editAudioName = t
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
                            model: root.audioKinds
                            delegate: SelectableChip {
                                required property var modelData
                                label: modelData.label
                                selected: root.editAudioKind === modelData.key
                                onPicked: root.editAudioKind = modelData.key
                            }
                        }
                    }
                }

                // Label + options follow the kind: "Device" with hardware
                // picks, or "Media" with content picks — the box shows the
                // stored choice (device/media source) from a scrollable list.
                SelectField {
                    width: parent.width
                    label: root.editAudioKind === "media" ? qsTr("Media") : qsTr("Device")
                    value: root.editAudioSublabel
                    placeholder: root.editAudioKind === "media" ? qsTr("Select media…") : qsTr("Select a device…")
                    options: root.editAudioKind === "media" ? root.mediaSourceOptions : root.deviceOptions
                    onValuePicked: (v) => root.editAudioSublabel = v
                }

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
                        id: editAudioMuteToggle
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.editAudioMuted
                        onToggled: root.editAudioMuted = !root.editAudioMuted
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
                        selected: root.editVideoKind === modelData.key
                        onPicked: root.editVideoKind = modelData.key
                    }
                }
            }
        }

        // ---- Preview (all kinds) — decorative, same mock convention as
        // the board cards' thumbs: the kind's glyph in a real 16:9 pane,
        // with the LIVE/PAUSED status and a decorative resolution readout
        // overlaid on the pane itself rather than living in separate rows.
        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Preview")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Rectangle {
                id: editVideoPreview
                width: parent.width
                height: width * 9 / 16
                radius: Theme.radiusMd
                color: "#0d0f16"
                border.width: 1
                border.color: Theme.border
                clip: true

                Item {
                    anchors.centerIn: parent
                    visible: root.editVideoKind === "camera"
                    width: 64; height: 64

                    Rectangle {
                        anchors.centerIn: parent
                        width: 44; height: 44; radius: 22
                        color: "transparent"
                        border.width: 3
                        border.color: Theme.textMuted
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: 14; height: 14; radius: 7
                        color: Theme.textMuted
                    }
                }

                Item {
                    anchors.centerIn: parent
                    visible: root.editVideoKind === "screen"
                    width: 80; height: 60

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        width: 64; height: 42; radius: 3
                        color: "transparent"
                        border.width: 2.4
                        border.color: Theme.textMuted
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 24; height: 3; radius: 1.5
                        color: Theme.textMuted
                    }
                }

                Shape {
                    anchors.centerIn: parent
                    visible: root.editVideoKind === "media"
                    width: 40; height: 46
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        fillColor: Theme.textMuted
                        strokeColor: "transparent"
                        startX: 0; startY: 0
                        PathLine { x: 0; y: 46 }
                        PathLine { x: 40; y: 23 }
                        PathLine { x: 0; y: 0 }
                    }
                }

                // The pill IS the toggle — same convention as every card on
                // the board, so there's no separate toggle row below. A
                // camera/screen feed isn't "muted" when off, it's paused;
                // only media carries audio to mute.
                MutePill {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 10
                    muted: root.editVideoMuted
                    offLabel: root.editVideoKind === "media" ? qsTr("MUTE") : qsTr("PAUSED")
                    accent: Theme.info
                    accentLight: Theme.infoLight
                    onToggleRequested: root.editVideoMuted = !root.editVideoMuted
                }

                // Decorative resolution/frame-rate readout — no real video
                // engine anywhere in this app, same mock convention as the
                // level meters.
                Pill {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 10
                    anchors.bottomMargin: 10
                    text: qsTr("1080p · 60fps")
                    tint: false
                    fontSize: 9
                }
            }

            // Kind-specific hint: media mutes audio, camera/screen pauses
            // the feed itself.
            Text {
                width: parent.width
                text: root.editVideoKind === "media"
                      ? qsTr("Muting silences this media's audio track.")
                      : qsTr("Pausing blacks this feed out at the bus it feeds — cameras and screens carry no audio track.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }

            // Media-only volume — a plain line meter with its value, not
            // the fuller dial + animated-bar treatment the Edit Audio
            // dialog gets: this is a supplementary control on a VIDEO
            // dialog, kept simple on purpose.
            LabeledSlider {
                width: parent.width
                visible: root.editVideoKind === "media"
                label: qsTr("Volume")
                suffix: "%"
                value: root.editVideoLevel
                onMoved: (v) => root.editVideoLevel = v
            }
        }

        SettingsField {
            width: parent.width
            label: qsTr("Device")
            placeholder: qsTr("e.g. “PTZ Camera · SDI 1”")
            text: root.editVideoSublabel
            onTextEdited: (t) => root.editVideoSublabel = t
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

        LabeledSlider {
            width: parent.width
            label: qsTr("Level")
            suffix: "%"
            value: root.editBusLevel
            onMoved: (v) => root.editBusLevel = v
        }

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
                    }
                }
            }
        }

        // Meter + dial + mute — audio only. Meter ticks only while open.
        Row {
            width: parent.width
            spacing: Theme.space4
            visible: root.addSourceType === "audio"

            Column {
                spacing: Theme.space2

                LevelMeterPreview {
                    anchors.horizontalCenter: parent.horizontalCenter
                    active: addSourceDialog.shown && root.addSourceType === "audio"
                }

                LevelDial {
                    width: 120
                    value: root.addSourceLevel
                    minValue: 0
                    maxValue: 100
                    label: qsTr("Input level")
                    onMoved: (v) => root.addSourceLevel = v
                }
            }

            Column {
                width: parent.width - 120 - Theme.space4
                spacing: Theme.space4

                SettingsField {
                    width: parent.width
                    label: qsTr("Name")
                    placeholder: qsTr("e.g. “Mic 3 · Podium”")
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
                            model: root.audioKinds
                            delegate: SelectableChip {
                                required property var modelData
                                label: modelData.label
                                selected: root.addSourceKind === modelData.key
                                onPicked: root.addSourceKind = modelData.key
                            }
                        }
                    }
                }

                // Same kind-following select as Edit — Device/Media label
                // and options swap with the kind chips above.
                SelectField {
                    width: parent.width
                    label: root.addSourceKind === "media" ? qsTr("Media") : qsTr("Device")
                    value: root.addSourceSublabel
                    placeholder: root.addSourceKind === "media" ? qsTr("Select media…") : qsTr("Select a device…")
                    options: root.addSourceKind === "media" ? root.mediaSourceOptions : root.deviceOptions
                    onValuePicked: (v) => root.addSourceSublabel = v
                }

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
                        id: addSourceMuteToggle
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.addSourceMuted
                        onToggled: root.addSourceMuted = !root.addSourceMuted
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
                            onPicked: root.addSourceKind = modelData.key
                        }
                    }
                }
            }

            // ---- Preview — same layout as the Edit Video dialog: the
            // kind's glyph in a real 16:9 pane, with the LIVE/PAUSED status
            // and a decorative resolution readout overlaid on the pane
            // itself. Moved up, right under Kind, ahead of Device.
            Column {
                width: parent.width
                spacing: Theme.space2

                Text {
                    text: qsTr("Preview")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }

                Rectangle {
                    id: addSourceVideoPreview
                    width: parent.width
                    height: width * 9 / 16
                    radius: Theme.radiusMd
                    color: "#0d0f16"
                    border.width: 1
                    border.color: Theme.border
                    clip: true

                    Item {
                        anchors.centerIn: parent
                        visible: root.addSourceKind === "camera"
                        width: 64; height: 64

                        Rectangle {
                            anchors.centerIn: parent
                            width: 44; height: 44; radius: 22
                            color: "transparent"
                            border.width: 3
                            border.color: Theme.textMuted
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 14; height: 14; radius: 7
                            color: Theme.textMuted
                        }
                    }

                    Item {
                        anchors.centerIn: parent
                        visible: root.addSourceKind === "screen"
                        width: 80; height: 60

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            width: 64; height: 42; radius: 3
                            color: "transparent"
                            border.width: 2.4
                            border.color: Theme.textMuted
                        }
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            width: 24; height: 3; radius: 1.5
                            color: Theme.textMuted
                        }
                    }

                    Shape {
                        anchors.centerIn: parent
                        visible: root.addSourceKind === "media"
                        width: 40; height: 46
                        preferredRendererType: Shape.CurveRenderer

                        ShapePath {
                            fillColor: Theme.textMuted
                            strokeColor: "transparent"
                            startX: 0; startY: 0
                            PathLine { x: 0; y: 46 }
                            PathLine { x: 40; y: 23 }
                            PathLine { x: 0; y: 0 }
                        }
                    }

                    // The pill IS the toggle — same convention as the board
                    // cards and the Edit Video dialog. A camera/screen feed
                    // isn't "muted" when off, it's paused.
                    MutePill {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.leftMargin: 10
                        muted: root.addSourceMuted
                        offLabel: root.addSourceKind === "media" ? qsTr("MUTE") : qsTr("PAUSED")
                        accent: Theme.info
                        accentLight: Theme.infoLight
                        onToggleRequested: root.addSourceMuted = !root.addSourceMuted
                    }

                    // Decorative resolution/frame-rate readout — no real
                    // video engine anywhere in this app.
                    Pill {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 10
                        anchors.bottomMargin: 10
                        text: qsTr("1080p · 60fps")
                        tint: false
                        fontSize: 9
                    }
                }

                // Media-only volume — a plain line meter with its value,
                // not the fuller dial + animated-bar treatment the audio
                // side gets: kept simple on this video dialog.
                LabeledSlider {
                    width: parent.width
                    visible: root.addSourceKind === "media"
                    label: qsTr("Volume")
                    suffix: "%"
                    value: root.addSourceLevel
                    onMoved: (v) => root.addSourceLevel = v
                }
            }

            SettingsField {
                width: parent.width
                label: qsTr("Device")
                placeholder: qsTr("e.g. “PTZ Camera · SDI 1”")
                text: root.addSourceSublabel
                onTextEdited: (t) => root.addSourceSublabel = t
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
