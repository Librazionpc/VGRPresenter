import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

Item {
    id: root

    signal sectionRequested(string key)
    // "Edit this screen": a screen's editor is the Screens page's, so the canvas
    // reports WHICH row (it right-clicked it) and lets the shell open it there.
    signal screenEditRequested(int row)

    // ---- Rosters, with the revision bridge every settings screen uses: the
    // models' Q_INVOKABLE reads are not tracked by QML's binding system, so this
    // counter is the honest dependency, bumped by the models' own signals.
    property int modelsRev: 0
    Connections {
        target: BusListModel
        function onDataChanged() { root.rostersChanged() }
        function onRowsInserted() { root.rostersChanged() }
        function onRowsRemoved() { root.rostersChanged() }
        function onModelReset() { root.rostersChanged() }
    }
    Connections {
        target: OutputListModel
        function onDataChanged() { root.rostersChanged() }
        function onRowsInserted() { root.rostersChanged() }
        function onRowsRemoved() { root.rostersChanged() }
        function onModelReset() { root.rostersChanged() }
        function onOutputRosterChanged() { root.rostersChanged() }
    }
    Connections {
        target: StyleListModel
        function onDataChanged() { root.rostersChanged() }
        function onRowsInserted() { root.rostersChanged() }
        function onRowsRemoved() { root.rostersChanged() }
        function onModelReset() { root.rostersChanged() }
        function onRosterChanged() { root.rostersChanged() }
    }
    // The recording / streaming presets (Settings · Recording): a card's subtitle
    // and the picker's list both read them.
    Connections {
        target: PresetListModel
        function onDataChanged() { root.rostersChanged() }
        function onRowsInserted() { root.rostersChanged() }
        function onRowsRemoved() { root.rostersChanged() }
        function onModelReset() { root.rostersChanged() }
    }
    Connections {
        target: RoutingService
        function onRoutingChanged() { root.modelsRev++ }
        function onRowsChanged() { root.modelsRev++ }
        function onRefused(reason) { EventBus.notify(String(reason)) }
    }

    // ---- THE BOARD IS A PATCH BAY, NOT A ROSTER DUMP -------------------------
    readonly property var busRoster: {
        root.modelsRev
        const out = []
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) out.push(BusListModel.getBus(i))
        return out
    }
    readonly property var busRows: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.busRoster.length; ++i) {
            const bus = root.busRoster[i]
            if (bus.onBoard !== true)
                continue
            const entry = Object.assign({}, bus)
            entry.row = i
            out.push(entry)
        }
        return out
    }
    // The buses the operator has NOT put on the board yet — what the Buses "+"
    // offers to add, so the rule never hides something that cannot be reached.
    readonly property var unboardedBuses: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.busRoster.length; ++i)
            if (root.busRoster[i].onBoard !== true) out.push(i)
        return out
    }
    readonly property bool screensShown: false
    readonly property var outputRows: {
        root.modelsRev
        return RoutingService.rows
    }
    readonly property var screenRows: {
        root.modelsRev
        const out = []
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].screen && root.screensShown) out.push(rows[i])
        return out
    }
    readonly property var sinkRows: {
        root.modelsRev
        const out = []
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i)
            if (!rows[i].screen || !root.screensShown) out.push(rows[i])
        return out
    }
    // Styles, split the same way (see busRoster/busRows): the roster is every
    // style; the board is the ones the operator put there.
    readonly property var styleRoster: {
        root.modelsRev
        const out = []
        const n = StyleListModel.rowCount()
        for (let i = 0; i < n; ++i) out.push(StyleListModel.getStyle(i))
        return out
    }
    readonly property var styleRows: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.styleRoster.length; ++i) {
            const style = root.styleRoster[i]
            if (style.onBoard !== true)
                continue
            const entry = Object.assign({}, style)
            entry.row = i
            out.push(entry)
        }
        return out
    }
    readonly property var unboardedStyles: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.styleRoster.length; ++i)
            if (root.styleRoster[i].onBoard !== true) out.push(i)
        return out
    }

    // ---- The Outputs column is SEGMENTED -------------------------------------
    function sinkGroupOf(o) {
        if (o.system) return "audio"
        const kind = String(o.kind || "").toUpperCase()
        if (kind === "NDI") return "network"
        if (kind === "SDI" || kind === "HDMI") return "physical"
        if (kind === "REC") return "recording"
        if (kind === "STREAM") return "streaming"
        return "other"
    }
    readonly property var sinkGroups: {
        root.modelsRev
        const keys = ["audio", "network", "physical", "recording", "streaming", "other"]
        const labels = {
            audio: qsTr("Audio"), network: qsTr("Network"), physical: qsTr("Physical"),
            recording: qsTr("Recording"), streaming: qsTr("Streaming"), other: qsTr("Other")
        }
        const buckets = {}
        for (let k = 0; k < keys.length; ++k) buckets[keys[k]] = []
        const rows = root.sinkRows
        for (let i = 0; i < rows.length; ++i) buckets[root.sinkGroupOf(rows[i])].push(rows[i])
        const out = []
        for (let k = 0; k < keys.length; ++k)
            if (buckets[keys[k]].length > 0)
                out.push({ key: keys[k], label: labels[keys[k]], rows: buckets[keys[k]] })
        return out
    }
    // The same rows, flattened in the order the column draws them — what the
    // card Repeater iterates and what `rowOf` counts through.
    readonly property var sinkFlat: {
        root.modelsRev
        const out = []
        const groups = root.sinkGroups
        for (let g = 0; g < groups.length; ++g) {
            const rows = groups[g].rows
            for (let i = 0; i < rows.length; ++i) out.push(rows[i])
        }
        return out
    }

    // ---- The Buses column is SEGMENTED BY KIND -------------------------------
    function busGroupOf(bus) {
        const t = String(bus && bus.type !== undefined ? bus.type : "audio")
        if (t === "video" || t === "both" || t === "template") return t
        return "audio"
    }
    readonly property var busGroups: {
        root.modelsRev
        const keys = ["video", "both", "audio", "template"]
        // The names the card's own badge uses (V, A+V, A, TPL), so a marker and
        // the run beneath it read as one thing.
        const labels = { video: qsTr("Video"), both: qsTr("A+V"),
                         audio: qsTr("Audio"), template: qsTr("Template") }
        const buckets = {}
        for (let k = 0; k < keys.length; ++k) buckets[keys[k]] = []
        const rows = root.busRows
        for (let i = 0; i < rows.length; ++i)
            buckets[root.busGroupOf(rows[i])].push(i)
        const out = []
        for (let k = 0; k < keys.length; ++k)
            if (buckets[keys[k]].length > 0)
                out.push({ key: keys[k], label: labels[keys[k]], rows: buckets[keys[k]] })
        return out
    }
    readonly property var busOrder: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.busRows.length; ++i) out.push(Number(root.busRows[i].row))
        return out
    }
    function busSlotOfRow(row) { return root.busOrder.indexOf(row) }
    // The style half of the same bridge (see busOrder).
    readonly property var styleOrder: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.styleRows.length; ++i) out.push(Number(root.styleRows[i].row))
        return out
    }
    function styleSlotOfRow(row) { return root.styleOrder.indexOf(row) }
    function styleRowOf(key) { return Number(key.substring(6)) }
    // The ROSTER row a key names — what data reads and every model call take
    // (rowOf answers the SLOT, which is geometry).
    function busRowOf(key) { return Number(key.substring(4)) }
    function busGroupTop(g) {
        let y = root.colHeaderH
        const groups = root.busGroups
        for (let i = 0; i < g && i < groups.length; ++i)
            y += root.groupLabelH + groups[i].rows.length * (root.cardH + root.rowGap)
        return y
    }
    function busSlotTop(i) {
        let y = root.colHeaderH
        let at = 0
        const groups = root.busGroups
        for (let g = 0; g < groups.length; ++g) {
            const n = groups[g].rows.length
            y += root.groupLabelH
            if (i < at + n) return y + (i - at) * (root.cardH + root.rowGap)
            y += n * (root.cardH + root.rowGap)
            at += n
        }
        return y
    }
    // The card's y from its ROSTER row (the number everything else uses).
    function busTopOfRow(row) {
        const slot = root.busSlotOfRow(row)
        return slot < 0 ? -1 : root.busSlotTop(slot)
    }
    readonly property int busColumnH: root.colHeaderH
        + root.busGroups.length * root.groupLabelH
        + root.busRows.length * (root.cardH + root.rowGap)
        + root.rowGap

    // ---- Canvas geometry. ONE set of formulas for both the cards and the port
    readonly property int colHeaderH: 52
    readonly property int cardH: 56
    readonly property int rowGap: 10
    readonly property int gutter: 44
    readonly property int columnCount: root.screensShown ? 4 : 3
    readonly property real minBoardW: root.columnCount * 130 + (root.columnCount - 1) * root.gutter

    readonly property real boardW: Math.max(flick.width, root.minBoardW)
    readonly property real boardPad: 24
    readonly property real colMaxW: 240
    readonly property real colW: Math.min(root.colMaxW,
        (root.boardW - root.gutter * (root.columnCount - 1) - root.boardPad * 2) / root.columnCount)
    readonly property real groupLeft: Math.max(root.boardPad,
        (root.boardW - (root.columnCount * root.colW + root.gutter * (root.columnCount - 1))) / 2)
    readonly property int addBtnSize: 22
    function columnHeight(rows) {
        return root.colHeaderH + rows * (root.cardH + root.rowGap) + root.rowGap
    }
    // The segment labels are one short line each.
    readonly property int groupLabelH: 18
    readonly property int sinkColumnH: root.colHeaderH
        + root.sinkGroups.length * root.groupLabelH
        + root.sinkRows.length * (root.cardH + root.rowGap)
        + root.rowGap
    readonly property real boardH: Math.max(
        Math.max(root.busColumnH, root.columnHeight(root.screenRows.length)),
        Math.max(root.stylesColumnH, root.sinkColumnH)) + 10

    // Outputs is column 3 in the layer order. With Screens off it sits in slot 2, the
    // slot Screens would have taken.
    function columnX(column) {
        const slot = !root.screensShown && column > 2 ? column - 1 : column
        return root.groupLeft + slot * (root.colW + root.gutter)
    }
    function rowTop(row) { return root.colHeaderH + row * (root.cardH + root.rowGap) }
    function gateTop(i) {
        return root.colHeaderH + root.groupLabelH + i * (root.cardH + root.rowGap)
    }
    function styleTop(row) {
        return root.colHeaderH + root.groupLabelH
             + root.gateCards.length * (root.cardH + root.rowGap)
             + root.groupLabelH + row * (root.cardH + root.rowGap)
    }
    readonly property int stylesColumnH: root.colHeaderH + 2 * root.groupLabelH
        + (root.gateCards.length + root.styleRows.length) * (root.cardH + root.rowGap)
        + root.rowGap
    // The y of the segment label above group `g`.
    function sinkGroupTop(g) {
        let y = root.colHeaderH
        const groups = root.sinkGroups
        for (let i = 0; i < g && i < groups.length; ++i)
            y += root.groupLabelH + groups[i].rows.length * (root.cardH + root.rowGap)
        return y
    }
    // The y of the card at FLAT sink index `i` — and, for i == the row count,
    // the y the "add output" tile takes (just under the last card).
    function sinkSlotTop(i) {
        let y = root.colHeaderH
        let at = 0
        const groups = root.sinkGroups
        for (let g = 0; g < groups.length; ++g) {
            const n = groups[g].rows.length
            y += root.groupLabelH
            if (i < at + n) return y + (i - at) * (root.cardH + root.rowGap)
            y += n * (root.cardH + root.rowGap)
            at += n
        }
        return y
    }
    function cardTopOf(key) {
        const c = root.columnOf(key)
        const r = root.rowOf(key)
        if (c < 0 || r < 0) return -1
        // The gate cards are NOT style rows: they sit in the column's first
        // segment, above the STYLE TEMPLATES marker.
        if (key.indexOf("gate:") === 0)
            return root.gateTop(r)
        // `r` is a SLOT (see rowOf), so the board's own slot geometry answers.
        return c === 0 ? root.busSlotTop(r)
             : c === 3 ? root.sinkSlotTop(r)
             : c === 1 ? root.styleTop(r) : root.rowTop(r)
    }

    // ---- Creating a layer from the canvas ------------------------------------
    property string addMenu: ""
    property Item addAnchor: null
    // Which panel a column's "+" owns: the two roster LISTS (buses, styles) and
    // the Outputs transport picker. Screens has no "+" yet.
    function addMenuFor(column) {
        return column === 0 ? "bus"
             : column === 1 ? "style"
             : column === 3 ? "output" : ""
    }
    onAddMenuChanged: {
        if (root.addMenu === "bus" && root.addAnchor)
            busAddMenu.openAt(root.addAnchor, 0, root.addAnchor.height + 4, root)
        else if (busAddMenu.visible)
            busAddMenu.visible = false
        if (root.addMenu === "style" && root.addAnchor)
            styleAddMenu.openAt(root.addAnchor, 0, root.addAnchor.height + 4, root)
        else if (styleAddMenu.visible)
            styleAddMenu.visible = false
    }
    function beginAdd(column, anchorItem) {
        const menu = root.addMenuFor(column)
        if (menu.length === 0)
            return
        if (menu === "output") {
            root.toggleAddMenu("output")
            return
        }
        if (!anchorItem)
            return
        root.addAnchor = anchorItem
        // The same "+" closes the list it opened.
        root.addMenu = root.addMenu === menu ? "" : menu
    }
    // What each "+" offers: every roster item not on the board yet, then the
    // column's own create flow.
    readonly property var addBusMenuItems: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.unboardedBuses.length; ++i) {
            const row = root.unboardedBuses[i]
            const bus = root.busRoster[row]
            out.push({ id: row, label: String(bus.name || ""),
                       trailing: root.typeLabel(bus.type) })
        }
        if (out.length > 0)
            out.push({ divider: true })
        out.push({ id: -1, label: qsTr("New bus…") })
        return out
    }
    readonly property var addStyleMenuItems: {
        root.modelsRev
        const out = []
        for (let i = 0; i < root.unboardedStyles.length; ++i) {
            const row = root.unboardedStyles[i]
            const style = root.styleRoster[row]
            out.push({ id: row, label: String(style.name || ""),
                       trailing: root.styleSubtitle(style) })
        }
        if (out.length > 0)
            out.push({ divider: true })
        out.push({ id: -1, label: qsTr("New style…") })
        return out
    }
    function pickAddMenuItem(column, item) {
        root.addMenu = ""
        const id = (item && item.id !== undefined && item.id !== null) ? Number(item.id) : -1
        if (column === 0) {
            if (id < 0) {
                busPicker.open = true
                return
            }
            BusListModel.setBusOnBoard(id, true)
            return
        }
        if (id < 0) {
            StyleListModel.addStyle()
            StyleListModel.setOnBoard(StyleListModel.rowCount() - 1, true)
            return
        }
        StyleListModel.setOnBoard(id, true)
    }

    // ---- Styles: picked from the roster, never drawn onto the canvas -------
    function openStylePicker() {
        stylePicker.selectedId = ""
        stylePicker.open = true
    }
    function markStyle(styleId) {
        const at = root.styleById(String(styleId || ""))
        if (at < 0) return
        root.hoveredNode = root.keyForStyle(at)
    }

    // ---- Configure an output: the OLD Outputs dialog, per transport ---------
    property int editOutputRow: -1
    property bool outputDialogShown: false
    property string outName: ""
    property string outKind: "HDMI"
    property string outRes: "1920 × 1080"
    property string outRefresh: "60 Hz"
    property string outPattern: "smpte"
    property string outDisplay: ""
    property bool outLocked: false
    property bool outStayOnTop: false
    property string outAudioDevice: ""
    // A Recording / Streaming output has no display, mode or test pattern: it is its
    // name and the PRESET it records / streams with. `outPresetId` is that pick.
    property string outPresetId: ""
    readonly property bool outIsPreset:
        root.outKind.toUpperCase() === "REC" || root.outKind.toUpperCase() === "STREAM"
    readonly property bool outIsHdmi: root.outKind.toUpperCase() === "HDMI"
    readonly property bool outHasDisplay:
        root.outKind.toUpperCase() === "HDMI" || root.outKind.toUpperCase() === "SDI"

    function openAddOutput(kind) {
        root.addMenu = ""
        root.editOutputRow = -1
        root.outName = ""
        root.outKind = String(kind || "HDMI").toUpperCase()
        root.outRes = "1920 × 1080"
        root.outRefresh = "60 Hz"
        root.outPattern = "smpte"
        root.outDisplay = ""
        root.outLocked = false
        root.outStayOnTop = false
        root.outAudioDevice = ""
        root.outPresetId = ""
        root.outputDialogShown = true
    }
    function openEditOutput(row) {
        if (row < 0 || row >= OutputListModel.rowCount())
            return
        const o = OutputListModel.getOutput(row)
        root.editOutputRow = row
        root.outName = String(o.name || "")
        root.outKind = String(o.kind || "HDMI").toUpperCase()
        root.outRes = String(o.res || "") !== "" ? String(o.res) : "1920 × 1080"
        root.outRefresh = String(o.refresh || "") !== "" ? String(o.refresh) : "60 Hz"
        root.outPattern = String(o.testPattern || "") !== "none" ? String(o.testPattern) : "smpte"
        root.outDisplay = String(o.screenName || "")
        root.outLocked = o.boundsLocked === true
        root.outStayOnTop = o.stayOnTop === true
        root.outAudioDevice = o.audioDevice !== undefined && o.audioDevice !== null
            ? String(o.audioDevice) : ""
        root.outPresetId = String(o.presetId || "")
        root.outputDialogShown = true
    }
    function submitOutput() {
        // A Recording / Streaming output is only a name and its preset.
        if (root.outIsPreset && root.editOutputRow >= 0) {
            if (root.outName !== "")
                OutputListModel.renameOutput(root.editOutputRow, root.outName)
            OutputListModel.setPresetId(root.editOutputRow, root.outPresetId)
            root.outputDialogShown = false
            root.editOutputRow = -1
            RoutingService.refresh()
            return
        }
        const row = root.editOutputRow >= 0 ? root.editOutputRow
                                            : OutputListModel.rowCount()
        if (root.editOutputRow < 0)
            OutputListModel.addScreen(root.outName, root.outKind, root.outRes,
                                      root.outRefresh, root.outPattern)
        else if (root.outName !== "")
            OutputListModel.renameOutput(row, root.outName)
        // The rest are setters on a row that now exists, in the same order
        // Settings · Outputs applies them.
        OutputListModel.setResolution(row, root.outRes)
        OutputListModel.setRefresh(row, root.outRefresh)
        OutputListModel.setTestPattern(row, root.outPattern)
        // Before the display rules, because setAudioDevice re-asks the routing
        // pass and that pass rewrites the row's plays-locally flag from it.
        OutputListModel.setAudioDevice(row, root.outAudioDevice)
        if (root.outHasDisplay) {
            OutputListModel.setScreenName(row, root.outDisplay)
            OutputListModel.setBoundsLocked(row, root.outLocked)
        }
        if (root.outIsHdmi)
            OutputListModel.setStayOnTop(row, root.outStayOnTop)
        root.outputDialogShown = false
        root.editOutputRow = -1
        RoutingService.refresh()
    }
    readonly property var outputKinds: {
        root.modelsRev
        const out = [
            { kind: "HDMI", label: qsTr("HDMI"), note: qsTr("A monitor or projector — pick its display next") },
            { kind: "SDI",  label: qsTr("SDI"),  note: qsTr("A capture card's output") },
            { kind: "NDI",  label: qsTr("NDI"),  note: qsTr("Video + audio over the network — name the sender next") }
        ]
        const kinds = [
            { preset: "recording", kind: "REC", noun: qsTr("Recording"),
              none: qsTr("No recording presets yet — make one in Settings · Recording") },
            { preset: "audio", kind: "REC", noun: qsTr("Audio"),
              none: qsTr("No audio presets yet — make one in Settings · Recording") },
            { preset: "streaming", kind: "STREAM", noun: qsTr("Streaming"),
              none: qsTr("No streaming presets yet — make one in Settings · Recording") }
        ]
        for (let k = 0; k < kinds.length; ++k) {
            const list = PresetListModel.presetsOfKind(kinds[k].preset)
            if (list.length === 0) {
                out.push({ kind: "PRESETS", label: qsTr("%1 preset…").arg(kinds[k].noun),
                           note: kinds[k].none })
                continue
            }
            for (let i = 0; i < list.length; ++i)
                out.push({ kind: kinds[k].kind, presetId: list[i].presetId,
                           label: list[i].name,
                           note: kinds[k].noun + " · " + list[i].summary })
        }
        return out
    }
    function toggleAddMenu(which) {
        root.addMenu = root.addMenu === which ? "" : which
    }
    // `entry` is one of outputKinds' rows.
    function addOutput(entry) {
        const k = String(entry.kind || "").toUpperCase()
        if (k.length === 0) return
        // No preset of that kind exists: the picker's own entry says so, and the
        // place that makes one is Settings · Recording.
        if (k === "PRESETS") {
            root.addMenu = ""
            root.sectionRequested("recording")
            return
        }
        if (k === "REC" || k === "STREAM") {
            root.addMenu = ""
            const row = OutputListModel.rowCount()
            OutputListModel.addScreen(String(entry.label || k), k, "", "")
            OutputListModel.setPresetId(row, String(entry.presetId || ""))
            RoutingService.refresh()
            return
        }
        root.openAddOutput(k)
    }

    // ---- Card right-click ----------------------------------------------------
    property string menuKey: ""
    property int deleteOutputRow: -1

    function menuFor(key) {
        const layer = root.columnOf(key)
        if (layer === 0) {
            return [
                { id: "edit", label: qsTr("Edit on Audio & Video") },
                { id: "duplicate", label: qsTr("Duplicate bus") },
                { divider: true },
                // Off the board, on it — the card goes, the bus does not.
                { id: "unboard", label: qsTr("Remove from routing") },
                { id: "delete", label: qsTr("Delete bus"), danger: true }
            ]
        }
        if (layer === 1) {
            return [
                { id: "edit", label: qsTr("Edit style") },
                { id: "duplicate", label: qsTr("Duplicate style") },
                { divider: true },
                // Off the board, on it — the card goes, the style does not.
                { id: "unboard", label: qsTr("Remove from routing") },
                { id: "delete", label: qsTr("Delete style"), danger: true }
            ]
        }
        const row = Number(key.substring(4))
        const o = root.outputByRow(row)
        const gateItems = []
        if (o) {
            gateItems.push({ id: "toggleOverlay", label: o.showOverlay === false
                ? qsTr("Show overlays on this output")
                : qsTr("Hide overlays on this output") })
            gateItems.push({ id: "toggleMedia", label: o.showMedia === false
                ? qsTr("Show media on this output")
                : qsTr("Hide media on this output") })
        }
        if (layer === 2) {
            const items = [{ id: "edit", label: qsTr("Configure output") }]
            if (gateItems.length > 0) {
                items.push({ divider: true })
                for (let i = 0; i < gateItems.length; ++i) items.push(gateItems[i])
            }
            if (o && Number(o.videoBus) >= 0) {
                items.push({ divider: true })
                items.push({ id: "unwireVideo", label: qsTr("Unwire video") })
            }
            if (o)
                items.push({ id: "toggleCompose", label: String(o.videoMode || "") === "compose"
                    ? qsTr("Show direct feed") : qsTr("Compose design") })
            if (o && String(o.styleId || "").length > 0)
                items.push({ id: "unwireStyle", label: qsTr("Take the style off") })
            // No Delete here: a screen is deleted on Settings · Screens, where
            // its display binding and its window are in view.
            return items
        }
        const kind = String(o ? o.kind : "").toUpperCase()
        const items = [{ id: "edit", label: qsTr("Configure output") }]
        // The preset it wears is edited on Settings · Recording.
        if (kind === "REC" || kind === "STREAM")
            items.push({ id: "editRecording", label: qsTr("Edit presets") })
        if (o && Number(o.videoBus) >= 0)
            items.push({ id: "unwireVideo", label: qsTr("Unwire video") })
        // The picture this output shows: its one routed feed, or the design with
        // each bus window showing its own assigned bus (compose).
        if (o)
            items.push({ id: "toggleCompose", label: String(o.videoMode || "") === "compose"
                ? qsTr("Show direct feed") : qsTr("Compose design") })
        if (o && Number(o.audioBus) >= 0)
            items.push({ id: "unwireAudio", label: qsTr("Unwire audio") })
        if (gateItems.length > 0) {
            items.push({ divider: true })
            for (let i = 0; i < gateItems.length; ++i) items.push(gateItems[i])
        }
        if (items.length > 0)
            items.push({ divider: true })
        items.push({ id: "delete", label: qsTr("Delete output"), danger: true })
        return items
    }

    function nodeName(key) {
        const layer = root.columnOf(key)
        const row = root.rowOf(key)
        if (layer === 0)
            return String((root.busRoster[root.busRowOf(key)] || {}).name || "")
        if (layer === 1)
            return String((root.styleRoster[root.styleRowOf(key)] || {}).name || "")
        const o = root.outputByRow(Number(key.substring(4)))
        return o ? String(o.name || "") : ""
    }
    function nodeSummary(key) {
        const layer = root.columnOf(key)
        const row = root.rowOf(key)
        if (layer === 0) {
            const bus = root.busRoster[root.busRowOf(key)]
            return qsTr("Bus · %1").arg(bus ? root.typeLabel(bus.type) : "")
        }
        if (layer === 1) {
            const style = root.styleRoster[root.styleRowOf(key)]
            return qsTr("Style · %1").arg(style ? root.styleSubtitle(style) : "")
        }
        const o = root.outputByRow(Number(key.substring(4)))
        if (!o)
            return ""
        if (layer === 2)
            return qsTr("Screen · %1").arg(String(o.kind || "") + (o.res ? " · " + o.res : ""))
        return qsTr("Output · %1").arg(root.sinkSubtitle(o))
    }

    function openCardMenu(key, card, x, y) {
        const layer = root.columnOf(key)
        if (layer < 0 || !card)
            return
        // A gate card has no menu: its whole surface is the dot its wire leaves
        // from, and the wire is the setting. Nothing to edit, rename or delete.
        if (key.indexOf("gate:") === 0)
            return
        if (layer === 3) {
            const o = root.outputByRow(Number(key.substring(4)))
            if (o && o.system)
                return
        }
        root.menuKey = key
        cardMenu.headerTitle = root.nodeName(key)
        cardMenu.headerSubtitle = root.nodeSummary(key)
        cardMenu.model = root.menuFor(key)
        cardMenu.openAt(card, x, y)
    }

    function editNode(key) {
        // A gate card has no editor page to hop to (its setting is a wire).
        if (key.indexOf("gate:") === 0)
            return
        const layer = root.columnOf(key)
        if (layer === 0)
            root.sectionRequested("av")
        else if (layer === 1)
            root.sectionRequested("styles")
        else if (layer === 2 || layer === 3)
            root.openEditOutput(Number(key.substring(4)))
    }

    function actOnCard(id) {
        const key = root.menuKey
        root.menuKey = ""
        if (!key || !id)
            return
        const layer = root.columnOf(key)
        // ROSTER rows: these are model calls, not geometry.
        if (layer === 0) {
            const row = root.busRowOf(key)
            if (id === "edit") root.editNode(key)
            else if (id === "duplicate") BusListModel.duplicateBus(row)
            else if (id === "delete") BusListModel.deleteBus(row)   // undoable: the toast carries Undo
            else if (id === "unboard") BusListModel.setBusOnBoard(row, false)
            return
        }
        if (layer === 1) {
            const styleRow = root.styleRowOf(key)
            if (id === "edit") root.editNode(key)
            else if (id === "duplicate") StyleListModel.duplicateStyle(styleRow)
            else if (id === "delete") StyleListModel.removeStyle(styleRow)
            else if (id === "unboard") StyleListModel.setOnBoard(styleRow, false)
            return
        }
        const rosterRow = Number(key.substring(4))
        if (layer === 2) {
            if (id === "edit") root.editNode(key)
            else if (id === "unwireVideo") RoutingService.assignBus(rosterRow, "video", -1)
            else if (id === "unwireStyle") OutputListModel.setStyle(rosterRow, "")
            else if (id === "toggleCompose") {
                const so = root.outputByRow(rosterRow)
                OutputListModel.setVideoMode(rosterRow,
                    so && String(so.videoMode || "") === "compose" ? "direct" : "compose")
            }
            return
        }
        if (id === "edit") root.editNode(key)
        else if (id === "editRecording") root.sectionRequested("recording")
        else if (id === "toggleCompose") {
            const o = root.outputByRow(rosterRow)
            OutputListModel.setVideoMode(rosterRow,
                o && String(o.videoMode || "") === "compose" ? "direct" : "compose")
        }
        else if (id === "unwireVideo") RoutingService.assignBus(rosterRow, "video", -1)
        else if (id === "unwireAudio") RoutingService.assignBus(rosterRow, "audio", -1)
        else if (id === "toggleOverlay") {
            const o = root.outputByRow(rosterRow)
            OutputListModel.setShowOverlay(rosterRow, !(o && o.showOverlay !== false))
        }
        else if (id === "toggleMedia") {
            const o = root.outputByRow(rosterRow)
            OutputListModel.setShowMedia(rosterRow, !(o && o.showMedia !== false))
        }
        else if (id === "delete") root.deleteOutputRow = rosterRow
    }

    function portOffset(key, kind) {
        if (!root.cardHasTwoDots(key)) return 0
        if (kind === "video") return -11
        if (kind === "audio") return 11
        return 0
    }
    function cardHasTwoDots(key) {
        const c = root.columnOf(key)
        // A BUS stacks two dots only when it carries both signals; an audio-only
        // or video-only bus has one, so it sits on the centreline.
        if (c === 0) {
            const row = Number(key.substring(4))
            return root.busHasVideo(row) && root.busHasAudio(row)
        }
        if (c !== 3) return false
        const o = root.outputByRow(Number(key.substring(4)))
        if (o === null) return false
        // ...and so does an output that accepts ONE signal (a video-only display,
        // the speaker endpoint), for the same reason: the stack is for the pair.
        return !!(o.acceptsVideo && o.acceptsAudio)
    }

    // ---- Node keys -----------------------------------------------------------
    function outputByRow(row) {
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i)
            if (Number(rows[i].row) === row) return rows[i]
        return null
    }
    function toggleOutputEnabled(row) {
        const output = root.outputByRow(Number(row))
        if (!output) return
        if (output.system)
            RoutingService.setSpeakersEnabled(output.enabled !== true)
        else {
            if (!OutputListModel.setEnabled(Number(output.row), output.enabled !== true)) {
                EventBus.notify(qsTr("At least one output card must remain enabled."),
                                "warning", qsTr("Outputs"), "outputs.lastEnabled")
                return
            }
            if (output.enabled === true && root.takeNode === root.sinkNodeId(output))
                RecordingService.stopRecording()
        }
    }
    function styleById(id) {
        if (!id) return -1
        for (let i = 0; i < root.styleRoster.length; ++i)
            if (String(root.styleRoster[i].id) === String(id)) return i
        return -1
    }

    function columnOf(key) {
        if (key.indexOf("bus:") === 0) return 0
        if (key.indexOf("style:") === 0 || key.indexOf("gate:") === 0) return 1
        if (key.indexOf("out:") === 0) {
            const o = root.outputByRow(Number(key.substring(4)))
            return o === null ? -1 : (o.screen && root.screensShown ? 2 : 3)
        }
        return -1
    }
    // The row this key occupies WITHIN ITS COLUMN — the filtered index, which is
    // what the geometry formulas want.
    function rowOf(key) {
        // SLOTS, not roster rows: everything below measures the DRAWN column, and
        // the board shows only the items the operator added (see busRows).
        if (key.indexOf("bus:") === 0) return root.busSlotOfRow(Number(key.substring(4)))
        if (key.indexOf("style:") === 0) return root.styleSlotOfRow(Number(key.substring(6)))
        if (key.indexOf("gate:") === 0) {
            const id = key.substring(5)
            for (let i = 0; i < root.gateCards.length; ++i)
                if (root.gateCards[i].key === id) return i
            return -1
        }
        if (key.indexOf("out:") === 0) {
            const row = Number(key.substring(4))
            const list = root.columnOf(key) === 2 ? root.screenRows : root.sinkFlat
            for (let i = 0; i < list.length; ++i)
                if (Number(list[i].row) === row) return i
            return -1
        }
        return -1
    }
    function keyForOutput(row) { return "out:" + String(row) }
    function keyForGate(index) {
        const card = root.gateCards[index]
        return card ? "gate:" + card.key : ""
    }
    function keyForStyle(index) { return "style:" + String(index) }
    function keyForBus(index) { return "bus:" + String(index) }

    function isSystem(key) {
        if (key.indexOf("out:") !== 0) return false
        const o = root.outputByRow(Number(key.substring(4)))
        return o !== null && o.system
    }

    // The canvas point a signal's dot occupies on `key`'s incoming or outgoing
    // edge — the ONE place a wire's endpoint comes from.
    function portPoint(key, kind, side) {
        const c = root.columnOf(key)
        const top = root.cardTopOf(key)
        if (c < 0 || top < 0) return null
        return {
            x: side === "out" ? root.columnX(c) + root.colW : root.columnX(c),
            y: top + root.cardH / 2 + root.portOffset(key, kind)
        }
    }

    // ---- Signals -------------------------------------------------------------
    function busType(index) {
        const bus = root.busRoster[index]
        return bus ? String(bus.type || "audio") : "audio"
    }
    function busHasVideo(index) {
        const bus = root.busRoster[index]
        return !bus || bus.type !== "audio"
    }
    function busHasAudio(index) { void index; return true }
    function busSourceCount(index) {
        const bus = root.busRoster[index]
        if (!bus) return 0
        return bus.routedAudioInputs.length + bus.routedVideoSources.length
    }
    function busOutputCount(index) {
        let n = 0
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i) {
            if (root.videoBusesOf(rows[i]).indexOf(index) >= 0
                    || root.audioBusesOf(rows[i]).indexOf(index) >= 0) n++
        }
        return n
    }
    function outputAccepts(row, kind) {
        const o = root.outputByRow(row)
        if (o === null) return false
        return kind === "audio" ? o.acceptsAudio === true : o.acceptsVideo === true
    }
    function audioBusesOf(o) {
        if (!o) return []
        if (o.audioBuses !== undefined && o.audioBuses !== null) {
            const list = []
            for (let i = 0; i < o.audioBuses.length; ++i)
                list.push(Number(o.audioBuses[i]))
            return list
        }
        return Number(o.audioBus) >= 0 ? [Number(o.audioBus)] : []
    }
    function visibleAudioBusesOf(o) {
        return root.audioBusesOf(o).filter(row => root.busRoster[row] && root.busRoster[row].onBoard === true)
    }
    function videoBusesOf(o) {
        if (!o) return []
        if (o.videoBuses !== undefined && o.videoBuses !== null) {
            const list = []
            for (let i = 0; i < o.videoBuses.length; ++i)
                list.push(Number(o.videoBuses[i]))
            return list
        }
        return Number(o.videoBus) >= 0 ? [Number(o.videoBus)] : []
    }
    function screenFeedsOf(o) {
        if (!root.screensShown || !o || o.screenFeeds === undefined || o.screenFeeds === null) return []
        const list = []
        for (let i = 0; i < o.screenFeeds.length; ++i)
            list.push(Number(o.screenFeeds[i]))
        return list
    }
    function styleNameFor(id) {
        const at = root.styleById(id)
        return at >= 0 ? String(root.styleRoster[at].name) : ""
    }
    function styleScreenCount(index) {
        const id = root.styleRoster[index] ? String(root.styleRoster[index].id) : ""
        if (id.length === 0) return 0
        let n = 0
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i)
            if (!rows[i].system && String(rows[i].styleId) === id) n++
        return n
    }

    function wireColor(kind) {
        if (kind === "audio") return Theme.wireAudio
        // A gate wire is a LOOK decision (may this output paint the overlay /
        // media layer), so it wears the style colour rather than a signal's.
        if (kind === "style" || kind === "gate") return Theme.wireStyle
        return Theme.wireVideo
    }

    // ---- IS DATA REALLY MOVING THROUGH THIS CARD? ---------------------------------------
    function cardLiveState(key) {
        void root.modelsRev
        void LiveOutputService.inputRev
        void LiveOutputService.feedHealthRev
        const k = String(key || "")
        if (k.indexOf("bus:") === 0) {
            const row = root.busRowOf(k)
            if (root.busSourceCount(row) === 0 && root.busOutputCount(row) === 0)
                return -1   // an empty bus has nothing to be idle about
            const mix = root.busChannelMix(row)
            const sound = Math.max(mix[0], mix[1]) > 0.01
            const picture = root.busHasVideo(row) && root.busVideoValue(row) > 0
            return (sound || picture) ? 1 : 0
        }
        if (k === "gate:overlay")
            return (LiveOutputService.activeOverlays || []).length > 0 ? 1 : 0
        if (k === "gate:media")
            return LiveOutputService.mediaOnAir === true ? 1 : 0
        if (k.indexOf("style:") === 0) {
            const slide = LiveOutputService.onAirSlide || {}
            const textOnAir = LiveOutputService.live === true && !!(slide.blocks && slide.blocks.length > 0)
                              && LiveOutputService.blackedOut !== true
            if (!textOnAir || root.styleScreenCount(root.styleRowOf(k)) === 0)
                return 0
            // An output wearing this style has to be receiving frames for the text to arrive.
            const rows = root.outputRows
            const style = root.styleRoster[root.styleRowOf(k)]
            for (let i = 0; i < rows.length; ++i)
                if (style && rows[i].styleId === style.styleId && rows[i].feedLive === true)
                    return 1
            return 0
        }
        if (k.indexOf("out:") === 0) {
            const o = root.outputByRow(Number(k.substring(4)))
            if (!o)
                return -1
            if (o.system) {
                const buses = root.audioBusesOf(o)
                if (buses.length === 0)
                    return 0
                for (let i = 0; i < buses.length; ++i) {
                    const mix = root.busChannelMix(buses[i])
                    if (Math.max(mix[0], mix[1]) > 0.01)
                        return 1
                }
                return 0
            }
            return o.feedLive === true ? 1 : 0
        }
        return -1
    }

    // ---- How fast a wire's data moves ---------------------------------------------
    readonly property real kUncompressedKbps: 1536   // 48 kHz x 2 channels x 16 bit
    function outputFps(row) {
        if (row < 0 || row >= OutputListModel.rowCount()) return 60
        const o = OutputListModel.getOutput(row)
        const kind = String(o.kind || "").toUpperCase()
        if ((kind === "REC" || kind === "STREAM") && String(o.presetId || "") !== "") {
            const preset = PresetListModel.getPreset(PresetListModel.rowOfId(String(o.presetId)))
            const rate = parseInt(String((preset.config || {}).frameRate || ""))
            if (rate > 0) return rate
        }
        const hz = parseInt(String(o.refresh || ""))
        return hz > 0 ? hz : 60
    }
    function outputAudioKbps(row) {
        if (row < 0 || row >= OutputListModel.rowCount()) return root.kUncompressedKbps
        const o = OutputListModel.getOutput(row)
        if (String(o.presetId || "") !== "") {
            const preset = PresetListModel.getPreset(PresetListModel.rowOfId(String(o.presetId)))
            const cfg = preset.config || {}
            if (preset.kind === "audio" && cfg.format === "WAV") return root.kUncompressedKbps
            const kbps = parseInt(String(cfg.audioBitrate || ""))
            if (kbps > 0) return kbps
        }
        return root.kUncompressedKbps   // a screen or sender gets the mix as it is
    }
    function wireCycleMs(kind, toKey) {
        const key = String(toKey || "")
        const row = key.indexOf("out:") === 0 ? Number(key.substring(4)) : -1
        if (kind === "audio") {
            const kbps = root.outputAudioKbps(row)
            const factor = Math.log(kbps / 32 + 1) / Math.log(192 / 32 + 1)
            return Math.round(Math.max(300, Math.min(1800, 900 / Math.max(0.3, factor))))
        }
        const fps = root.outputFps(row)
        return Math.round(Math.max(250, Math.min(2800, 700 * 60 / Math.max(1, fps))))
    }

    // ---- Wires, derived from the engine's assignments ------------------------
    readonly property var wires: {
        root.modelsRev
        const out = []
        const rows = root.outputRows

        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            const to = root.keyForOutput(Number(o.row))
            const videos = root.videoBusesOf(o)
            const shown = Number(o.videoBus)
            for (let k = 0; k < videos.length; ++k) {
                const a = root.portPoint(root.keyForBus(videos[k]), "video", "out")
                const b = root.portPoint(to, "video", "in")
                if (a !== null && b !== null)
                    out.push({ from: root.keyForBus(videos[k]), to: to, kind: "video",
                               onAir: videos[k] === shown,
                               x1: a.x, y1: a.y, x2: b.x, y2: b.y })
            }
            const feeds = root.screenFeedsOf(o)
            for (let k = 0; k < feeds.length; ++k) {
                const a = root.portPoint(root.keyForOutput(feeds[k]), "video", "out")
                const b = root.portPoint(to, "video", "in")
                if (a !== null && b !== null)
                    out.push({ from: root.keyForOutput(feeds[k]), to: to, kind: "video",
                               x1: a.x, y1: a.y, x2: b.x, y2: b.y })
            }
            const audioBuses = root.audioBusesOf(o)
            for (let k = 0; k < audioBuses.length; ++k) {
                const a = root.portPoint(root.keyForBus(audioBuses[k]), "audio", "out")
                const b = root.portPoint(to, "audio", "in")
                if (a !== null && b !== null)
                    out.push({ from: root.keyForBus(audioBuses[k]), to: to, kind: "audio",
                               x1: a.x, y1: a.y, x2: b.x, y2: b.y })
            }
        }

        // style → screen, from the screen's own style id. Drawn left to right
        // like every other wire: styles are the column just LEFT of the screens.
        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            if (o.system || !o.styleId) continue
            const at = root.styleById(o.styleId)
            if (at < 0) continue
            const from = root.keyForStyle(at)
            const to = root.keyForOutput(Number(o.row))
            const a = root.portPoint(from, "style", "out")
            const b = root.portPoint(to, "style", "in")
            if (a === null || b === null) continue
            out.push({ from: from, to: to, kind: "style",
                       x1: a.x, y1: a.y, x2: b.x, y2: b.y })
        }

        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            if (o.system) continue
            const to = root.keyForOutput(Number(o.row))
            for (let g = 0; g < root.gateCards.length; ++g) {
                const gate = root.gateCards[g].key
                const open = gate === "overlay" ? o.showOverlay === true
                                                : o.showMedia === true
                if (!open) continue
                const from = root.keyForGate(g)
                const a = root.portPoint(from, "gate", "out")
                const b = root.portPoint(to, "style", "in")
                if (a === null || b === null) continue
                out.push({ from: from, to: to, kind: "gate",
                           x1: a.x, y1: a.y, x2: b.x,
                           y2: b.y + (gate === "overlay" ? -6 : 6) })
            }
        }
        return out
    }

    function wiresFrom(key, kind) {
        const out = []
        for (let i = 0; i < root.wires.length; ++i) {
            const w = root.wires[i]
            if (w.from === key && (kind === undefined || w.kind === kind)) out.push(w)
        }
        return out
    }
    function wiresInto(key, kind) {
        const out = []
        for (let i = 0; i < root.wires.length; ++i) {
            const w = root.wires[i]
            if (w.to === key && (kind === undefined || w.kind === kind)) out.push(w)
        }
        return out
    }
    function portConnected(key, side) {
        return side === "out" ? root.wiresFrom(key).length > 0
                              : root.wiresInto(key).length > 0
    }

    // ---- The two GATES: Overlays and Media ------------------------------------
    readonly property var gateCards: [
        { key: "overlay", title: qsTr("Overlays"), layer: qsTr("Overlay layer") },
        { key: "media", title: qsTr("Media"), layer: qsTr("Media layer") }
    ]
    // The output rows a gate wire reaches — read from the rows, because the flag on
    // the row IS the wire (see OutputListModel's OutputItem::showOverlay).
    function gateAllowedRows(gate) {
        const out = []
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            if (o.system)
                continue
            const on = gate === "overlay" ? o.showOverlay === true : o.showMedia === true
            if (on)
                out.push(Number(o.row))
        }
        return out
    }
    // The card's own summary: how many outputs are allowed, never a verdict about
    // which style is on air (the gates are per OUTPUT now, see above).
    function gateCount(gate) { return root.gateAllowedRows(gate).length }
    function gateSummary(gate) {
        const n = root.gateCount(gate)
        return n === 0 ? qsTr("off")
             : n === 1 ? qsTr("1 output")
             : qsTr("%1 outputs").arg(n)
    }

    // ---- THE OVERLAYS / MEDIA GATES ARE PER OUTPUT ---------------------------

    // ---- Ports: ONE list, so a card can never be missing a dot it should have
    // or show one for a signal it cannot carry. The bus/port rule lives here,
    // beside the cards it describes.
    readonly property var ports: {
        root.modelsRev
        const list = []
        const add = function (key, kind, side) {
            const p = root.portPoint(key, kind, side)
            if (p === null) return
            list.push({
                key: key,
                kind: kind,
                side: side,
                x: p.x,
                y: p.y,
                color: root.wireColor(kind),
                // A dot lights once its own half is wired — which is what makes
                // "the video arrives but the audio does not" visible at a glance.
                connected: root.portConnected(key, side)
            })
        }

        for (let i = 0; i < root.busRoster.length; ++i) {
            const key = root.keyForBus(i)
            if (root.busHasVideo(i)) add(key, "video", "out")
            if (root.busHasAudio(i)) add(key, "audio", "out")
        }
        for (let i = 0; i < root.screenRows.length; ++i) {
            const key = root.keyForOutput(Number(root.screenRows[i].row))
            add(key, "video", "in")
            add(key, "style", "in")
            add(key, "video", "out")
        }
        for (let i = 0; i < root.sinkRows.length; ++i) {
            const o = root.sinkRows[i]
            const key = root.keyForOutput(Number(o.row))
            if (o.acceptsVideo) add(key, "video", "in")
            if (o.acceptsAudio) add(key, "audio", "in")
        }
        // A style SENDS its look out of its right edge, into the screen beside it.
        for (let i = 0; i < root.styleRoster.length; ++i)
            add(root.keyForStyle(i), "style", "out")
        // ...and so does each GATE card (Overlays, Media): one dot, on the
        // centreline, because a permission is one thing — not a pair.
        for (let i = 0; i < root.gateCards.length; ++i)
            add(root.keyForGate(i), "gate", "out")
        return list
    }

    // ---- Hover / drag state --------------------------------------------------
    property int hoveredWire: -1
    property string hoveredNode: ""
    property bool dragging: false
    property string dragFrom: ""
    // The signal the live drag started on. A drag is typed from its first pixel:
    // the dot it left decides what it may land on.
    property string dragKind: ""
    property string dropKind: ""
    property real dragStartX: 0
    property real dragStartY: 0
    property real dragX: 0
    property real dragY: 0
    property string dropKey: ""
    readonly property bool dropOk: root.dragging && root.dropKey.length > 0
        && root.dropKind.length > 0
    readonly property string activeHoverKey: root.dragging ? root.dragFrom : root.hoveredNode

    function setHoveredNode(key, hovered) {
        if (hovered) root.hoveredNode = key
        else if (root.hoveredNode === key) root.hoveredNode = ""
    }

    function rostersChanged() {
        root.modelsRev++
    }

    // ---- Live metering: what the AUDIO halves read --------------------------
    readonly property var meterTargets: {
        root.modelsRev
        const out = []
        const add = (label) => {
            const l = label === undefined || label === null ? "" : String(label).trim()
            if (l === "" || out.indexOf(l) >= 0)
                return
            out.push(l)
        }
        for (let b = 0; b < root.busRoster.length; ++b) {
            if (!root.busHasAudio(b) || !root.busHalfWired(b, "audio"))
                continue
            const routed = root.busRoster[b].routedAudioInputs || []
            for (let i = 0; i < routed.length; ++i) {
                const data = AudioInputListModel.getInput(routed[i])
                if (data && data.sublabel !== undefined)
                    add(String(data.sublabel))
            }
        }
        return out
    }
    // ---- The OTHER carrier: a video row's own audio --------------------------
    readonly property var streamMeterTargets: {
        root.modelsRev
        const out = []
        const add = (label, kind) => {
            const l = label === undefined || label === null ? "" : String(label).trim()
            if (l === "" || (kind !== "media" && kind !== "ndi"))
                return
            for (let i = 0; i < out.length; ++i)
                if (out[i].label === l)
                    return
            out.push({ label: l, kind: kind })
        }
        for (let b = 0; b < root.busRoster.length; ++b) {
            if (!root.busHasAudio(b) || !root.busHalfWired(b, "audio"))
                continue
            const routed = root.busRoster[b].routedVideoSources || []
            for (let i = 0; i < routed.length; ++i) {
                const data = VideoSourceListModel.getSource(routed[i])
                if (data)
                    add(data.sublabel, data.kind)
            }
        }
        return out
    }
    property var heldMeters: []
    property var heldStreamMeters: []
    function syncMeters() {
        if (!root)
            return
        const wanted = root.meterTargets
        const held = root.heldMeters
        for (const label of held)
            if (wanted.indexOf(label) < 0)
                EngineBridge.stopInputMeter(label)
        for (const label of wanted)
            if (held.indexOf(label) < 0)
                EngineBridge.startInputMeter(label)
        root.heldMeters = wanted

        const wantStreams = root.streamMeterTargets
        const heldStreams = root.heldStreamMeters
        const has = (list, t) => list.some(x => x.label === t.label && x.kind === t.kind)
        for (const t of heldStreams)
            if (!has(wantStreams, t))
                EngineBridge.stopStreamMeter(t.label)
        for (const t of wantStreams)
            if (!has(heldStreams, t))
                EngineBridge.startStreamMeter(t.label, t.kind, t.label)
        root.heldStreamMeters = wantStreams
    }
    onMeterTargetsChanged: root.syncMeters()
    onStreamMeterTargetsChanged: root.syncMeters()

    Component.onCompleted: {
        RoutingService.refresh()
        root.syncMeters()
    }
    // Only this page's own holds are released — a trip through Routing must not
    // silence a meter the Media pane or Audio & Video is still showing.
    Component.onDestruction: {
        // The add lists live in the window overlay: close them with the page.
        if (busAddMenu)
            busAddMenu.visible = false
        if (styleAddMenu)
            styleAddMenu.visible = false
        for (const label of root.heldMeters)
            EngineBridge.stopInputMeter(label)
        root.heldMeters = []
        for (const t of root.heldStreamMeters)
            EngineBridge.stopStreamMeter(t.label)
        root.heldStreamMeters = []
    }

    function pairOk(aKey, aKind, bKey) {
        const ca = root.columnOf(aKey)
        const cb = root.columnOf(bKey)
        if (ca < 0 || cb < 0 || ca === cb) return false
        const fromCol = Math.min(ca, cb)
        const toCol = Math.max(ca, cb)
        const fromKey = ca < cb ? aKey : bKey
        const toKey = ca < cb ? bKey : aKey

        if (aKind === "style")
            return fromCol === 1 && toCol >= 2 && !root.isSystem(toKey)

        if (fromCol === 2 && toCol === 3) {
            if (aKind !== "video") return false
            const sink = Number(toKey.substring(4))
            return sink !== Number(fromKey.substring(4)) && root.outputAccepts(sink, "video")
        }

        if (aKind === "gate" || fromKey.indexOf("gate:") === 0) {
            if (fromCol !== 1 || toCol < 2) return false
            if (toKey.indexOf("out:") !== 0) return false
            return !root.isSystem(toKey)
        }

        // A signal wire is always a bus into a screen or an output, one type at
        // a time — the bus must carry it and the destination must take it.
        if (fromCol !== 0) return false
        if (toCol !== 2 && toCol !== 3) return false
        const bus = root.rowOf(fromKey)
        const row = Number(toKey.substring(4))
        if (aKind === "video") return root.busHasVideo(bus) && root.outputAccepts(row, "video")
        if (aKind === "audio") return root.busHasAudio(bus) && root.outputAccepts(row, "audio")
        return false
    }

    // ---- Connect gestures ----------------------------------------------------
    function resolveKind(fromKey, kind, toKey) {
        if (kind === "" || toKey === "") return ""
        if (root.pairOk(fromKey, kind, toKey)) return kind
        const other = kind === "audio" ? "video" : kind === "video" ? "audio" : ""
        if (other !== "" && root.pairOk(fromKey, other, toKey)) return other
        return ""
    }
    function canvasPoint(sceneX, sceneY) {
        const p = board.mapFromItem(null, sceneX, sceneY)
        return { x: p.x, y: p.y }
    }

    function beginDrag(key, kind, sceneX, sceneY) {
        const p = root.canvasPoint(sceneX, sceneY)
        root.dragging = true
        root.dragFrom = key
        root.dragKind = kind
        root.dragStartX = p.x
        root.dragStartY = p.y
        root.dragX = p.x
        root.dragY = p.y
        root.dropKey = ""
        root.dropKind = ""
        root.hoveredWire = -1
    }

    function moveDrag(key, kind, sceneX, sceneY) {
        if (!root.dragging || key !== root.dragFrom || kind !== root.dragKind) return
        const p = root.canvasPoint(sceneX, sceneY)
        root.dragX = p.x
        root.dragY = p.y
        const over = root.nodeAt(p.x, p.y)
        root.dropKey = over === root.dragFrom ? "" : over
        root.dropKind = root.dropKey.length > 0
                        ? root.resolveKind(root.dragFrom, root.dragKind, root.dropKey) : ""
    }

    function endDrag(key, kind, sceneX, sceneY) {
        if (!root.dragging || key !== root.dragFrom || kind !== root.dragKind) return
        const p = root.canvasPoint(sceneX, sceneY)
        const over = root.nodeAt(p.x, p.y)
        // Read the half BEFORE the state is cleared: it is what the drop writes.
        const wireKind = (over.length > 0 && over !== key)
                         ? root.resolveKind(key, kind, over) : ""
        root.dragging = false
        root.dragFrom = ""
        root.dragKind = ""
        root.dropKey = ""
        root.dropKind = ""
        // ONE LINE PER DROP, so a wire that "will not connect" says where it stopped:
        // nothing under the pointer, a pair the canvas refuses, or a write that failed.
        if (typeof EngineBridge !== "undefined")
            EngineBridge.log("info", "RoutingWire",
                             "drop from=" + key + " dot=" + kind + " over='" + over
                             + "' resolved='" + wireKind + "'"
                             + (wireKind.length > 0 ? "" : " (refused: no card under the pointer, or it cannot take this signal)"))
        if (wireKind.length > 0)
            root.toggleWire(key, over, wireKind)
    }

    function nodeAt(x, y) {
        for (let g = 0; g < root.gateCards.length; ++g) {
            const left = root.columnX(1)
            const top = root.gateTop(g)
            if (x >= left - 13 && x <= left + root.colW + 13 &&
                y >= top - 8 && y <= top + root.cardH + 8)
                return root.keyForGate(g)
        }
        for (let c = 0; c < 4; ++c) {
            const list = c === 0 ? root.busRows
                       : c === 1 ? root.styleRows
                       : c === 2 ? root.screenRows : root.sinkFlat
            for (let r = 0; r < list.length; ++r) {
                const left = root.columnX(c)
                const top = c === 0 ? root.busSlotTop(r)
                          : c === 3 ? root.sinkSlotTop(r)
                          : c === 1 ? root.styleTop(r)
                                    : root.rowTop(r)
                if (x >= left - 13 && x <= left + root.colW + 13 &&
                    y >= top - 8 && y <= top + root.cardH + 8)
                    return c === 0 ? root.keyForBus(Number(list[r].row))
                         : c === 1 ? root.keyForStyle(Number(list[r].row))
                         : root.keyForOutput(Number(list[r].row))
            }
        }
        return ""
    }

    function toggleWire(a, b, kind) {
        const ca = root.columnOf(a)
        const cb = root.columnOf(b)
        const fromKey = ca < cb ? a : b
        const toKey = ca < cb ? b : a

        if (kind === "gate" || fromKey.indexOf("gate:") === 0) {
            const gate = fromKey.substring(5)
            const row = Number(toKey.substring(4))
            const o = root.outputByRow(row)
            if (o === null) return false
            const open = gate === "overlay" ? o.showOverlay === true : o.showMedia === true
            if (gate === "overlay")
                OutputListModel.setShowOverlay(row, !open)
            else
                OutputListModel.setShowMedia(row, !open)
            return true
        }

        if (kind === "style") {
            const outKey = root.columnOf(a) === 1 ? b : a
            const styleKey = outKey === a ? b : a
            const row = Number(outKey.substring(4))
            const style = root.styleRoster[root.styleRowOf(styleKey)]
            const o = root.outputByRow(row)
            if (!style || o === null) return false
            OutputListModel.setStyle(row, String(o.styleId) === String(style.id) ? "" : String(style.id))
            return true
        }

        const bus = root.busRowOf(fromKey)
        const row = Number(toKey.substring(4))
        const o = root.outputByRow(row)
        if (o === null) return false
        if (root.columnOf(fromKey) === 2) {
            // The ROSTER row, not the column index — rowOf() answers the position
            // within the Screens column, and a sink's feed is a row id.
            const screenRow = Number(fromKey.substring(4))
            const feeds = root.screenFeedsOf(o)
            RoutingService.linkScreen(screenRow, Number(toKey.substring(4)),
                                      feeds.indexOf(screenRow) < 0)
            return true
        }

        const wired = kind === "audio" ? root.audioBusesOf(o).indexOf(bus) >= 0
                                       : root.videoBusesOf(o).indexOf(bus) >= 0
        if (wired) RoutingService.unwireBus(row, kind, bus)
        else RoutingService.assignBus(row, kind, bus)
        return true
    }

    // ---- Wire hit-testing: one layer for every wire, never a handler on a wire
    // delegate (the Audio & Video board's header records the crash that pattern
    // caused when a roster change destroyed the delegate under the cursor).
    function cubicAt(a, b, c, d, t) {
        const u = 1 - t
        return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d
    }
    function wireAt(x, y) {
        const list = root.wires
        for (let i = 0; i < list.length; ++i) {
            const w = list[i]
            const reversed = w.x2 < w.x1
            const x1 = reversed ? w.x2 : w.x1
            const x2 = reversed ? w.x1 : w.x2
            const y1 = reversed ? w.y2 : w.y1
            const y2 = reversed ? w.y1 : w.y2
            const dx = Math.max(24, Math.abs(x2 - x1) * 0.5)
            for (let s = 0; s <= 24; ++s) {
                const t = s / 24
                const px = root.cubicAt(x1, x1 + dx, x2 - dx, x2, t)
                const py = root.cubicAt(y1, y1, y2, y2, t)
                if (Math.abs(px - x) <= 7 && Math.abs(py - y) <= 7) return i
            }
        }
        return -1
    }
    function showWireAt(x, y) {
        const at = root.wireAt(x, y)
        if (at < 0) return
        const w = root.wires[at]
        if (!w || w.kind !== "video" || root.columnOf(w.from) !== 0) return
        if (w.onAir === true) return          // already the picture going out
        RoutingService.showBusOn(Number(w.to.substring(4)), root.busRowOf(w.from))
    }
    function removeWireAt(x, y) {
        const at = root.wireAt(x, y)
        if (at < 0) return
        const w = root.wires[at]
        // One wire = one side of one assignment. The other half of a both-bus
        // pair stays exactly as it was.
        if (w.kind === "gate") {
            // Cutting a gate wire takes the permission away for that output alone.
            const gate = w.from.substring(5)
            const row = Number(w.to.substring(4))
            if (gate === "overlay")
                OutputListModel.setShowOverlay(row, false)
            else
                OutputListModel.setShowMedia(row, false)
        } else if (w.kind === "style") {
            // A style wire's FROM end is the style; the screen is where it lands.
            OutputListModel.setStyle(Number(w.to.substring(4)), "")
        } else if (root.columnOf(w.from) === 2) {
            // Screen → sink: the two ends are roster rows, and the edge is the wire.
            RoutingService.linkScreen(Number(w.from.substring(4)),
                                      Number(w.to.substring(4)), false)
        } else {
            // The wire names its bus at the FROM end — required on the speaker
            // endpoint, harmless on a roster row (one bus per side there).
            RoutingService.unwireBus(Number(w.to.substring(4)), w.kind, root.busRowOf(w.from))
        }
        root.hoveredWire = -1
    }

    // ---- Labels --------------------------------------------------------------
    function typeLabel(kind) {
        if (kind === "audio") return qsTr("Audio")
        if (kind === "video") return qsTr("Video")
        // A bus made from a template: its windows are filled by the video
        // sources routed into it, and it takes one sound.
        if (kind === "template") return qsTr("Template")
        return qsTr("Audio + Video")
    }
    function typeBadge(kind) {
        if (kind === "audio") return "A"
        if (kind === "video") return "V"
        if (kind === "template") return "TPL"
        return "A+V"
    }
    function acceptsLabel(o) {
        if (o.system) return qsTr("Audio")
        if (o.acceptsAudio && o.acceptsVideo) return qsTr("Audio + Video")
        return o.acceptsAudio ? qsTr("Audio") : qsTr("Video")
    }
    // A sink's second line names its TRANSPORT and what that transport carries —
    // the two facts that decide what can be wired to it.
    function sinkSubtitle(o) {
        if (o.system) return qsTr("System audio")
        const kind = String(o.kind || "")
        // A recording or a stream is its PRESET: the card says which settings it will
        // use, or that it has none yet — never a bare transport name.
        if (kind.toUpperCase() === "REC" || kind.toUpperCase() === "STREAM") {
            void root.modelsRev
            const summary = PresetListModel.summaryOf(String(o.presetId || ""))
            return summary.length > 0 ? summary : qsTr("No preset — pick one in Configure output")
        }
        const accepts = root.acceptsLabel(o)
        return kind.length > 0 ? kind + " · " + accepts : accepts
    }
    // The glyph each transport gets: a sink that reads "link" for NDI, SDI,
    // recording AND streaming looks like four of the same thing.
    function sinkIcon(o) {
        if (o.system) return "volume2"
        const kind = String(o.kind || "").toUpperCase()
        if (kind === "NDI") return "link"
        if (kind === "SDI") return "layers-intersect"
        if (kind === "HDMI") return "presentation"
        if (kind === "REC") return PresetListModel.kindOf(String(o.presetId || "")) === "audio" ? "volume2" : "fileText"
        if (kind === "STREAM") return "activity"
        return "link"
    }

    // ---- A sink's own transport ---------------------------------------------
    function sinkNodeId(o) {
        if (!o || o.system) return ""
        return o.id ? "out:" + String(o.id) : ""
    }
    // The node the LIVE take belongs to — "" while idle.
    readonly property string takeNode: RecordingService.recording
                                       && RecordingService.recordingNode.length > 0
                                       ? RecordingService.recordingNode : ""
    function sinkActionLabel(o) {
        if (!o || o.system) return ""
        const kind = String(o.kind || "").toUpperCase()
        if (kind !== "REC") return ""
        const node = root.sinkNodeId(o)
        if (node.length === 0) return ""
        if (root.takeNode === node) return qsTr("Stop")
        return qsTr("Record")
    }
    function transportSink(o) {
        const node = root.sinkNodeId(o)
        if (node.length === 0) return
        if (root.takeNode === node) {
            RecordingService.stopRecording()
            return
        }
        const presetId = String(o.presetId || "")
        const presetKind = PresetListModel.kindOf(presetId)
        if (presetKind !== "recording" && presetKind !== "audio") {
            EventBus.notify(qsTr("“%1” has no recording preset yet — pick one in Configure output (right-click the card).").arg(String(o.name || "")),
                            "warning", qsTr("Routing"), "routing.transport.nopreset")
            return
        }
        if (RecordingService.recording) {
            // ONE engine session at a time: say so rather than letting a second
            // Start replace the profile under the take that is already running.
            EventBus.notify(qsTr("A recording is already running on another output — stop that one first."),
                            "warning", qsTr("Routing"), "routing.transport.busy")
            return
        }
        RecordingService.startRecordingOn(node, PresetListModel.configOf(presetId))
    }

    // ---- What each WIRED HALF is doing --------------------------------------
    readonly property int stripW: 30
    readonly property int stripH: 4
    readonly property int pillSize: 16
    // Wide enough for the 9 px dot to sit between the two pills with a hair of
    // clearance on each side, so the dot stays visible and grabbable.
    readonly property int pillGap: 12
    readonly property int pillPairHeight: root.pillSize * 2 + root.pillGap

    readonly property var inputMeters: EngineBridge.inputLevels
    function meterSnapshotFor(label) {
        const want = String(label === undefined || label === null ? "" : label).trim()
        if (want === "")
            return null
        const list = root.inputMeters
        for (let i = 0; i < list.length; ++i)
            if (String(list[i].label || "").trim() === want)
                return list[i]
        return null
    }
    function busChannelMix(busRow) {
        const bus = root.busRoster[busRow]
        if (!bus)
            return [0, 0]
        if (bus.muted === true)
            return [0, 0]
        const mix = [0, 0]
        const routed = bus.routedAudioInputs || []
        for (let r = 0; r < routed.length; ++r) {
            const data = AudioInputListModel.getInput(routed[r])
            if (!data || data.muted)
                continue
            const snap = root.meterSnapshotFor(data.sublabel)
            if (!snap || !snap.peaks || snap.peaks.length === 0)
                continue
            const fader = Math.max(0, Math.min(1, Number(data.level || 0) / 100))
            for (let c = 0; c < 2; ++c) {
                // A MONO tap has one peak for both sides, exactly as the pane
                // reads it — not "left only".
                const channel = snap.peaks.length === 1 ? 0 : c
                if (channel >= snap.peaks.length)
                    continue
                const gain = AudioInputListModel.channelGain(routed[r], channel)
                const level = Math.max(0, Math.min(1, Number(snap.peaks[channel]) || 0)) * gain * fader
                if (level > mix[c])
                    mix[c] = level
            }
        }
        const routedVideo = bus.routedVideoSources || []
        for (let r = 0; r < routedVideo.length; ++r) {
            const data = VideoSourceListModel.getSource(routedVideo[r])
            if (!data || data.muted)
                continue
            const kind = String(data.kind || "")
            if (kind !== "media" && kind !== "ndi")
                continue
            const snap = root.meterSnapshotFor(data.sublabel)
            if (!snap || !snap.peaks || snap.peaks.length === 0)
                continue
            const fader = Math.max(0, Math.min(1, Number(data.level || 0) / 100))
            for (let c = 0; c < 2; ++c) {
                const channel = snap.peaks.length === 1 ? 0 : c
                if (channel >= snap.peaks.length)
                    continue
                const gain = VideoSourceListModel.channelGain(routedVideo[r], channel)
                const level = Math.max(0, Math.min(1, Number(snap.peaks[channel]) || 0)) * gain * fader
                if (level > mix[c])
                    mix[c] = level
            }
        }
        const busLevel = Math.max(0, Math.min(1, Number(bus.level || 0) / 100))
        return [mix[0] * busLevel * BusListModel.channelGain(busRow, 0),
                mix[1] * busLevel * BusListModel.channelGain(busRow, 1)]
    }
    function busVideoValue(busRow) {
        void LiveOutputService.inputRev
        const bus = root.busRoster[busRow]
        if (!bus)
            return 0
        const routed = bus.routedVideoSources || []
        if (routed.length === 0)
            return 0
        // The FIRST routed source is the one a bus thumbnail shows, and the same
        // choice here keeps the strip and the picture talking about one feed.
        const data = VideoSourceListModel.getSource(routed[0])
        if (!data || data.sublabel === undefined)
            return 0
        const label = String(data.sublabel).trim()
        if (label === "")
            return 0
        return LiveOutputService.inputDelivery(label, String(data.kind || "")) === "connected" ? 100 : 0
    }
    function sinkVideoValue(rosterRow) {
        void LiveOutputService.feedHealthRev
        const o = root.outputByRow(rosterRow)
        if (!o || o.system)
            return 0
        const name = String(o.name || "")
        if (name === "")
            return 0
        if (LiveOutputService.outputFeedStalled(name))
            return 0
        const runtime = LiveOutputService.outputRuntime[name]
        return (runtime && runtime.presented === true) ? 100 : 0
    }
    function stripValue(strip) {
        if (!strip)
            return 0
        return strip.target === "bus" ? root.busVideoValue(strip.row)
                                      : root.sinkVideoValue(strip.row)
    }
    // Is this half of a bus carrying anything onward? A bus half nobody wired
    // onward gets no strip: there is nothing to watch.
    function busHalfWired(busRow, kind) {
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            // The speaker endpoint counts: a bus wired only to the speakers is
            // carrying something onward, and its pair has to show it.
            if (kind === "audio" ? root.audioBusesOf(o).indexOf(busRow) >= 0
                                 : root.videoBusesOf(o).indexOf(busRow) >= 0)
                return true
        }
        return false
    }

    readonly property var signalStrips: {
        root.modelsRev
        const out = []

        // No video level strips: no card draws one under its video dot any more.
        return out
    }

    readonly property var audioPills: {
        root.modelsRev
        const out = []
        const gap = 8

        // BOARD order: a pill sits beside the card that is drawn, and the card's
        // y is its SLOT's (see busSlotTop) while its DATA is its roster row.
        for (let i = 0; i < root.busRows.length; ++i) {
            const row = Number(root.busRows[i].row)
            if (!root.busHasAudio(row) || !root.busHalfWired(row, "audio"))
                continue
            const key = root.keyForBus(row)
            const dotY = root.busSlotTop(i) + root.cardH / 2 + root.portOffset(key, "audio")
            out.push({ target: "bus", bus: row,
                       x: root.columnX(0) + root.colW + gap,
                       y: dotY - root.pillPairHeight / 2 })
        }

        for (let c = 2; c < 4; ++c) {
            const list = c === 2 ? root.screenRows : root.sinkFlat
            for (let i = 0; i < list.length; ++i) {
                const o = list[i]
                if (!o.acceptsAudio)
                    continue
                const buses = root.visibleAudioBusesOf(o)
                if (buses.length === 0)
                    continue
                const top = c === 3 ? root.sinkSlotTop(i) : root.rowTop(i)
                const key = root.keyForOutput(Number(o.row))
                const dotY = top + root.cardH / 2 + root.portOffset(key, "audio")
                out.push({ target: "sink", bus: buses[0], buses: buses,
                           x: root.columnX(c) - gap - root.pillSize,
                           y: dotY - root.pillPairHeight / 2 })
            }
        }
        return out
    }
    function pillBuses(pair) {
        if (!pair) return []
        return pair.buses !== undefined && pair.buses !== null ? pair.buses : [Number(pair.bus)]
    }
    // A side is OPEN when it is open somewhere: closing the left of the speakers
    // must not read as closed while a bus feeding it still passes its left.
    function pillGain(pair, channel) {
        const buses = root.pillBuses(pair)
        let gain = 0
        for (let i = 0; i < buses.length; ++i)
            gain = Math.max(gain, BusListModel.channelGain(buses[i], channel))
        return gain
    }
    function pillLevel(pair, channel) {
        const buses = root.pillBuses(pair)
        let mix = 0
        for (let i = 0; i < buses.length; ++i) {
            const levels = root.busChannelMix(buses[i])
            mix = Math.max(mix, Number(levels[channel]) || 0)
        }
        // Shaped the way every live meter in the app shapes it (sqrt of the raw
        // mix): a near-silent source still reads as a glow instead of vanishing.
        return Math.sqrt(Math.max(0, Math.min(1, mix)))
    }
    function togglePill(pair, channel) {
        const buses = root.pillBuses(pair)
        const open = root.pillGain(pair, channel) > 0
        for (let i = 0; i < buses.length; ++i) {
            const gain = BusListModel.channelGain(buses[i], channel)
            if (open ? gain > 0 : gain <= 0)
                BusListModel.toggleChannelGain(buses[i], channel)
        }
    }
    function receivesLabel(o) {
        // A screen feeding this sink is video arriving, exactly like a video bus.
        const videoBuses = root.videoBusesOf(o)
        const video = videoBuses.length > 0 || root.screenFeedsOf(o).length > 0
        const buses = root.visibleAudioBusesOf(o)
        const audio = buses.length > 0
        if (o.acceptsVideo && !o.acceptsAudio) return video ? qsTr("video in") : qsTr("silent")
        // An audio-only sink can carry SEVERAL buses (the speaker endpoint sums
        // them), so it says how many instead of pretending to hold one.
        if (o.acceptsAudio && !o.acceptsVideo)
            return audio ? (buses.length > 1 ? qsTr("%1 buses in").arg(buses.length)
                                             : qsTr("audio in"))
                         : qsTr("silent")
        if (video && audio) return qsTr("both in")
        if (video) return qsTr("video only")
        if (audio) return qsTr("audio only")
        return qsTr("silent")
    }
    function layoutClassOf(key) { return ScreenHelpers.layoutClassOf(key) }
    function styleSubtitle(style) {
        const keys = [style.familyTemplateShows, style.familyTemplateMedia,
                      style.familyTemplateScripture, style.familyTemplateTable]
                     .filter(k => typeof k === "string" && k !== "")
        if (keys.length === 0)
            keys.push(String(style.templateKey || ""))
        const kinds = new Set(keys.map(root.layoutClassOf).filter(c => c !== ""))
        if (kinds.size > 1)
            return qsTr("Mixed")
        if (kinds.size === 1)
            return kinds.has("lower") ? qsTr("Lower Third") : qsTr("Full Screen")
        const kind = String(style.contentType || "")
        return kind.length > 0 ? kind : qsTr("Style")
    }
    function styleSwatch(style) {
        const colour = String(style.backgroundColor || "")
        return colour.length > 0 ? colour : "transparent"
    }
    function screenStatus(o) {
        if (!o.wired) return qsTr("No signal")
        if (o.feedLive !== true) return qsTr("no frames")
        const style = root.styleNameFor(o.styleId)
        return style.length > 0 ? style : qsTr("live")
    }
    function screenStatusColor(o) {
        if (!o.wired) return Theme.warningLight
        return o.feedLive === true ? Theme.textSecondary : Theme.warningLight
    }
    function toggleCardGate(key, gate) {
        const row = Number(key.substring(key.indexOf(":") + 1))
        const o = root.outputByRow(row)
        if (!o)
            return
        if (gate === "overlay")
            OutputListModel.setShowOverlay(row, o.showOverlay === false)
        else
            OutputListModel.setShowMedia(row, o.showMedia === false)
    }
    // A sink's status colour, same rule: delivered frames are ordinary, a wired
    // sink with none is the warning.
    function sinkStatusColor(o) {
        const wired = o.wired || root.videoBusesOf(o).length > 0
                      || root.audioBusesOf(o).length > 0
                      || root.screenFeedsOf(o).length > 0
        if (!wired) return Theme.textMuted
        if (o.system) return Theme.textSecondary      // no frame of its own to wait on
        return o.feedLive === true ? Theme.textSecondary : Theme.warningLight
    }

    // =====================================================================
    // Header
    // =====================================================================
    Item {
        id: pageHeader
        x: Theme.space6
        y: Theme.space6
        width: parent.width - Theme.space6 * 2
        height: headerColumn.implicitHeight

        Column {
            id: headerColumn
            width: parent.width
            spacing: 3

            Item {
                width: parent.width
                height: Math.max(titleText.implicitHeight, refreshButton.height)

                Text {
                    id: titleText
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Routing")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }

                AppButton {
                    id: refreshButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Refresh")
                    variant: "ghost"
                    onClicked: RoutingService.refresh()
                }

                Row {
                    id: legend
                    anchors.right: refreshButton.left
                    anchors.rightMargin: Theme.space5
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space4

                    Repeater {
                        model: [
                            { label: qsTr("Audio"), color: Theme.wireAudio, dashed: false },
                            { label: qsTr("Video"), color: Theme.wireVideo, dashed: false },
                            { label: qsTr("Style"), color: Theme.wireStyle, dashed: true }
                        ]
                        delegate: Row {
                            required property var modelData
                            spacing: 6

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Rectangle {
                                    width: 5
                                    height: 3
                                    radius: 1.5
                                    color: modelData.color
                                    visible: modelData.dashed
                                }
                                Rectangle {
                                    width: modelData.dashed ? 5 : 16
                                    height: 3
                                    radius: 1.5
                                    color: modelData.color
                                }
                                Rectangle {
                                    width: 5
                                    height: 3
                                    radius: 1.5
                                    color: modelData.color
                                    visible: modelData.dashed
                                }
                            }

                            Text {
                                text: modelData.label
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSub
                            }
                        }
                    }
                }
            }
        }
    }

    // =====================================================================
    // Canvas
    // =====================================================================
    Rectangle {
        id: canvas
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: pageHeader.bottom
        anchors.topMargin: Theme.space3
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6
        anchors.leftMargin: Theme.space6
        anchors.rightMargin: Theme.space6 + Theme.space2
        radius: Theme.radiusLg
        color: Theme.windowBg
        border.color: Theme.border
        border.width: 1
        clip: true

        Checkerboard {
            anchors.fill: parent
            tileSize: 13
        }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 1
            anchors.topMargin: Theme.space4
            contentWidth: board.width
            contentHeight: board.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            interactive: !root.dragging

            Item {
                id: board
                width: root.boardW
                height: Math.max(root.boardH,
                                 addPicker.showing ? addPicker.y + addPicker.height + Theme.space3
                                                   : 0)

                // ---- Column headers ----
                Repeater {
                    model: [
                        { title: qsTr("BUSES"), column: 0 },
                        { title: qsTr("STYLES"), column: 1 },
                        // Shown only while screensShown is on.
                        { title: qsTr("SCREENS"), column: 2, shown: root.screensShown },
                        { title: qsTr("OUTPUTS"), column: 3 }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        visible: modelData.shown !== false
                        x: root.columnX(modelData.column)
                        y: 0
                        width: root.colW
                        height: 40
                        radius: Theme.radiusMd
                        color: Theme.inset
                        border.width: 1
                        border.color: Theme.border

                        Text {
                            anchors.centerIn: parent
                            text: modelData.title
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }

                        Item {
                            id: headerAdd
                            visible: modelData.column !== 2
                            anchors.right: parent.right
                            anchors.rightMargin: 5
                            anchors.verticalCenter: parent.verticalCenter
                            width: root.addBtnSize
                            height: root.addBtnSize

                            readonly property bool active: addArea.containsMouse
                                || (root.addMenuFor(modelData.column).length > 0
                                    && root.addMenu === root.addMenuFor(modelData.column))

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radiusSm
                                color: headerAdd.active ? Theme.surface : "transparent"
                                border.width: headerAdd.active ? 1 : 0
                                border.color: Theme.border
                                Behavior on color {
                                    ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing }
                                }
                            }

                            PlusGlyph {
                                anchors.centerIn: parent
                                size: 10
                                thickness: 1.6
                                color: headerAdd.active ? Theme.accent : Theme.textSecondary
                            }

                            MouseArea {
                                id: addArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.beginAdd(modelData.column, headerAdd.parent)
                            }
                        }
                    }
                }

                // ---- Wires ----
                Repeater {
                    model: root.wires
                    delegate: Shape {
                        id: wireShape
                        required property var modelData
                        required property int index
                        anchors.fill: parent
                        preferredRendererType: Shape.CurveRenderer

                        readonly property bool highlighted: root.hoveredWire === index
                            || (root.activeHoverKey.length > 0
                                && (modelData.from === root.activeHoverKey || modelData.to === root.activeHoverKey))
                        readonly property bool dimmed: (root.hoveredWire >= 0 || root.activeHoverKey.length > 0) && !highlighted
                        readonly property bool reversed: modelData.x2 < modelData.x1
                        readonly property real fromX: reversed ? modelData.x2 : modelData.x1
                        readonly property real fromY: reversed ? modelData.y2 : modelData.y1
                        readonly property real toX: reversed ? modelData.x1 : modelData.x2
                        readonly property real toY: reversed ? modelData.y1 : modelData.y2
                        readonly property real bend: Math.max(24, Math.abs(toX - fromX) * 0.5)
                        readonly property bool offAir: modelData.onAir === false
        // ms for the pattern to travel once: the output's frame rate (video, style) or audio bitrate.
        readonly property int cycleMs: { root.modelsRev; return root.wireCycleMs(modelData.kind, modelData.to) }

                        opacity: dimmed ? 0.16 : (offAir ? 0.5 : 1)

                        Behavior on opacity { NumberAnimation { duration: 120 } }

                        ShapePath {
                            strokeColor: root.wireColor(modelData.kind)
                            strokeWidth: highlighted ? 3.4 : 2.2
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            strokeStyle: modelData.kind === "style"
                                         || modelData.kind === "gate"
                                         || wireShape.offAir
                                         ? ShapePath.DashLine : ShapePath.SolidLine
                            dashPattern: [4, 4]
                            dashOffset: 0
                            NumberAnimation on dashOffset {
                                from: 8
                                to: 0
                                duration: wireShape.cycleMs
                                loops: Animation.Infinite
                                running: (wireShape.modelData.kind === "style"
                                          && (root.outputByRow(Number(String(wireShape.modelData.to).substring(4))) || {}).enabled !== false)
                                         || (wireShape.modelData.kind === "gate"
                                             && root.cardLiveState(wireShape.modelData.from) === 1)
                            }
                            startX: wireShape.fromX
                            startY: wireShape.fromY
                            PathCubic {
                                x: wireShape.toX
                                y: wireShape.toY
                                control1X: wireShape.fromX + wireShape.bend
                                control1Y: wireShape.fromY
                                control2X: wireShape.toX - wireShape.bend
                                control2Y: wireShape.toY
                            }
                        }

                        ShapePath {
                            id: dataFlow
                            readonly property bool isAudio: wireShape.modelData.kind === "audio"
                            // Moves only while the bus it leaves is really carrying something (a source
                            // delivering frames, or sound above silence) — measured, not assumed.
                            readonly property bool flows: (isAudio || wireShape.modelData.kind === "video")
                                                          && !wireShape.offAir
                                                          && root.cardLiveState(wireShape.modelData.from) === 1
                            // Stroke widths per pattern: dash + gap (video) or bead + gap (audio).
                            readonly property real period: isAudio ? 3.6 : 10
                            strokeColor: flows
                                         ? Qt.lighter(root.wireColor(wireShape.modelData.kind), 1.55) : "transparent"
                            strokeWidth: highlighted ? 3.4 : (isAudio ? 3 : 2.2)
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            strokeStyle: ShapePath.DashLine
                            dashPattern: isAudio ? [0.05, 3.55] : [6, 4]
                            dashOffset: 0
                            NumberAnimation on dashOffset {
                                from: dataFlow.period
                                to: 0
                                duration: wireShape.cycleMs
                                loops: Animation.Infinite
                                running: dataFlow.flows
                            }
                            startX: wireShape.fromX
                            startY: wireShape.fromY
                            PathCubic {
                                x: wireShape.toX
                                y: wireShape.toY
                                control1X: wireShape.fromX + wireShape.bend
                                control1Y: wireShape.fromY
                                control2X: wireShape.toX - wireShape.bend
                                control2Y: wireShape.toY
                            }
                        }
                    }
                }

                // ---- Wire hover + remove. One layer for every wire, above the
                // lines and below the cards, so the wire under the pointer is the
                // one the pointer answers for.
                MouseArea {
                    id: wireLayer
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    hoverEnabled: true
                    enabled: !root.dragging
                    cursorShape: root.hoveredWire >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onPositionChanged: (mouse) => { root.hoveredWire = root.wireAt(mouse.x, mouse.y) }
                    onExited: root.hoveredWire = -1
                    // Right = cut this one wire, leaving the row's other buses.
                    onClicked: (mouse) => root.removeWireAt(mouse.x, mouse.y)
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    enabled: !root.dragging
                    onTapped: (eventPoint) => root.showWireAt(eventPoint.position.x,
                                                              eventPoint.position.y)
                }

                DropdownPanel {
                    id: busAddMenu
                    visible: false
                    matchAnchorWidth: true
                    rowHeight: 32
                    model: root.addBusMenuItems
                    onItemActivated: (label, item) => root.pickAddMenuItem(0, item)
                }
                MenuCatcher { menu: busAddMenu }

                DropdownPanel {
                    id: styleAddMenu
                    visible: false
                    matchAnchorWidth: true
                    rowHeight: 32
                    model: root.addStyleMenuItems
                    onItemActivated: (label, item) => root.pickAddMenuItem(1, item)
                }
                MenuCatcher { menu: styleAddMenu }

                Connections {
                    target: busAddMenu
                    function onVisibleChanged() {
                        if (!busAddMenu.visible && root.addMenu === "bus")
                            root.addMenu = ""
                    }
                }
                Connections {
                    target: styleAddMenu
                    function onVisibleChanged() {
                        if (!styleAddMenu.visible && root.addMenu === "style")
                            root.addMenu = ""
                    }
                }

                // ---- Buses, in labelled segments --------------------------
                Repeater {
                    model: root.busGroups
                    delegate: Text {
                        required property var modelData
                        required property int index
                        x: root.columnX(0)
                        y: root.busGroupTop(index) + 2
                        width: root.colW
                        text: modelData.label.toUpperCase()
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }
                }

                Repeater {
                    model: root.busRows
                    delegate: RoutingNodeCard {
                        required property var modelData
                        required property int index
                        // `index` is the SLOT on the board; modelData.row is the
                        // ROSTER row every read and action speaks in.
                        readonly property string key: root.keyForBus(modelData.row)
                        readonly property int sources: root.busSourceCount(modelData.row)
                        readonly property int outs: root.busOutputCount(modelData.row)

                        x: root.columnX(0)
                        y: root.busSlotTop(index)
                        width: root.colW
                        height: root.cardH

                        nodeKey: key
                        title: modelData.name
                        subtitle: root.typeLabel(modelData.type)
                                  + (sources > 0 ? qsTr(" · %1 in").arg(sources) : "")
                                  + (outs > 0 ? qsTr(" · %1 out").arg(outs) : "")
                        iconName: "layers"
                        accent: modelData.type === "audio" ? Theme.wireAudio
                              : modelData.type === "video" ? Theme.wireVideo : Theme.accent
                        badge: root.typeBadge(modelData.type)
                        status: sources > 0 ? "" : qsTr("unrouted")
                        statusColor: Theme.textMuted
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        onHoverChanged: (hoverKey, isHovered) => root.setHoveredNode(hoverKey, isHovered)
                        onContextRequested: (hoverKey, card, x, y) => root.openCardMenu(hoverKey, card, x, y)
                        // Double-click = open this node's editor (see editNode).
                        onActivated: (hoverKey) => root.editNode(hoverKey)
                    }
                }

                // ---- Screens: empty while screensShown is off ----
                Repeater {
                    model: root.screenRows
                    delegate: RoutingNodeCard {
                        required property var modelData
                        required property int index
                        readonly property string key: root.keyForOutput(Number(modelData.row))

                        x: root.columnX(2)
                        y: root.rowTop(index)
                        width: root.colW
                        height: root.cardH

                        nodeKey: key
                        title: modelData.name
                        subtitle: (modelData.kind ? modelData.kind : "")
                                  + (modelData.res ? " · " + modelData.res : "")
                        iconName: "presentation"
                        accent: Theme.wireVideo
                        enableToggle: true
                        outputEnabled: modelData.enabled !== false
                        onOutputEnabledToggled: (hoverKey) => root.toggleOutputEnabled(Number(hoverKey.substring(4)))
                        gateChips: true
                        overlayOn: modelData.showOverlay !== false
                        mediaOn: modelData.showMedia !== false
                        onGateToggled: (hoverKey, gate) => root.toggleCardGate(hoverKey, gate)
                        // The design's own empty state: a screen with nothing
                        // mapped to it says so instead of showing a dead picture.
                        status: root.screenStatus(modelData)
                        statusColor: root.screenStatusColor(modelData)
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        onHoverChanged: (hoverKey, isHovered) => root.setHoveredNode(hoverKey, isHovered)
                        onContextRequested: (hoverKey, card, x, y) => root.openCardMenu(hoverKey, card, x, y)
                        // Double-click = open this node's editor (see editNode).
                        onActivated: (hoverKey) => root.editNode(hoverKey)
                    }
                }

                // ---- Styles, in labelled segments ------------------------
                Text {
                    visible: root.gateCards.length > 0
                    x: root.columnX(1)
                    y: root.colHeaderH + 2
                    width: root.colW
                    text: qsTr("LAYERS")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                Repeater {
                    model: root.gateCards
                    delegate: RoutingNodeCard {
                        required property var modelData
                        required property int index
                        readonly property string key: root.keyForGate(index)

                        x: root.columnX(1)
                        y: root.gateTop(index)
                        width: root.colW
                        height: root.cardH

                        nodeKey: key
                        title: modelData.title
                        subtitle: modelData.layer
                        iconName: modelData.key === "overlay" ? "overlays" : "media"
                        accent: root.gateCount(modelData.key) > 0 ? Theme.accent
                                                                  : Theme.textMuted
                        status: root.gateSummary(modelData.key)
                        statusColor: root.gateCount(modelData.key) > 0 ? Theme.accentLight
                                                                       : Theme.textMuted
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        onHoverChanged: (hoverKey, isHovered) => root.setHoveredNode(hoverKey, isHovered)
                    }
                }

                Text {
                    visible: root.gateCards.length > 0
                    x: root.columnX(1)
                    y: root.colHeaderH + root.groupLabelH
                       + root.gateCards.length * (root.cardH + root.rowGap) + 2
                    width: root.colW
                    text: qsTr("STYLE TEMPLATES")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                Repeater {
                    model: root.styleRows
                    delegate: RoutingNodeCard {
                        required property var modelData
                        required property int index
                        // Slot for geometry, roster row for data (see the bus delegate).
                        readonly property string key: root.keyForStyle(modelData.row)
                        readonly property int attached: root.styleScreenCount(modelData.row)

                        x: root.columnX(1)
                        y: root.styleTop(index)
                        width: root.colW
                        height: root.cardH

                        nodeKey: key
                        title: modelData.name
                        subtitle: root.styleSubtitle(modelData)
                        // The style's own look, as its chip: the colour the output
                        // paints behind the text.
                        swatch: root.styleSwatch(modelData)
                        accent: Theme.wireStyle
                        status: attached === 0 ? qsTr("unused")
                              : attached === 1 ? qsTr("1 output")
                              : qsTr("%1 outputs").arg(attached)
                        statusColor: attached > 0 ? Theme.textSecondary : Theme.textMuted
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        onHoverChanged: (hoverKey, isHovered) => root.setHoveredNode(hoverKey, isHovered)
                        onContextRequested: (hoverKey, card, x, y) => root.openCardMenu(hoverKey, card, x, y)
                        // Double-click = open this node's editor (see editNode).
                        onActivated: (hoverKey) => root.editNode(hoverKey)
                    }
                }

                // (No add buttons down here: each column's "+" lives in its own
                // header — see the column-headers Repeater above.)

                // ---- Outputs (sinks), in labelled segments ------------------
                Repeater {
                    model: root.sinkGroups
                    delegate: Text {
                        required property var modelData
                        required property int index
                        x: root.columnX(3)
                        y: root.sinkGroupTop(index) + 2
                        width: root.colW
                        text: modelData.label.toUpperCase()
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }
                }

                Repeater {
                    model: root.sinkFlat
                    delegate: RoutingNodeCard {
                        required property var modelData
                        required property int index
                        readonly property string key: root.keyForOutput(Number(modelData.row))

                        x: root.columnX(3)
                        y: root.sinkSlotTop(index)
                        width: root.colW
                        height: root.cardH

                        nodeKey: key
                        title: modelData.name
                        subtitle: root.sinkSubtitle(modelData)
                        iconName: root.sinkIcon(modelData)
                        accent: (modelData.acceptsAudio && modelData.acceptsVideo) ? Theme.accent
                              : modelData.acceptsAudio ? Theme.wireAudio : Theme.wireVideo
                        enableToggle: true
                        outputEnabled: modelData.enabled !== false
                        onOutputEnabledToggled: (hoverKey) => root.toggleOutputEnabled(Number(hoverKey.substring(4)))
                        status: root.receivesLabel(modelData)
                        gateChips: !modelData.system
                        overlayOn: modelData.showOverlay !== false
                        mediaOn: modelData.showMedia !== false
                        onGateToggled: (hoverKey, gate) => root.toggleCardGate(hoverKey, gate)
                        // Outputs say WHAT is arriving as a compact badge, so the
                        // transport line beside the name is never cut for it.
                        statusBadge: true
                        statusColor: root.sinkStatusColor(modelData)
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        actionLabel: root.sinkActionLabel(modelData)
                        actionActive: root.takeNode.length > 0
                                      && root.sinkNodeId(modelData) === root.takeNode
                        onActionTriggered: root.transportSink(modelData)
                        onHoverChanged: (hoverKey, isHovered) => root.setHoveredNode(hoverKey, isHovered)
                        onContextRequested: (hoverKey, card, x, y) => root.openCardMenu(hoverKey, card, x, y)
                        // Double-click = open this node's editor (see editNode).
                        onActivated: (hoverKey) => root.editNode(hoverKey)
                    }
                }

                // ---- The "+" pickers ---------------------------------------
                Rectangle {
                    id: addPicker
                    readonly property var entries: root.addMenu === "output" ? root.outputKinds : []
                    readonly property bool showing: root.addMenu === "output"

                    visible: showing
                    z: 25
                    width: 304
                    height: pickerCol.height + Theme.space2
                           + (root.addMenu === "output" ? pickerFoot.height + Theme.space2 : 0)
                           + Theme.space3 * 2
                    x: root.addMenu === "output" ? root.columnX(3) - width - 10
                                                 : root.columnX(0) + root.colW + 10
                    y: root.colHeaderH + root.rowGap
                    radius: Theme.radiusMd
                    color: Theme.panelBg
                    border.width: 1
                    border.color: Theme.border

                    // No heading: the list of output types is the whole panel.
                    Column {
                        id: pickerCol
                        x: Theme.space3
                        y: Theme.space3
                        width: parent.width - Theme.space3 * 2
                        spacing: 2

                        Repeater {
                            model: addPicker.entries
                            delegate: Rectangle {
                                required property var modelData
                                width: pickerCol.width
                                height: 42
                                radius: Theme.radiusSm
                                color: "transparent"

                                HoverGlow {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    hovered: entryArea.hovered
                                }

                                Column {
                                    anchors.left: parent.left
                                    anchors.leftMargin: Theme.space2
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.space2
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1

                                    Text {
                                        width: parent.width
                                        text: modelData.label
                                        elide: Text.ElideRight
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        width: parent.width
                                        text: modelData.note
                                        elide: Text.ElideRight
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }

                                PositionHoverArea {
                                    id: entryArea
                                    anchors.fill: parent
                                    onClicked: root.addOutput(modelData)
                                }
                            }
                        }
                    }

                    Text {
                        id: pickerFoot
                        visible: root.addMenu === "output"
                        x: Theme.space3
                        y: pickerCol.y + pickerCol.height + Theme.space2
                        width: parent.width - Theme.space3 * 2
                        text: qsTr("There is one speaker output already: wire a bus to it and that bus plays.")
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    z: 20
                    visible: root.addMenu.length > 0
                    enabled: visible
                    onClicked: root.addMenu = ""
                }

                // ---- VIDEO level strips ---------------------------------------
                Repeater {
                    model: root.signalStrips
                    delegate: LevelTrack {
                        required property var modelData
                        x: modelData.x
                        y: modelData.y
                        width: root.stripW
                        height: root.stripH
                        value: root.stripValue(modelData)
                        showDot: false
                        fixedColor: true
                        fillColor: modelData.color
                    }
                }

                // ---- AUDIO level: the app's L/R pair --------------------------
                Repeater {
                    model: root.audioPills
                    delegate: Column {
                        id: pillPair
                        required property var modelData
                        x: modelData.x
                        y: modelData.y
                        spacing: root.pillGap

                        Repeater {
                            model: 2
                            delegate: ChannelPill {
                                required property int modelData
                                channelIndex: modelData
                                compact: true
                                diameter: root.pillSize
                                gain: {
                                    void root.modelsRev
                                    return root.pillGain(pillPair.modelData, modelData)
                                }
                                level: root.pillLevel(pillPair.modelData, modelData)
                                onToggled: Qt.callLater(() =>
                                    root.togglePill(pillPair.modelData, modelData))
                            }
                        }
                    }
                }

                // ---- Ports: one list, one delegate. A card's dots are drawn at
                Repeater {
                    model: root.ports
                    delegate: RoutingPort {
                        required property var modelData
                        nodeKey: modelData.key
                        kind: modelData.kind
                        x: modelData.x - width / 2
                        y: modelData.y - height / 2
                        portColor: modelData.color
                        connected: modelData.connected
                        dragOrigin: root.dragging && root.dragFrom === modelData.key
                        dragTarget: root.dragging && root.dropKey === modelData.key
                                    && root.dropKind.length > 0
                        onDragStarted: (key, kind, sx, sy) => root.beginDrag(key, kind, sx, sy)
                        onDragMoved: (key, kind, sx, sy) => root.moveDrag(key, kind, sx, sy)
                        onDragEnded: (key, kind, sx, sy) => root.endDrag(key, kind, sx, sy)
                    }
                }

                // ---- The wire being dragged ----
                Shape {
                    id: draggedWire
                    anchors.fill: parent
                    visible: root.dragging
                    z: 2
                    preferredRendererType: Shape.CurveRenderer
                    // Dimmed while the pointer is over nothing it can land on:
                    // the ghost follows the cursor, the promise does not.
                    opacity: root.dropOk ? 1 : 0.55

                    readonly property bool reversed: root.dragX < root.dragStartX
                    readonly property real fromX: reversed ? root.dragX : root.dragStartX
                    readonly property real fromY: reversed ? root.dragY : root.dragStartY
                    readonly property real toX: reversed ? root.dragStartX : root.dragX
                    readonly property real toY: reversed ? root.dragStartY : root.dragY
                    readonly property real bend: Math.max(24, Math.abs(toX - fromX) * 0.5)

                    ShapePath {
                        strokeColor: root.dragging
                                     ? root.wireColor(root.dropKind.length > 0
                                                      ? root.dropKind : root.dragKind)
                                     : Theme.textMuted
                        strokeWidth: 2.6
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        strokeStyle: ShapePath.DashLine
                        dashPattern: [6, 5]
                        startX: draggedWire.fromX
                        startY: draggedWire.fromY
                        PathCubic {
                            x: draggedWire.toX
                            y: draggedWire.toY
                            control1X: draggedWire.fromX + draggedWire.bend
                            control1Y: draggedWire.fromY
                            control2X: draggedWire.toX - draggedWire.bend
                            control2Y: draggedWire.toY
                        }
                    }
                }
            }
        }

        AppScrollBar {
            x: parent.width - (Theme.space2 + width) / 2
            y: Theme.space2
            height: parent.height - Theme.space4
            flickable: flick
        }
    }

    // ---- Card context menu -------------------------------------------------
    MenuCatcher {
        menu: cardMenu
    }

    DropdownPanel {
        id: cardMenu
        z: 90
        visible: false
        // A context menu hugs its verbs, the way a native one does, instead of
        // opening as a 480 px panel around six short lines.
        fitContentWidth: true
        model: []
        onItemActivated: (label, item) => {
            cardMenu.visible = false
            root.actOnCard(item && item.id ? String(item.id) : "")
        }
    }

    ConfirmDialog {
        id: deleteOutputDialog
        shown: root.deleteOutputRow >= 0
        title: qsTr("Delete output?")
        message: {
            if (root.deleteOutputRow < 0)
                return ""
            const o = root.outputByRow(root.deleteOutputRow)
            const name = o ? String(o.name) : ""
            return "\u201C" + name + "\u201D "
                 + qsTr("will be removed, and the buses wired to it are released.")
        }
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            OutputListModel.removeOutput(root.deleteOutputRow)
            root.deleteOutputRow = -1
        }
        onDismissed: root.deleteOutputRow = -1
    }

    // ---- Styles: the "Select Style" picker, reused from Outputs -------------
    // The picker Outputs' Style row used before it was removed (StylePickerModal),
    // so the canvas picks from the same roster the Styles page edits.
    StylePickerModal {
        id: stylePicker
        onApplied: (styleId) => {
            stylePicker.open = false
            // Picking a style here is ADDING it to the board (see beginAdd): a
            // style exists on the Styles page whether or not it is drawn here.
            StyleListModel.setOnBoard(StyleListModel.rowForId(String(styleId)), true)
            root.markStyle(styleId)
        }
        onCancelled: stylePicker.open = false
    }

    // ---- Add bus: the app's bus picker, in its create mode ------------------
    BusSourceModal {
        id: busPicker
        objectName: "routingBusPicker"
        mode: "newBus"
        onCancelled: busPicker.open = false
        onCreated: (busRow) => {
            busPicker.open = false
            // A bus made from THIS board's "+" is one the operator asked to see:
            // put it on the board (Audio & Video's own Add Bus does not).
            BusListModel.setBusOnBoard(Number(busRow), true)
            RoutingService.refresh()
        }
    }

    // ---- Configure an output: the Outputs page's own form, on the canvas ----
    ModalCard {
        id: outputDialog
        shown: root.outputDialogShown
        title: root.editOutputRow < 0 ? qsTr("Add output") : qsTr("Configure output")
        subtitle: root.outKind
        cardWidth: 780
        saveText: root.editOutputRow < 0 ? qsTr("Add output") : qsTr("Save")
        onCancelled: {
            root.outputDialogShown = false
            root.editOutputRow = -1
        }
        onAccepted: root.submitOutput()

        SettingsField {
            width: parent.width
            visible: root.outIsPreset
            label: qsTr("Name")
            text: root.outName
            onTextEdited: (t) => root.outName = t
        }
        SelectField {
            id: outPresetField
            width: parent.width
            visible: root.outIsPreset
            label: root.outKind.toUpperCase() === "STREAM" ? qsTr("Streaming preset") : qsTr("Recording preset")
            placeholder: qsTr("Pick a preset")
            readonly property var presets: {
                root.modelsRev
                // A Recording output takes a recording preset or an audio one.
                return root.outKind.toUpperCase() === "STREAM"
                       ? PresetListModel.presetsOfKind("streaming")
                       : PresetListModel.presetsOfKind("recording").concat(PresetListModel.presetsOfKind("audio"))
            }
            // The box shows the preset's NAME; the value that is kept is its id.
            value: PresetListModel.nameOf(root.outPresetId)
            options: outPresetField.presets.map((p) => ({ label: p.name, value: p.presetId }))
            onValuePicked: (v) => {
                // The panel reports the picked label; map it back to the id.
                for (let i = 0; i < outPresetField.presets.length; ++i)
                    if (outPresetField.presets[i].name === v || outPresetField.presets[i].presetId === v)
                        root.outPresetId = outPresetField.presets[i].presetId
            }
        }
        Text {
            width: parent.width
            visible: root.outIsPreset
            text: outPresetField.presets.length === 0
                  ? qsTr("There are no presets of this type yet. Make one in Settings · Recording, then pick it here.")
                  : qsTr("This output uses that preset's settings; edit them in Settings · Recording.")
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }

        ScreenForm {
            width: parent.width
            name: root.outName
            type: root.outKind
            showType: false
            res: root.outRes
            refresh: root.outRefresh
            pattern: root.outPattern
            placement: root.outDisplay
            locked: root.outLocked
            audioDevice: root.outAudioDevice
            onAudioDevicePicked: (id) => root.outAudioDevice = id
            onNameEdited: (t) => root.outName = t
            onResPicked: (v) => root.outRes = v
            onRefreshPicked: (v) => root.outRefresh = v
            onPlacementPicked: (screenName, res, refresh) => {
                root.outDisplay = screenName
                if (res !== "") {
                    root.outRes = res
                    root.outRefresh = refresh
                }
            }
            onLockToggled: (l) => root.outLocked = l
            onPatternPicked: (k) => root.outPattern = k
        }

        Text {
            width: parent.width
            visible: root.outKind.toUpperCase() === "NDI"
            text: qsTr("Other machines see this name on the network, and the picture is sent at the resolution and frame rate above.")
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }

        Item {
            width: parent.width
            height: root.outIsHdmi ? 34 : 0
            visible: root.outIsHdmi

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Stay on top")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }

            SettingsToggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: root.outStayOnTop
                onToggled: root.outStayOnTop = !root.outStayOnTop
            }
        }
    }
}