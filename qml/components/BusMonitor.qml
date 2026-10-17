import QtQuick
import VGRPresenterUI

// ONE BUS, read the way the Media pane's Inputs tab reads it.
//
// The bus picker and the bus item on the canvas have to say the same thing as
// the card in Media › Inputs — which source feeds the bus, whether its frames
// are really arriving (so a live thumbnail is worth drawing), and the
// three-state light the operator learned there:
//
//   green  "connected"  a source is routed in and its device is there
//   amber  "offline"    routed, but the device it names is gone
//          "starved"    routed and there, but the tap the app holds is handing
//                       nothing over (same amber, different sentence)
//   red    "none"       nothing routed into this bus at all
//
// The rules are copied from MediaLibraryPane's bus card because that card is
// where the operator's model was worked out ("green connect, but the source is
// not active yellow, not connected red"). Pulling the same answers through one
// component is what keeps a future bus surface from disagreeing with the pane —
// and nothing here STARTS work: the routing graph is a model read, reachability
// is a cached-roster probe, and the delivery question reads the taps the app
// already holds.
QtObject {
    id: root

    // Row in BusListModel. -1 = no bus (an unassigned canvas item, a stale id).
    property int busRow: -1

    // ---- SOURCE MODE ------------------------------------------------------
    // A bus is not the only thing these questions are asked about. The "New bus"
    // picker offers the SOURCES a bus could be built from, and a source card has
    // to answer exactly what a bus card answers — is a feed actually flowing, is
    // the device there, what colour is the light — before any bus exists to
    // route it into. Set sourceKind ("video"|"audio") and sourceRow to the
    // roster row and every answer below comes from that row instead of a bus;
    // the defaults ("" / -1) leave the bus behaviour exactly as it was. One
    // component, so a source and a bus can never disagree about a device.
    property string sourceKind: ""
    property int sourceRow: -1
    readonly property bool sourceMode: root.sourceKind !== "" && root.sourceRow >= 0
    // Bumped by the models below; every computed value reads it so a rename, a
    // re-route or a roster change re-answers on its own.
    property int rev: 0

    // The owner's own re-ask (the picker bumps it when it opens, so a picker
    // built at boot cannot greet the operator with the answer it read before a
    // single device was enumerated).
    property int tick: 0

    // The models' own change signals. A QtObject has no default property, so
    // these cannot be child items — they are Connections held by PROPERTIES,
    // the shape DesignBackend/ShowSession already use. The property is what
    // makes the teardown right: a Connections is a QObject owned by this
    // monitor, so it drops its hooks when the monitor is destroyed. A bare
    // `Model.signal.connect(handler)` in Component.onCompleted cannot: the
    // models outlive every monitor, the connection keeps the monitor (and its
    // status probe) alive, and a parked delegate then bumps a dead object for
    // the rest of the session (QML logged "Cannot read property 'rev' of
    // null" for exactly that, on every roster change).
    property Connections busConn: Connections {
        target: BusListModel
        function onDataChanged() { root.bump() }
        function onRowsInserted() { root.bump() }
        function onRowsRemoved() { root.bump() }
        function onModelReset() { root.bump() }
    }
    property Connections videoConn: Connections {
        target: VideoSourceListModel
        function onDataChanged() { root.bump() }
        function onRowsInserted() { root.bump() }
        function onRowsRemoved() { root.bump() }
        function onModelReset() { root.bump() }
    }
    property Connections audioConn: Connections {
        target: AudioInputListModel
        function onDataChanged() { root.bump() }
        function onRowsInserted() { root.bump() }
        function onRowsRemoved() { root.bump() }
        function onModelReset() { root.bump() }
    }
    // THE MISSING HALF OF THE PROBE: inputAvailable() may PING the machine
    // when its cached roster cannot answer, and that answer arrives later
    // through EngineBridge.devicesChanged. The Media pane's own points listen
    // to it (its devicesRev), which is why a bus could read green there and red
    // here at the same moment — this monitor read the cold "no" and nothing
    // ever told it the roster had filled in.
    property Connections deviceConn: Connections {
        target: EngineBridge
        function onDevicesChanged() { root.bump() }
    }

    // A signal handler takes the emit's arguments and ignores them; the revision
    // is all this needs. The `root` guard stays as a belt beside the connections'
    // own teardown: these run from a singleton's emit, and a signal already in
    // flight when the destruction begins must not touch a torn-down object.
    function bump() { if (root) root.rev++ }

    readonly property var bus: {
        void root.rev
        return root.busRow >= 0 ? BusListModel.getBus(root.busRow) : ({})
    }
    readonly property string busName: root.bus.name !== undefined ? String(root.bus.name) : ""
    // The row being read, bus or source — the one object both modes describe.
    readonly property var sourceRowData: {
        void root.rev
        if (!root.sourceMode)
            return ({})
        return root.sourceKind === "audio"
            ? AudioInputListModel.getInput(root.sourceRow)
            : VideoSourceListModel.getSource(root.sourceRow)
    }
    // The name to put under the window: the source's in source mode, the bus's
    // otherwise (so a tile never has to know which mode it is drawing).
    readonly property string title: root.sourceMode
        ? (root.sourceRowData.name !== undefined && String(root.sourceRowData.name) !== ""
           ? String(root.sourceRowData.name) : root.sourceKind)
        : root.busName
    readonly property string busType: root.bus.type !== undefined ? String(root.bus.type) : ""
    readonly property real busLevel: root.bus.level !== undefined ? root.bus.level : 0
    readonly property int busId: root.bus.id !== undefined ? root.bus.id : 0

    // The FIRST routed video source — the one whose frames a bus thumbnail can
    // show (the pane's card makes the same choice, so the two cannot pick
    // different feeds for the same bus).
    readonly property int videoSourceIndex: {
        void root.rev
        if (root.sourceMode)
            return root.sourceKind === "video" ? root.sourceRow : -1
        if (root.busRow < 0 || root.busType === "audio")
            return -1
        const list = root.bus.routedVideoSources
        return list && list.length > 0 ? list[0] : -1
    }
    readonly property var videoSource: root.videoSourceIndex >= 0
        ? VideoSourceListModel.getSource(root.videoSourceIndex) : ({})
    readonly property string videoLabel: root.videoSource.sublabel !== undefined
        ? String(root.videoSource.sublabel).trim() : ""
    readonly property string videoKind: root.videoSource.kind !== undefined
        ? String(root.videoSource.kind) : ""

    // Is a thumbnail actually flowing? cardPreviews is the app's list of taps it
    // is pulling frames for; asking it means this component never starts a tap
    // of its own just to fill a picture.
    readonly property bool previewing: {
        void root.rev
        void LiveOutputService.inputRev
        return root.videoLabel !== "" && LiveOutputService.cardPreviews.indexOf(root.videoLabel) >= 0
    }

    // Frames really arriving for this label — under EITHER hold: a card preview
    // or an output take. inputProducing() is a plain call, so inputRev (the
    // pump's own tick) is the tracked dependency.
    readonly property bool producing: {
        void root.rev
        void LiveOutputService.inputRev
        return root.videoLabel !== "" && LiveOutputService.inputProducing(root.videoLabel)
    }
    // Is there a picture to draw? Bus mode keeps its long-standing answer (a
    // CARD PREVIEW is what fills a thumbnail there); a source card also shows
    // the feed when the source is simply on the output, because those frames are
    // arriving anyway and hiding them would be the odd thing to do.
    readonly property bool showingFeed: root.sourceMode ? root.producing : root.previewing

    // The URL the thumbnail Image binds to. The `?n=` nonce is what makes a
    // still URL refresh (the image provider caches by URL); it rides inputRev,
    // the same pump tick the pane's own preview uses.
    readonly property string previewSource: root.showingFeed
        ? "image://videopreview/" + encodeURIComponent(root.videoLabel) + "?n=" + LiveOutputService.inputRev
        : ""

    // ---- the light ---------------------------------------------------------
    readonly property string status: {
        void root.rev
        void root.tick
        void LiveOutputService.inputRev
        // A SOURCE is asked exactly what a bus card is asked, with the same
        // probe behind the answer — the light must not say "ready" about a
        // device the pane's own card already calls gone, and the pane's card is
        // the reference the operator learned this light from:
        //
        //   red    "none"       the row names no device to capture at all
        //   amber  "offline"    the device it names is not there (a closed
        //                       window, an unplugged camera)
        //   amber  "starved"    there, and a tap the app holds is delivering
        //                       nothing
        //   green  "connected"  there and usable
        //
        // The probe is the bridge's own (EngineBridge::inputSourceAlive): a
        // cached union of the device sweeps when it can answer, and a real ping
        // when it cannot. It is the same call, and the same cost, the bus cards
        // in the picker right beside these ones already pay — the light's own
        // one-line-per-source diagnostic lands in the engine log when it cannot
        // answer, which is what makes a wrong light diagnosable.
        if (root.sourceMode) {
            const audio = root.sourceKind === "audio"
            const label = audio
                ? (root.sourceRowData.sublabel !== undefined
                   ? String(root.sourceRowData.sublabel).trim() : "")
                : root.videoLabel
            const kind = audio ? "audio" : root.videoKind
            if (label === "")
                return "none"
            if (!LiveOutputService.inputAvailable(label, kind))
                return "offline"
            return LiveOutputService.inputDelivery(label, kind) === "starved" ? "starved" : "connected"
        }
        if (root.busRow < 0)
            return "none"
        const audio = root.bus.routedAudioInputs !== undefined ? root.bus.routedAudioInputs : []
        const video = root.bus.routedVideoSources !== undefined ? root.bus.routedVideoSources : []
        // Only the kinds this bus's own type carries count as a connection.
        const routedVideo = root.busType !== "audio" ? video.length : 0
        const routedAudio = root.busType !== "video" ? audio : []
        if (routedVideo === 0 && routedAudio.length === 0)
            return "none"

        let starved = false
        if (routedVideo > 0) {
            if (LiveOutputService.inputAvailable(root.videoLabel, root.videoKind)) {
                if (LiveOutputService.inputDelivery(root.videoLabel, root.videoKind) === "starved")
                    starved = true
                else
                    return "connected"
            }
        }
        for (const r of routedAudio) {
            const data = AudioInputListModel.getInput(r)
            if (!data || data.sublabel === undefined)
                continue
            const label = String(data.sublabel)
            if (LiveOutputService.inputAvailable(label, "audio")) {
                if (LiveOutputService.inputDelivery(label, "audio") === "starved")
                    starved = true
                else
                    return "connected"
            }
        }
        if (starved)
            return "starved"
        return "offline"
    }
    readonly property color statusColor: root.status === "connected" ? Theme.success
        : (root.status === "offline" || root.status === "starved") ? Theme.warning
        : Theme.dangerLight
    readonly property string statusText: root.sourceMode
        ? (root.status === "connected" ? qsTr("device ready")
           : root.status === "offline" ? qsTr("device not available")
           : root.status === "starved" ? qsTr("source is not delivering")
           : qsTr("no device"))
        : root.status === "connected" ? qsTr("Bus connected")
          : root.status === "offline" ? qsTr("Bus connected · source not available")
          : root.status === "starved" ? qsTr("Bus connected · source is not delivering")
          : qsTr("Bus not connected")

    // The row a STABLE bus id names, or -1 — what a canvas item carries in its
    // meta (ids survive renames and row moves; rows do not).
    function rowForBusId(id) {
        void root.rev
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) {
            const b = BusListModel.getBus(i)
            if (b.id !== undefined && Number(b.id) === Number(id))
                return i
        }
        return -1
    }
}
