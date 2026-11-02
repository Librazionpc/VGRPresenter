import QtQuick
import VGRPresenterUI

QtObject {
    id: root

    // Row in BusListModel. -1 = no bus (an unassigned canvas item, a stale id).
    property int busRow: -1

    // ---- SOURCE MODE ------------------------------------------------------
    property string sourceKind: ""
    property int sourceRow: -1
    readonly property bool sourceMode: root.sourceKind !== "" && root.sourceRow >= 0
    // Bumped by the models below; every computed value reads it so a rename, a
    // re-route or a roster change re-answers on its own.
    property int rev: 0

    property int tick: 0

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
    property Connections deviceConn: Connections {
        target: EngineBridge
        function onDevicesChanged() { root.bump() }
    }
    property Timer windowWatch: Timer {
        interval: 2000
        repeat: true
        running: root.videoKind === "screen" && root.videoLabel !== "" && root.status === "offline"
        onTriggered: EngineBridge.refreshWindow(root.videoLabel)
    }

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

    readonly property bool previewing: {
        void root.rev
        void LiveOutputService.inputRev
        return root.videoLabel !== "" && LiveOutputService.cardPreviews.indexOf(root.videoLabel) >= 0
    }

    readonly property bool producing: {
        void root.rev
        void LiveOutputService.inputRev
        return root.videoLabel !== "" && LiveOutputService.inputProducing(root.videoLabel)
    }
    readonly property bool showingFeed: root.sourceMode ? root.producing : root.previewing

    readonly property string previewSource: root.showingFeed
        ? "image://videopreview/" + encodeURIComponent(root.videoLabel) + "?n=" + LiveOutputService.inputRev
        : ""

    // ---- the light ---------------------------------------------------------
    readonly property string status: {
        void root.rev
        void root.tick
        void LiveOutputService.inputRev
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