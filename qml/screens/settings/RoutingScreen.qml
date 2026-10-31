import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

// Settings · Routing — the routing canvas: Buses | Styles | Screens | Outputs.
//
// THE DESIGN (docs: "Routing design for Buses, Screens, Styles and Outputs") is
// a node canvas in the Audio & Video screen's visual language where a signal
// goes only where a wire puts it. Four layers, one column each:
//
//   Buses    carry audio and/or video; the sources feeding them are wired on
//            Audio & Video, and each card shows how many it has
//   Styles   a look; one style can sit on many screens, and it sits LEFT of the
//            screens that wear it — see the note below
//   Screens  the HDMI surfaces a show is sent to; each takes video from a bus
//            and wears a style
//   Outputs  the sinks (NDI, SDI, recording, streaming and the system output),
//            each accepting only the signal types it can carry. This canvas is
//            where a sink is CREATED, because it is where its buses are wired
//
// STYLES SIT BEHIND THE SIGNAL PATH — one column LEFT of the screens they dress.
// A style is a property a screen wears, not a hop a signal makes, so its dashed
// link never enters the bus → screen → output run that IS a signal: it ends at
// the screen and nothing crosses it. (An earlier order put styles on the far
// right, where every style link had to travel back past Outputs to reach the
// screen it belonged to, which read as a signal going the wrong way.)
//
// ONE PORT PER SIGNAL. A bus that carries both has an audio dot and a video dot;
// so does an output that takes both. The split is the whole point of a typed
// graph: the audio half of a both-bus goes to Speakers while its video half goes
// to the screen that shows it, and each half is a separate wire you can see,
// hover, explain and cut on its own. A drag carries the signal it started on, so
// an audio dot can only land on another audio dot — the refusal is structural,
// not a rule checked after the fact.
//
// A WIRE HERE IS NOT A MEMORY. It is an OutputConfig entry in the engine's
// production graph: each wire is that output node's VIDEO or AUDIO BUS
// (ProductionGraph::AssignOutputBuses — the same pairing every output in
// docs/specs/27 carries), and the style wire is the output's style id. Both are
// validated by the engine, persisted with the production document, and read back
// through RoutingService — so the canvas cannot drift from what the system is
// configured to send. Nothing on this screen keeps a private copy of the
// routing: RoutingService mirrors each roster row as "out:<stable id>" and reads
// the assignments back, and this file only draws them.
//
// Dragging a port onto a card connects; dragging the same pair again takes it
// back out; right-click removes a single wire. While a drag is live every card
// that cannot take it steps back — "not wired, not sent" made visible instead of
// refusing silently on release. A screen with nothing mapped to it reads
// "No signal"; an output's dots fill in as its halves arrive, and an output with
// neither reads "silent".
//
// A SCREEN HAS TWO SIDES. It takes video on its left dot (from a bus, and a style
// beside that) and sends its picture from a dot on its RIGHT edge into a sink —
// the design's "a screen feeds a video output". That feed is a video EDGE between
// the two output nodes in the production graph (RoutingService::linkScreen), so
// it validates and persists like every other wire here; today it records which
// screen an output mirrors, and the sink's delivery side does not consume it yet.
// A sink's video dot therefore has two possible sources — a bus, or a screen —
// which is what an input dot on a destination really is.
Item {
    id: root

    // The canvas asks the settings shell to switch section when a node's
    // right-click menu leads elsewhere (a bus to Audio & Video, a screen to
    // Screens, a style to Styles). Same signal GeneralScreen already emits, so
    // Settings navigation stays in one place.
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
    // The assignments themselves. RoutingService republishes its rows after
    // every engine write, so a refused assignment (a dead bus, a signal the
    // output does not take) shows up as the wire simply not appearing.
    Connections {
        target: RoutingService
        function onRoutingChanged() { root.modelsRev++ }
        function onRowsChanged() { root.modelsRev++ }
        // A wire the ENGINE refused now says so. Before this the gesture was
        // dropped in silence and the wire simply never appeared, which is
        // indistinguishable from a broken canvas — or from a wire that landed and
        // is producing nothing.
        function onRefused(reason) { EventBus.notify(String(reason)) }
    }

    // ---- THE BOARD IS A PATCH BAY, NOT A ROSTER DUMP -------------------------
    // A card exists here only because the operator PUT it here (the column's
    // "+", or the create flow that "+" opens). The roster is a different list:
    // every bus Audio & Video knows about. Two properties, one meaning each —
    //   busRoster — roster order, indexed by ROSTER ROW (what actions, wires and
    //               the models speak in);
    //   busRows   — the board, in the order it draws; `row` carries the roster
    //               row, and the array INDEX is the slot the geometry uses.
    // Without the split, a bus made on Audio & Video (or a style made on the
    // Styles page) appeared on this canvas as if the operator had added it.
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
    // One entry per roster row + the system output, from the service that
    // mirrors them into the graph: { row, id, name, kind, res, styleId,
    // enabled, screen, system, acceptsVideo, acceptsAudio, videoBus, videoBuses,
    // audioBus, audioBuses, wired }.
    //
    // `videoBuses` / `audioBuses` are the WIRED SETS (every bus that side holds);
    // `videoBus` is which of the video ones is ON AIR, because an output sends one
    // picture. Both lists are the allow-list for "show this bus here".
    // Screens are off the canvas for now (screensShown). With it off an HDMI row is an
    // output like any other: it sits in the Physical group of the Outputs column, so a
    // screen added from this page is visible where it was added. Turning it on brings
    // back the SCREENS column, its cards, its ports and its style and feed wires.
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
    // The sinks: every non-screen row (NDI / SDI / REC / STREAM) plus the
    // speaker endpoint, which is a real sink with a real audio port — the
    // engine's mixer sums every audio bus WIRED to it, so the card is where
    // those wires belong (a bus added after boot used to have nowhere to land).
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
    // A sink is one of a handful of DIFFERENT things — the mixer's audio
    // endpoint, a network sender, a card, a file, a stream — and the operator
    // looks for one of them by name ("where is the SDI?"). One undifferentiated
    // run of cards answered that badly, so the column is read in labelled
    // segments. The segments keep sinkRows' own order inside each one, so a
    // wire's row index still means the roster's row.
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
    // The same markers the Outputs column wears. A bus card already says what it
    // is (V / A / A+V / TPL), but a roster interleaved by kind is a list you read
    // four times to find one camera: Video, Both, Audio and Template each get their
    // own run with a marker above it. Inside a run the roster's own order is kept,
    // so a wire's row number still means the roster's row.
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
    // Roster row per DRAWN slot, in the order the column draws them — the bridge
    // between a row (which every action and wire speaks in) and a slot (which the
    // geometry speaks in). Built from busRows itself, so the two can never
    // disagree about what is drawn.
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
    // points: this screen draws the ports (the cards do not), so a wire's
    // endpoint and the dot it leaves from are computed from the same numbers and
    // can never drift apart.
    readonly property int colHeaderH: 40
    readonly property int cardH: 56
    readonly property int rowGap: 10
    readonly property int gutter: 44
    // Columns on the board: Buses, Styles and Outputs, plus Screens while screensShown
    // is on. Cards are capped in width and the group is centred on the board, so a
    // wide window grows the margins, not the cards.
    readonly property int columnCount: root.screensShown ? 4 : 3
    readonly property real minBoardW: root.columnCount * 130 + (root.columnCount - 1) * root.gutter

    readonly property real boardW: Math.max(flick.width, root.minBoardW)
    readonly property real boardPad: 24
    readonly property real colMaxW: 240
    readonly property real colW: Math.min(root.colMaxW,
        (root.boardW - root.gutter * (root.columnCount - 1) - root.boardPad * 2) / root.columnCount)
    readonly property real groupLeft: Math.max(root.boardPad,
        (root.boardW - (root.columnCount * root.colW + root.gutter * (root.columnCount - 1))) / 2)
    // EVERY column's "add" is a small "+" IN ITS HEADER, not a 40 px bar under its
    // cards: four full-width accent buttons spent more vertical room than the
    // cards they sat beneath, and the canvas is the wires. One control per column,
    // exactly where the column's own name already is.
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
    // TWO GATE CARDS ride at the HEAD of the STYLES column (Overlays, Media — see
    // gateCards). They are not style rows: no style of their own, no swatch, so
    // the style cards below them start past both the gate run and the marker that
    // closes it. styleTop is the ONE place that add is made, so a style's dot can
    // never end up above its own card or sitting on a gate card's rectangle.
    //
    // AND THE COLUMN IS SEGMENTED like Buses and Outputs: the gates are LAYERS
    // (permission wires into an output), the cards beneath are STYLE TEMPLATES
    // (the design an output paints). Two runs, two markers, so the eye can tell a
    // gate from a style without reading the card.
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
    // The y of ANY card, in whatever column it lives in: the Outputs column is
    // segmented, so it cannot use the flat rowTop — but the cards and the wires
    // must still read their position from ONE function.
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
    // Each column's "+" opens the picker that ALREADY owns that layer's creation,
    // so the canvas never grows a second, thinner way to make the same thing:
    //   Buses   -> BusSourceModal's create mode (Media › Inputs' "New bus"): pick
    //              the source, and the bus is born already wired to it
    //   Styles  -> StylePickerModal, the "Select Style" picker Outputs' Style row
    //              used, so a style is picked from the roster, not drawn onto the canvas
    //   Screens -> its own column, shown while screensShown is on (no picker yet)
    //   Outputs -> the transport picker drawn below: a sink exists nowhere else,
    //              because the canvas is where its signals are decided
    property string addMenu: ""
    // The header box the open roster menu drops from (see beginAdd). Handing the
    // panel its own column's header is what makes it land under that column at
    // that column's width, instead of a fixed 480 px slab laid across the next
    // column — the "this drop is not clean, align it" report.
    property Item addAnchor: null
    // Which panel a column's "+" owns: the two roster LISTS (buses, styles) and
    // the Outputs transport picker. Screens has no "+" yet.
    function addMenuFor(column) {
        return column === 0 ? "bus"
             : column === 1 ? "style"
             : column === 3 ? "output" : ""
    }
    // The roster panels are DropdownPanels driven from this ONE state, and they
    // OPEN with openAt. Three things follow, and all three are the fix:
    //   * openAt reparents the panel to the window layer, so the canvas's own
    //     clip:true no longer cuts the list off at the canvas edge;
    //   * it places the panel at the header it was opened from, with
    //     matchAnchorWidth giving it that column's width;
    //   * it lifts the MenuCatcher beside it to the same layer, so a click
    //     ANYWHERE off the menu dismisses it. The board-scoped catcher this
    //     replaced only covered the canvas, so a press on the page header (or
    //     Refresh) left the list hanging open over the wiring.
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
    // A menu opened with openAt lives in the WINDOW OVERLAY from then on, not
    // under this page (the Loader that hosts the settings sections deletes the
    // page when the operator switches section). Without this, leaving Routing
    // with a list open left that list floating over the next section — the same
    // way GroupPickerField closes its own reparented menu on the way out.
    // (Closed in this page's one Component.onDestruction, further down, with the
    // meter holds: an object takes only one such handler.)
    // One entry point per column, called by the header "+".
    //
    // BUSES and STYLES open a list first, because those two columns show the
    // BOARD (only what the operator put there) while their popup is the ROSTER
    // (everything that exists). The list offers what is not on the board yet, and
    // one entry that makes something new — so "add" always means "put it here",
    // and a card can never appear because it happened to exist.
    //
    // `anchorItem` is the header box the "+" sits in. The caller hands it in: the
    // menu is placed from it, so nothing has to guess a position in board
    // coordinates (which also drift once the board scrolls).
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
    // The Styles "+" opens the "Select Style" picker (StylePickerModal) that
    // Outputs' Style row used. Picking a style marks it on the canvas, so the
    // operator sees which look was chosen without a new card appearing.
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
    // Picking HDMI in the Outputs "+" no longer drops a bare "HDMI" row on the
    // canvas: it opens this first, because a physical output is not configured
    // by its name — it is configured by WHICH DISPLAY it goes out on, at what
    // resolution and refresh, with the window rules that decide whether it
    // covers the monitor or starts half-size, and whether it stays on top.
    //
    // It is the same form Settings · Outputs' Add/Edit uses (ScreenForm: name,
    // resolution, refresh, test pattern and the display picker that lists the
    // real QScreens), so the two doors configure one row the same way. The
    // window rules (bounds lock, stay on top) are shown for HDMI only — an NDI
    // or recording sink has no window to place, and offering them there was
    // how a row ended up carrying a display binding it could never use.
    //
    // `editOutputRow` < 0 = ADDING (the row is created on Save, so Cancel leaves
    // no half-made output behind); >= 0 = editing that roster row.
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
    // Where this row's audio plays ("" = the program stream). Its own endpoint
    // makes the row's mix leave on that device's stream, opened from the row's
    // audio wires — see RoutingService::bindOutputAudio.
    property string outAudioDevice: ""
    readonly property bool outIsHdmi: root.outKind.toUpperCase() === "HDMI"
    // The transports that sit on a real display. ScreenForm's own placement
    // section is gated the same way (HDMI/SDI), so these two are the rows where
    // a display binding means something — see its `visible` rule.
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
        root.outputDialogShown = true
    }
    function openEditOutput(row) {
        if (row < 0 || row >= OutputListModel.rowCount())
            return
        // The ROSTER row, not the routing snapshot: the dialog edits fields the
        // engine's routing rows do not publish (refresh, test pattern, the
        // display binding, the two window rules) — getOutput carries them, the
        // same accessor Settings · Outputs' Edit dialog reads.
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
        root.outputDialogShown = true
    }
    function submitOutput() {
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
    // What an output can BE. HDMI is here because it is the physical screen's
    // transport — picking it makes a SCREEN (it appears in the Screens column);
    // the rest are sinks that live in this column.
    readonly property var outputKinds: [
        { kind: "HDMI",   group: qsTr("Physical"),  label: qsTr("HDMI"),      note: qsTr("A monitor or projector — pick its display next") },
        { kind: "SDI",    group: qsTr("Physical"),  label: qsTr("SDI"),       note: qsTr("A capture card's output") },
        { kind: "NDI",    group: qsTr("Network"),   label: qsTr("NDI"),       note: qsTr("Video + audio over the network — name the sender next") },
        { kind: "REC",    group: qsTr("Recording"), label: qsTr("Recording"), note: qsTr("A file on this machine") },
        { kind: "STREAM", group: qsTr("Streaming"), label: qsTr("Streaming"), note: qsTr("The RTMP target on the Recording page") }
    ]
    function toggleAddMenu(which) {
        root.addMenu = root.addMenu === which ? "" : which
    }
    function addOutput(kind) {
        const k = String(kind || "").toUpperCase()
        if (k.length === 0) return
        // EVERY transport is configured before it exists. The row's identity is
        // its transport, and each transport is addressed differently — an HDMI
        // display, an NDI sender's name and mode, an SDI card's output, a file,
        // a stream target — so the picker opens the dialog with that transport
        // preset instead of dropping a bare row the operator then has to go and
        // fill in on another page. Cancel leaves nothing behind (the row is
        // created by Save).
        root.openAddOutput(k)
        return
        // The TRANSPORT is the thing being chosen, so it is written at creation:
        // Settings · Screens deliberately has no kind picker any more. Naming a
        // sink after its transport lets the model keep names unique from there
        // ("NDI", "NDI 2", …) — a duplicated name is a duplicated key to every
        // per-output reader.
        root.addMenu = ""
        OutputListModel.addScreen(k, k, "", "")
        RoutingService.refresh()
    }


    // ---- Card right-click ----------------------------------------------------
    // The canvas WIRES; it does not host three editors. So a node's menu offers
    // what can be done to it here (wire it, take a side out, delete it) plus the
    // one hop to the layer's real editor — a bus on Audio & Video, a screen on
    // Screens, a style on Styles.
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
        // THE OUTPUT'S OWN GATES, on BOTH kinds of output card. They used to be two
        // cards at the head of the STYLES column writing the ON-AIR STYLE's flags —
        // one close, every screen wearing that theme. Here they write THIS row
        // (OutputListModel::setShowMedia / setShowOverlay), so the picture changes on
        // this output and no other; a style's own gates still apply on top.
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
            // The picture this screen shows: its one routed feed, or the design
            // composed with each bus window painting its own assigned bus. A
            // DISPLAY deserves the same choice a recorder or sender gets.
            if (o)
                items.push({ id: "toggleCompose", label: String(o.videoMode || "") === "compose"
                    ? qsTr("Show direct feed") : qsTr("Compose design") })
            if (o && String(o.styleId || "").length > 0)
                items.push({ id: "unwireStyle", label: qsTr("Take the style off") })
            // No Delete here: a screen is deleted on Settings · Screens, where
            // its display binding and its window are in view.
            return items
        }
        // A sink. It is configured HERE — the same dialog the canvas's picker
        // opens — and the deeper settings (a recording profile, the RTMP target)
        // stay one hop away on the Recording page. Its wires are cut here.
        const kind = String(o ? o.kind : "").toUpperCase()
        const items = [{ id: "edit", label: qsTr("Configure output") }]
        if (kind === "REC")
            items.push({ id: "editRecording", label: qsTr("Recording settings") })
        if (kind === "STREAM")
            items.push({ id: "editRecording", label: qsTr("Stream settings") })
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

    // The card's own name and what it is — the menu's header, so the panel says
    // WHICH node it is about and what layer it belongs to (a DropdownPanel opens
    // with headerTitle/headerSubtitle everywhere else in the app; a bare list of
    // verbs was the only menu in Settings that did not).
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
            // The speaker endpoint has nothing to open, rename or delete: the
            // node is the engine's (AudioEngine makes it with the caps the mixer
            // needs), so its card carries wires and no roster menu.
            if (o && o.system)
                return
        }
        root.menuKey = key
        cardMenu.headerTitle = root.nodeName(key)
        cardMenu.headerSubtitle = root.nodeSummary(key)
        cardMenu.model = root.menuFor(key)
        cardMenu.openAt(card, x, y)
    }

    // OPEN A NODE'S EDITOR — the one action behind both the card's menu ("Edit
    // …") and a DOUBLE-CLICK on the card itself. Each layer's editor lives where
    // that layer is edited: a bus on Audio & Video, a style on Styles, an output
    // in the dialog this canvas now owns (display, mode, window rules).
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
            // Its own editor is the OUTPUT DIALOG — the same form Settings ·
            // Outputs uses, display picker included — opened on this row here on
            // the canvas, so configuring a display no longer means leaving the
            // wiring you are looking at.
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

    // Where a signal's dot sits on its card, measured from the card's centre.
    //
    // A card with ONE dot keeps that dot on the centreline — FreeShow's own `.port`
    // at `top: 50%`, so a bus or a style lines up with its neighbour instead of
    // hanging above or below the middle. Only an output that carries BOTH signals
    // draws two dots, and those straddle the same centre symmetrically: video
    // above, audio below, which is the order every other surface in the app reads.
    function portOffset(key, kind) {
        if (!root.cardHasTwoDots(key)) return 0
        if (kind === "video") return -11
        if (kind === "audio") return 11
        return 0
    }
    // Which cards stack TWO dots: every bus (it carries a video plane and an audio
    // plane) and every output but the speaker endpoint (it takes both signals).
    // Those pairs straddle the card's centreline; a STYLE card has one link dot, and
    // so does the speaker endpoint, which takes audio and nothing else — for those
    // the single dot IS the centre.
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
    // A key names a row as the canvas draws it: "bus:<busRow>", "out:<rosterRow>"
    // and "style:<styleRow>". They are gesture identities, not storage — every
    // wire is re-derived from the engine's assignments on each revision, so a
    // row that moves cannot leave a stale line behind (the failure a stored
    // key graph has to prune its way out of).
    function outputByRow(row) {
        const rows = root.outputRows
        for (let i = 0; i < rows.length; ++i)
            if (Number(rows[i].row) === row) return rows[i]
        return null
    }
    function styleById(id) {
        if (!id) return -1
        for (let i = 0; i < root.styleRoster.length; ++i)
            if (String(root.styleRoster[i].id) === String(id)) return i
        return -1
    }

    // The column order IS the layer order (Buses → Styles → Screens → Outputs),
    // and every gesture below is written in terms of it: a drag is normalised
    // left-to-right, so moving a column is a change HERE, not in six handlers.
    function columnOf(key) {
        if (key.indexOf("bus:") === 0) return 0
        // A GATE is a Styles-column node: the Overlays/Media cards sit at the head
        // of that column and send their permission out of the same right edge a
        // style's look leaves from.
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
            // The Outputs column draws its rows GROUPED, so a sink's row index is
            // its index in the flat, grouped order — the same list the
            // "+" tile and the wires measure from.
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
    // HAS THIS BUS A PICTURE? Its roster type decides: an "audio" bus sends sound
    // only, so its card carries no video dot and no video strip. "video" and
    // "both" send a picture. (This used to answer true for every bus, so an
    // audio-only bus was drawn with a video port nothing could ever land on —
    // BusListModel refuses a video route into it with "audio-only-bus".)
    function busHasVideo(index) {
        const bus = root.busRoster[index]
        return !bus || bus.type !== "audio"
    }
    // HAS IT SOUND? Every bus can carry it: a "video" bus is a camera/feed bus,
    // and the mixer sums its sources' OWN audio (AudioMixer::FeedsForBus reads the
    // paired #v plane for the #a one), so its audio dot is a wire the engine really
    // carries — hiding that dot was a bug this file has already fixed once.
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
    // Every audio bus wired into one row. A row can hold several (the wires are
    // an allow-list, not one assignment — see RoutingService::rows), and the
    // speaker endpoint sums them, so the canvas reads one shape for every card.
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
    // Every VIDEO bus wired into one row, same shape — and the set a click is
    // allowed to pick from (RoutingService.showBusOn refuses anything else). One
    // of them is on air: that is `o.videoBus`.
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
    // The SCREEN rows feeding one sink ("this output sends what that screen
    // shows"), published from the graph's edges by RoutingService::linkScreen.
    // Empty for a screen and for the speaker endpoint.
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

    // ---- Wires, derived from the engine's assignments ------------------------
    readonly property var wires: {
        root.modelsRev
        const out = []
        const rows = root.outputRows

        // bus → output, one wire per assigned side — and per SIDE is literal
        // here: the video wire leaves the bus's video dot and lands on the
        // output's video dot, the audio wire does the same on the other pair, so
        // a both-bus into a both-output is two parallel lines with four distinct
        // endpoints instead of one ambiguous curve.
        for (let i = 0; i < rows.length; ++i) {
            const o = rows[i]
            const to = root.keyForOutput(Number(o.row))
            // One video wire PER wired bus — a row can hold several, and every
            // one of them lands on the same dot, because that is one input. The
            // one ON AIR is marked (`onAir`), which is what the wire layer draws
            // solid; the rest are dashed: wired, carried, not the picture going
            // out. Clicking a wire makes its bus the one on air.
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
            // One audio wire PER feeding bus: a roster row has one, the speaker
            // endpoint has as many as are wired to it — and every one of them
            // lands on the same dot, because that is literally one input.
            // A screen's picture into this sink, drawn from the screen's OWN
            // right edge to the same video dot a bus lands on (one input, two
            // possible feeds — which is what the row actually is).
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

        // GATE → OUTPUT: the allow list, drawn. The wire is not stored anywhere —
        // the row's own flag IS the wire (OutputListModel::setShowOverlay), so this
        // list is read from the rows and a wire can never disagree with what the
        // engine is painting. It lands on the output's LEFT edge, where its style
        // wire lands, because both answer the same question: what may this output
        // show.
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
                // The two permissions arrive a few pixels either side of the style
                // line (overlay above, media below), so a row that wears a style AND
                // allows both layers shows three lines, not one smudged one.
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
    // A dot lights when ANY wire passes through that edge of the card: the dot is
    // the CARD's, not a signal's, so a both-bus's video wire lights it while its
    // audio half is still unwired.
    function portConnected(key, side) {
        return side === "out" ? root.wiresFrom(key).length > 0
                              : root.wiresInto(key).length > 0
    }

    // ---- The two GATES: Overlays and Media ------------------------------------
    // The two chips that used to sit inside Styles · Edit Style. They are the
    // ALLOW LIST, expressed the way every other decision on this canvas is: with a
    // wire. They sit at the head of the Styles column because they are a look
    // question (what may this output paint of the design), and the column to their
    // right is the outputs that answer it.
    //
    // A GATE CARD IS A WIRE SOURCE, exactly like a bus: drag from its right-edge dot
    // to an output and that output MAY paint the layer; the wire that appears IS the
    // permission; cutting it (or dragging it again) takes the permission away. No
    // wire, nothing paints. A style's own gate still applies on top, so a wire can
    // open what its theme already offers and never what the theme forbids.
    //
    // The per-output chips on an output card (RoutingNodeCard::gateChips) write the
    // SAME flag, so they are a shortcut for the wire, not a second rule.
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
    // They used to be two cards at the head of the STYLES column, writing the
    // ON-AIR STYLE's own flags — so closing media for one screen closed it for
    // every screen wearing that theme. They are now the two switches on an
    // OUTPUT's own card menu ("Hide media on this output", "Hide overlays on this
    // output"), which write that ROW's gates (OutputListModel::setShowMedia /
    // setShowOverlay) and reach only that row's picture. A style's own gates still
    // apply on top: both must allow, so a per-output switch can close what a
    // theme offers but never open what it forbids.

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

        // A bus sends the halves it CARRIES from its right edge: a both-bus stacks
        // them (video above the centre, audio below — see portOffset) and a bus
        // that is only audio or only video has ONE dot, on the centreline, like
        // every other single-signal card.
        for (let i = 0; i < root.busRoster.length; ++i) {
            const key = root.keyForBus(i)
            if (root.busHasVideo(i)) add(key, "video", "out")
            if (root.busHasAudio(i)) add(key, "audio", "out")
        }
        // A screen takes video on its left edge and wears a style there too —
        // the style column is immediately LEFT of it, so both dots face the
        // things that feed them. Its RIGHT edge SENDS its picture: a screen is a
        // layer, not just a destination ("a screen feeds a video output"), so it
        // has an outgoing video dot for the sink beside it.
        // Empty while screensShown is off (screenRows is then empty).
        for (let i = 0; i < root.screenRows.length; ++i) {
            const key = root.keyForOutput(Number(root.screenRows[i].row))
            add(key, "video", "in")
            add(key, "style", "in")
            add(key, "video", "out")
        }
        // An output takes through one dot PER SIGNAL it can carry: a video-only
        // transport (an HDMI display) has one, centred; one that carries both
        // (NDI, recording, streaming) has two, stacked around the same centre by
        // portOffset. The speaker endpoint takes audio and nothing else, so audio
        // is its one dot. A sink wears its style through these same dots — a look
        // is not a third input.
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
    // The half a live drag would actually carry, resolved against the card under
    // the pointer: one dot per card means the DROP decides whether a both-bus is
    // sending its picture or its sound (see resolveKind).
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
    // The bridge's taps are REFCOUNTED per device label, so holding one here never
    // disturbs the Media pane's own hold on the same microphone — and this page
    // releases ONLY what it took (never stopAllInputMeters, which is how a closed
    // Settings used to kill the meters the Media pane still had on screen).
    // A label is held while an audio bus on this canvas routes it in AND that bus
    // is itself wired onward — exactly when its level strip is drawn: no tap is
    // opened for a meter nobody can see.
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
    // A media file and an NDI stream carry audio no capture device can reach, so
    // the bridge's STREAM tap is its carrier (IAudio::StartStreamTap through
    // startStreamMeter). It publishes into the SAME inputLevels list under the
    // row's own sublabel, which is why busChannelMix below needed no new lookup.
    // Without this half, a bus fed only by a video row read "flat zero" on its
    // strip while the engine was mixing that row's audio into the bus — the one
    // thing a level strip exists to disprove. The hold discipline is the
    // device meters': a tap is held only while a bus that carries audio routes
    // the row in AND is itself wired onward, releases only this page's holds, and
    // is refcounted per label so Audio & Video can hold the same row too.
    // kind is the PAL's producer kind: "media" (the backend decodes the file's
    // audio track) or "ndi" (a receiver pushes the samples); a camera or a screen
    // capture has no audio at all, so it is never asked for.
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
    // Idempotent diff, the same contract Audio & Video uses: release what left,
    // start what arrived, leave the rest running. Both carriers are reconciled in
    // one pass because they feed one reading (busChannelMix) — a bus that swaps a
    // microphone for a media file must not lose its meter for a frame in between.
    function syncMeters() {
        // The guard is belt beside the connections' own teardown: a model signal
        // already in flight when this page is destroyed must not start a tap on a
        // torn-down object.
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

    // Can a drag that started on `kind` at `aKey` land on `bKey`? Both directions
    // are allowed (a wire can be drawn from either end), so the pair is
    // normalised left-to-right first: the column order IS the signal path.
    function pairOk(aKey, aKind, bKey) {
        const ca = root.columnOf(aKey)
        const cb = root.columnOf(bKey)
        if (ca < 0 || cb < 0 || ca === cb) return false
        const fromCol = Math.min(ca, cb)
        const toCol = Math.max(ca, cb)
        const fromKey = ca < cb ? aKey : bKey
        const toKey = ca < cb ? bKey : aKey

        // A look is WORN, not sent: styles (col 1) feed a screen (col 2), and a
        // screen is the only thing that can wear one. `toKey` is the screen
        // because the pair was normalised left-to-right.
        // A look is worn by an OUTPUT: a screen (when Screens is on) or any sink, but
        // never the speaker endpoint, which has no picture to dress.
        if (aKind === "style")
            return fromCol === 1 && toCol >= 2 && !root.isSystem(toKey)

        // A SCREEN feeds a sink: its picture leaves the screen's right edge and
        // lands on an output that takes video. Only video travels this way — a
        // screen has no audio of its own to send, and a screen never feeds a
        // screen (each one is its own surface).
        if (fromCol === 2 && toCol === 3) {
            if (aKind !== "video") return false
            const sink = Number(toKey.substring(4))
            return sink !== Number(fromKey.substring(4)) && root.outputAccepts(sink, "video")
        }

        // A GATE (col 1) reaches an OUTPUT — a screen or a sink — and that wire is
        // the permission itself: "this output may paint the overlay layer" / "…the
        // media layer". The speaker endpoint is refused: it has no picture to dress.
        // (`fromKey` rather than `aKind`, so a wire pulled BACK from an output's dot
        // onto the gate card is the same edge: the pair is normalised first.)
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
    // Which HALF a drop carries. The dot a card owns is one, so the drag is not
    // committed to a signal until it lands: a bus carrying both, dropped on the
    // speakers, is its audio half; the same dot dropped on a screen is its video
    // half. The dot's own kind is tried first (so a video-only bus can never
    // arrive as audio), and the sibling is accepted only when the card really
    // carries it — pairOk owns both facts.
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
        if (wireKind.length > 0)
            root.toggleWire(key, over, wireKind)
    }

    // Which node's card contains this canvas point — the cards' rects come from
    // the same formulas that place them, so this can never disagree with what is
    // drawn.
    function nodeAt(x, y) {
        // THE GATE CARDS FIRST: they carry the output dot their permission leaves
        // from, so a drop on one (a wire pulled back from an output) has to land on
        // its key, not on the style card below it.
        for (let g = 0; g < root.gateCards.length; ++g) {
            const left = root.columnX(1)
            const top = root.gateTop(g)
            if (x >= left - 13 && x <= left + root.colW + 13 &&
                y >= top - 8 && y <= top + root.cardH + 8)
                return root.keyForGate(g)
        }
        for (let c = 0; c < 4; ++c) {
            // ...in the same order the columns are drawn: buses, styles, screens,
            // sinks. A card's rect comes from columnX/rowTop, so a hit test can
            // never disagree with the card the pointer is on.
            const list = c === 0 ? root.busRows
                       : c === 1 ? root.styleRows
                       : c === 2 ? root.screenRows : root.sinkFlat
            for (let r = 0; r < list.length; ++r) {
                const left = root.columnX(c)
                // The Outputs column is segmented, so its cards sit where
                // sinkSlotTop puts them, not where a flat rowTop would — and the
                // Buses/Styles columns draw only their board, so a slot's y is the
                // SLOT's, while the key carries the ROSTER row.
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

    // Applying a wire twice takes it back out — the design's "not wired, not
    // sent" from the other direction. Each side is a real engine write:
    //   bus → output   AssignOutputBuses (or ClearOutputBusAssignment when it is
    //                  already that bus), for THIS signal only — the other half
    //                  of the bus is untouched
    //   screen → style OutputListModel::setStyle (the output's style id)
    function toggleWire(a, b, kind) {
        const ca = root.columnOf(a)
        const cb = root.columnOf(b)
        const fromKey = ca < cb ? a : b
        const toKey = ca < cb ? b : a

        // A GATE wire is the permission: applying it twice takes the permission
        // away again, which is exactly what a bus wire does to its assignment.
        // Detected by the FROM end, not by `kind`: a wire pulled BACK from an
        // output's own dot arrives here typed as that dot's signal (resolveKind
        // answers with the dot it started on), and it is still this same edge.
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
            // Which end is which comes from the COLUMNS, never from the drag's
            // direction — a wire can be pulled from either end, and a style is
            // now the LEFT end of its link.
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
        // A wire between two output columns is a SCREEN feeding a sink, not a
        // bus assignment — a different engine write (a video edge between the two
        // output nodes), so it is decided by the COLUMNS before anything else.
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
        // Unwiring names the BUS, not just the side: the speaker endpoint carries
        // several audio buses, and "clear the audio side" there would have taken
        // every one of them instead of the wire that was dropped.
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
            // The SAME direction-free geometry the wire is DRAWN with (see the
            // wire delegate's `reversed`): a hit test that mirrored the drawing
            // would leave a wire unclickable exactly where it curves.
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
    // A LEFT-click on a video wire: show THAT bus here — the click is the picker,
    // and the wire it lands on is by definition in the allow-list. Only wired
    // buses are drawn, so this can only be refused against a stale frame (the
    // service checks the engine's own edge set regardless, and says so out loud).
    // A screen→sink wire is not a bus pick and is left alone.
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
        if (kind === "REC") return "fileText"
        if (kind === "STREAM") return "activity"
        return "link"
    }

    // ---- A sink's own transport ---------------------------------------------
    // The graph node a sink row's transport IS: RoutingService names every output
    // node "out:<stable row id>", and the RTMP row is the one exception
    // (RecordingService's "out:stream", the node it creates when a destination is
    // set). This is the node a take started from the card listens to.
    function sinkNodeId(o) {
        if (!o || o.system) return ""
        const kind = String(o.kind || "").toUpperCase()
        if (kind === "STREAM") return "out:stream"
        return o.id ? "out:" + String(o.id) : ""
    }
    // The node the LIVE take belongs to — "" while idle, and for a take started on
    // the program from Settings · Recording, which no card owns.
    readonly property string takeNode: RecordingService.recording
                                       && RecordingService.recordingNode.length > 0
                                       ? RecordingService.recordingNode : ""
    // Which sinks carry a transport pill, and what it says. Recording and
    // streaming are the two transports you START and STOP; every other card simply
    // describes itself. A take is bound to the node it was started on, so only that
    // card says Stop and the rest keep offering their own Start.
    function sinkActionLabel(o) {
        if (!o || o.system) return ""
        const kind = String(o.kind || "").toUpperCase()
        if (kind !== "REC" && kind !== "STREAM") return ""
        const node = root.sinkNodeId(o)
        if (node.length === 0) return ""
        if (root.takeNode === node) return qsTr("Stop")
        // The RTMP row has no node until a destination is set, and a take on a node
        // the graph does not have is refused — so it offers nothing yet.
        if (kind === "STREAM" && !RecordingService.streaming) return ""
        return qsTr("Record")
    }
    // Start/stop a take ON THIS CARD'S NODE. The settings are the ones Settings ·
    // Recording persists, so a take begun here is encoded exactly like one begun
    // there; startRecordingOn refuses a node with nothing wired (its own toast
    // names the reason, which is the honest answer for an empty card).
    function transportSink(o) {
        const node = root.sinkNodeId(o)
        if (node.length === 0) return
        if (root.takeNode === node) {
            RecordingService.stopRecording()
            return
        }
        if (RecordingService.recording) {
            // ONE engine session at a time: say so rather than letting a second
            // Start replace the profile under the take that is already running.
            EventBus.notify(qsTr("A recording is already running on another output — stop that one first."),
                            "warning", qsTr("Routing"), "routing.transport.busy")
            return
        }
        RecordingService.startRecordingOn(node, RecordingService.config)
    }

    // ---- What each WIRED HALF is doing --------------------------------------
    // Under each dot that has a wire sits the half's own meter, so "the video
    // arrives but the audio does not" is visible on the canvas instead of two
    // wires that look equally alive — and each half is read with the control the
    // app already owns for that kind of signal:
    //
    //   VIDEO half  a thin bar (LevelTrack). A picture has no level, so it is
    //               full while frames arrive and empty when they stop:
    //                 bus      its first routed video source is delivering
    //                 sink     its own runtime (outputRuntime / outputFeedStalled),
    //                          the same measured fact its Screens row prints
    //   AUDIO half  the app's L/R pair (ChannelPill) — the SAME control a source
    //               card and a bus card carry in Media › Inputs, so a side reads
    //               (and closes) identically wherever it is shown. Its level is
    //               the bus's stereo mix (busChannelMix), and a sink's pair reads
    //               the bus wired into it, which is the only audio a sink has.
    //
    // The pair now STRADDLES the card's dot — L above it, R below it — because the
    // dot itself moved to the card's centreline (see portOffset). A port is still
    // one dot; the two pills are the meter/trim beside it, not a second port.
    readonly property int stripW: 30
    readonly property int stripH: 4
    readonly property int pillSize: 16
    // Wide enough for the 9 px dot to sit between the two pills with a hair of
    // clearance on each side, so the dot stays visible and grabbable.
    readonly property int pillGap: 12
    readonly property int pillPairHeight: root.pillSize * 2 + root.pillGap

    readonly property var inputMeters: EngineBridge.inputLevels
    // The bridge's published snapshot for one device label (null when it has no
    // tap or no device behind it — an empty label is REFUSED by the bridge and
    // must never match whatever was published under "").
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
    // A bus's own STEREO mix, per side, computed exactly the way the Media pane's
    // bus card computes it (that card is where the operator's model of this was
    // worked out, so the two can never disagree): the loudest routed audio input
    // per channel, times that input's own gain and fader, times the bus's level,
    // times the bus's own per-side trim. Returns [L, R] in 0..1.
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
        // A VIDEO row routed into this bus carries its own audio (a media file's
        // track, NDI's embedded audio) and the engine's mixer sums it into the
        // bus — so the bus's own pair must read it too, or a bus whose only
        // source is a video row sat at zero while it was audibly open. Same
        // rules as Audio & Video's videoChannelLevels: a camera or screen feed
        // carries no audio at all, a muted row is silence, each side takes its
        // own trim and the row's fader, and a mono stream's single peak feeds
        // both sides.
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

    // One VIDEO strip per wired video half, in the gutter beside its own dot.
    // Geometry only — the value lives in each delegate's binding (stripValue), so
    // a frame tick does not rebuild this list twenty times a second.
    readonly property var signalStrips: {
        root.modelsRev
        const out = []

        // No video level strips: no card draws one under its video dot any more.
        return out
    }

    // One L/R pair per wired audio half, straddling its own dot: L above the
    // centreline dot, R below it (the pair is centred on that dot, and pillGap is
    // wide enough for the dot to sit between the two). `bus` is the BUS ROW whose
    // stereo mix the pair reads and whose per-side trim its clicks write: on a bus
    // that is the bus itself, and on a screen or output it is the bus wired into
    // it — a sink has no audio of its own, so the pair there is the feeding bus's
    // own trim seen from the other end of the same wire (one fact, one model;
    // nothing is duplicated).
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
                const buses = root.audioBusesOf(o)
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
    // The buses one L/R pair stands for: a roster row's single bus, or every bus
    // wired into the speaker endpoint. Reading through this is what lets the pair
    // on the sum sink mean "the program mix" without a second control.
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
    // One gesture, every bus behind it: closing a side of the speaker pair closes
    // that side on each bus feeding it (the only reading that makes "the left of
    // the speakers is closed" true), and opening does the reverse. A bus already
    // in the wanted state is left alone so a mixed pair is not needlessly flipped.
    function togglePill(pair, channel) {
        const buses = root.pillBuses(pair)
        const open = root.pillGain(pair, channel) > 0
        for (let i = 0; i < buses.length; ++i) {
            const gain = BusListModel.channelGain(buses[i], channel)
            if (open ? gain > 0 : gain <= 0)
                BusListModel.toggleChannelGain(buses[i], channel)
        }
    }
    // What the dot is receiving, per half: "video only" and "audio only" are
    // half-wired outputs, which the single "silent" badge of a blind card could
    // never show.
    function receivesLabel(o) {
        // A screen feeding this sink is video arriving, exactly like a video bus.
        const videoBuses = root.videoBusesOf(o)
        const video = videoBuses.length > 0 || root.screenFeedsOf(o).length > 0
        const buses = root.audioBusesOf(o)
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
    function styleSubtitle(style) {
        const template = String(style.templateKey || "")
        if (template.length > 0) return template
        const kind = String(style.contentType || "")
        return kind.length > 0 ? kind : qsTr("Style")
    }
    // A style's own background as its chip colour. "" and "transparent" both
    // mean "no colour of its own", which is why they resolve to a transparent
    // swatch (the card then draws its glyph instead).
    function styleSwatch(style) {
        const colour = String(style.backgroundColor || "")
        return colour.length > 0 ? colour : "transparent"
    }
    // The one-line state under a screen's name: the style it wears, or the
    // design's own "No signal" when nothing is mapped to it.
    //
    // WIRED IS NOT LIVE. `feedLive` is the render engine's own answer (did this
    // output's buffer receive a frame in the last moment? — see
    // RoutingService::refreshFeedHealth), and a wired row with no frames is the
    // one state worth shouting about: it is exactly the case that used to read as
    // healthy because the wire existed.
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
    // A GATE CHIP WAS PRESSED on an output card (RoutingNodeCard::gateToggled).
    // The card reports its own key; the row is the tail of it. `on` reads from the
    // row, not from the chip, so a press always flips the STORED flag even if the
    // card's copy is a frame behind.
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

                            // The swatch repeats the wire's own shape — one bar
                            // for a signal, three dashes for a style link — so
                            // the legend cannot describe a line the canvas draws
                            // differently.
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
            contentWidth: board.width
            contentHeight: board.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            // A connect-drag owns the gesture end-to-end — the page must not
            // scroll away under the pointer (the ports' preventStealing is the
            // other half of this).
            interactive: !root.dragging

            Item {
                id: board
                width: root.boardW
                // A picker is CONTENT, not a popover that may hang off the canvas:
                // its panel is taller than a nearly-empty column, so the board grows
                // with an open picker instead of clipping the last entries.
                height: Math.max(root.boardH,
                                 addPicker.showing ? addPicker.y + addPicker.height + Theme.space3
                                                   : 0)

                // ---- Column headers ----
                Repeater {
                    model: [
                        // The order is the layer order the design asks for, left
                        // to right: signal sources, the looks, the surfaces that
                        // wear them, then the sinks.
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
                        height: 30
                        radius: Theme.radiusSm
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

                        // THE column's add control. Small, in the header, one per
                        // column: it opens that layer's own picker / form, so a
                        // layer is still created where it is wired — it just no
                        // longer costs the canvas a row of cards to say so.
                        // Screens has no "+" yet: its screens are made on Settings · Screens.
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
                        // THE SAME CURVE MATHS THE DRAG GHOST USES (see
                        // draggedWire): plotted from the LEFT end, each handle to
                        // the OUTSIDE of its own end, so a wire reads identically
                        // whichever end drew it. Nothing here may assume x1 < x2 —
                        // that is what made the reverse drag hook around the
                        // cursor, and the same assumption on the committed line
                        // would hook a wire the moment its ends swapped.
                        readonly property bool reversed: modelData.x2 < modelData.x1
                        readonly property real fromX: reversed ? modelData.x2 : modelData.x1
                        readonly property real fromY: reversed ? modelData.y2 : modelData.y1
                        readonly property real toX: reversed ? modelData.x1 : modelData.x2
                        readonly property real toY: reversed ? modelData.y1 : modelData.y2
                        readonly property real bend: Math.max(24, Math.abs(toX - fromX) * 0.5)
                        // WIRED, BUT NOT THE PICTURE GOING OUT. A row can hold
                        // several video buses and shows one of them; the others are
                        // drawn dashed and fainter, so "carried" and "on air" stop
                        // looking like the same line. Clicking one shows it.
                        readonly property bool offAir: modelData.onAir === false

                        opacity: dimmed ? 0.16 : (offAir ? 0.5 : 1)

                        Behavior on opacity { NumberAnimation { duration: 120 } }

                        ShapePath {
                            strokeColor: root.wireColor(modelData.kind)
                            strokeWidth: highlighted ? 3.4 : 2.2
                            fillColor: "transparent"
                            capStyle: ShapePath.RoundCap
                            // A style link is not a signal, a gate wire is a
                            // permission rather than a picture, and a video wire
                            // that is not the one on air is not the picture going
                            // out: dashed, all three.
                            strokeStyle: modelData.kind === "style"
                                         || modelData.kind === "gate"
                                         || wireShape.offAir
                                         ? ShapePath.DashLine : ShapePath.SolidLine
                            dashPattern: [4, 4]
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

                // LEFT-click = SHOW this bus here (a wired video wire only).
                // A TapHandler, not a widening of the MouseArea above: a
                // left-accepting MouseArea covering the canvas would press on
                // every left button-down and starve the canvas's own drags and
                // the drags that start on a port. This claims only a completed
                // tap, so a press that turns into a drag falls straight through.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    enabled: !root.dragging
                    onTapped: (eventPoint) => root.showWireAt(eventPoint.position.x,
                                                              eventPoint.position.y)
                }

                // The two MEMBERSHIP menus (see beginAdd): under the column's own
                // "+", so the list is unmistakably that column's, and only rows
                // not on the board are offered (plus one create entry).
                // Opening/placing is NOT done here — `visible` stays false and
                // openAt (page-level onAddMenuChanged) puts the panel under its
                // header. `matchAnchorWidth` makes the menu exactly the column's
                // width, so it reads as that column's list rather than a slab
                // over the wiring; the tighter row keeps the whole menu about the
                // height of the cards it adds.
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

                // The catcher dismisses a panel by writing `visible`; the page's
                // own state has to follow, or the column's "+" would stay lit and
                // the next press would look like it did nothing. A Connections
                // and not an `onVisibleChanged` on the panel: DropdownPanel's own
                // handler is what claims the pointer (AppCursor) while a menu is
                // open, and an inline handler would REPLACE it.
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
                // Video / Both / Audio / Template, the same markers the Outputs
                // column wears: a roster that mixes a camera, a mic and a
                // template bus is one long list you have to read four times, and
                // the kind is the first thing an operator sorts by.
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
                        // "unrouted" is about the BUS's own input: a bus nothing
                        // feeds carries silence no matter how many outputs are
                        // wired to it, and that is worth seeing here.
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
                        // A row with no resolution (an NDI sender, a stream) must
                        // not print a dangling separator: "NDI · " alone reads as
                        // missing information that is not missing.
                        subtitle: (modelData.kind ? modelData.kind : "")
                                  + (modelData.res ? " · " + modelData.res : "")
                        iconName: "presentation"
                        accent: Theme.wireVideo
                        // THE GATES THEMSELVES, on the card (RoutingNodeCard::gateChips):
                        // one press flips this row's own overlay or media flag, so an
                        // operator does not have to open a menu to answer "why is this
                        // screen not showing the overlay".
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
                // LAYERS first: the two gate cards (Overlays, Media — see
                // gateCards). They are WIRE SOURCES: their output dot leaves the
                // Styles column's right edge, and the wire that lands on an output
                // is that output's permission to paint the layer. The card itself is
                // not clickable — the line is the state, exactly as a bus's wire is
                // its routing.
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
                        // Lit once ANY output is allowed through it, muted when no
                        // wire leaves it — the same "wired / not wired" reading every
                        // other node on this canvas gives.
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

                // STYLE TEMPLATES beneath: the design an output paints, wired
                // from the same right edge. The marker is what separates a layer's
                // permission from a template's look.
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
                // Audio / Network / Physical / Recording / Streaming: the operator
                // looks for a TRANSPORT ("where is the SDI?"), so the column says
                // which kind each run of cards is instead of showing one long
                // undifferentiated list.
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
                        // The port's own colour — the endpoint is an AUDIO sink now,
                        // so it reads purple like the wire that lands on it instead
                        // of grey like something that takes nothing.
                        accent: (modelData.acceptsAudio && modelData.acceptsVideo) ? Theme.accent
                              : modelData.acceptsAudio ? Theme.wireAudio : Theme.wireVideo
                        status: root.receivesLabel(modelData)
                        // ...and the same two chips, on every sink that is an output of
                        // ours. The speaker endpoint (system) is the engine's node, not
                        // a roster row, so it carries no gate to flip.
                        gateChips: !modelData.system
                        overlayOn: modelData.showOverlay !== false
                        mediaOn: modelData.showMedia !== false
                        onGateToggled: (hoverKey, gate) => root.toggleCardGate(hoverKey, gate)
                        // Outputs say WHAT is arriving as a compact badge, so the
                        // transport line beside the name is never cut for it.
                        statusBadge: true
                        // A sink fed only by a SCREEN is fed, and a sink the engine
                        // is delivering frames to reads ordinary; the colour now
                        // also carries FEED HEALTH, so "wired but nothing arriving"
                        // is visible on this card too (see sinkStatusColor).
                        statusColor: root.sinkStatusColor(modelData)
                        // Every sink is a drop target for the signals it carries —
                        // the speaker endpoint included, on its audio port alone
                        // (pairOk asks outputAccepts, which answers from the row).
                        dropValid: root.dragging && root.dragFrom !== key
                                   && root.pairOk(root.dragFrom, root.dragKind, key)
                        dropInvalid: root.dragging && root.dragFrom !== key
                                     && !root.pairOk(root.dragFrom, root.dragKind, key)
                        // The two transports you start and stop carry their own
                        // transport, aimed at THIS card's node — the same node the
                        // canvas wires buses into, so "record what is wired here"
                        // needs no second selection.
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
                // One panel, positioned beside the tile that opened it, drawn in
                // the shared DropdownPanel's visual language (card ground, border,
                // hover row) rather than a second menu style.
                Rectangle {
                    id: addPicker
                    readonly property var entries: root.addMenu === "output" ? root.outputKinds : []
                    readonly property bool showing: root.addMenu.length > 0

                    visible: showing
                    z: 25
                    width: 304
                    height: pickerCol.height + Theme.space2
                           + (root.addMenu === "output" ? pickerFoot.height + Theme.space2 : 0)
                           + Theme.space3 * 2
                    // Anchored to the HEADER it was opened from, at the full height it
                    // needs: the panel is a menu, not a popover that has to fit above a
                    // card that may no longer be there.
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
                                    onClicked: root.addOutput(modelData.kind)
                                }
                            }
                        }
                    }

                    // The one thing the picker cannot offer: there is exactly one
                    // speaker endpoint and it is already on the canvas — adding a
                    // second would be a second name for the same device.
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

                // A press anywhere off the picker closes it — the same "click
                // away and it is gone" a menu owes, without a modal. Declared
                // above the picker's own z so the picker keeps its clicks.
                MouseArea {
                    anchors.fill: parent
                    z: 20
                    visible: root.addMenu.length > 0
                    enabled: visible
                    onClicked: root.addMenu = ""
                }

                // ---- VIDEO level strips ---------------------------------------
                // The bar alone (LevelTrack's own dot is off — the canvas already
                // draws the port dot) in the video wire's colour: full while frames
                // flow, empty when they stop. Declared BEFORE the ports so the dots
                // and the wires stay on top of both these and the pills below.
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
                        // A picture has no level to read: one flat colour that is
                        // full while frames arrive and empty when they stop. (The
                        // audio half's level is the L/R pill pair beside its dot.)
                        fixedColor: true
                        fillColor: modelData.color
                    }
                }

                // ---- AUDIO level: the app's L/R pair --------------------------
                // ChannelPill — the SAME control a source card and a bus card carry
                // in Media › Inputs, so a side glows, trims and closes identically
                // here instead of growing a canvas-only meter. Stacked vertically,
                // and centred on the audio dot: L sits above the dot and R below it,
                // which is where they were before the dot itself moved to the middle
                // of the card (see portOffset).
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
                                // The BUS's own per-side trim — on a screen or
                                // output that is the bus wired into it, and on the
                                // speaker endpoint it is every bus feeding it (see
                                // pillGain: a side is open when it is open
                                // somewhere).
                                gain: {
                                    void root.modelsRev
                                    return root.pillGain(pillPair.modelData, modelData)
                                }
                                // The summed mix, shaped the way every live meter
                                // in the app shapes it (sqrt of the raw mix): a
                                // near-silent source still reads as a glow
                                // instead of vanishing.
                                level: root.pillLevel(pillPair.modelData, modelData)
                                // One gesture, every bus behind the pair: closing
                                // a side of the speakers closes it wherever it is
                                // open, so the control cannot disagree with the
                                // reading beside it.
                                onToggled: Qt.callLater(() =>
                                    root.togglePill(pillPair.modelData, modelData))
                            }
                        }
                    }
                }

                // ---- Ports: one list, one delegate. A card's dots are drawn at
                // the same coordinates its wires end on (portPoint), so a dot and
                // its line can never drift apart, and a card cannot end up with a
                // dot for a signal it does not carry (the ports list decides).
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
                        // A live drag lights the dot it left and the dot it would
                        // land on. Keyed by CARD, not by kind: most cards own a
                        // single dot whose signal is resolved on drop (dropKind),
                        // so matching on the kind would light nothing at all.
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

                    // THE GHOST IS THE WIRE IT IS ABOUT TO BECOME, so it is
                    // plotted the way the wire layer plots one: LEFT to right,
                    // with each handle to the OUTSIDE of its own end (a bus's dot
                    // to a sink's dot). The ends swap when the drag runs
                    // backwards — pulled from a sink's dot back towards a bus —
                    // because keeping both handles pinned to the drag direction
                    // hooked the curve around the cursor instead of following it:
                    // the line went stiff exactly while you were dragging the way
                    // the canvas is really read (right to left).
                    readonly property bool reversed: root.dragX < root.dragStartX
                    readonly property real fromX: reversed ? root.dragX : root.dragStartX
                    readonly property real fromY: reversed ? root.dragY : root.dragStartY
                    readonly property real toX: reversed ? root.dragStartX : root.dragX
                    readonly property real toY: reversed ? root.dragStartY : root.dragY
                    readonly property real bend: Math.max(24, Math.abs(toX - fromX) * 0.5)

                    ShapePath {
                        // Drawn in the colour of the signal it carries, and
                        // neutral the moment it is over nothing it can land on —
                        // the canvas never promises a wire it will not make.
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

                // (An empty column needs no note: every column's header carries its
                // own "+", so "nothing here yet" is answered by the one control
                // that fixes it.)
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
    // The SHARED menu pair every roster in Settings uses (MenuCatcher dismisses
    // it on any outside click or scroll, DropdownPanel draws it), so a
    // right-click here behaves like one on a screen row instead of a
    // canvas-only interaction nobody else has to learn.
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
        // `item` is the row's own entry, so the action is picked by ID, not by
        // comparing translated labels.
        onItemActivated: (label, item) => root.actOnCard(item && item.id ? String(item.id) : "")
    }

    // Deleting a sink is destructive and leaves nothing to undo (removing a bus
    // does — the model carries its own restore). Confirm it with the shared
    // dialog rather than dropping a row on a single stray click.
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
    // The same picker Media › Inputs' "New bus" opens, so a bus made from this
    // canvas is made the one way buses are made: source first, bus second, the
    // route between them already in the graph. The picker never closes itself
    // (its header says so) — the caller closes it on both outcomes.
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
    // Add (from the "+" picker, HDMI) and Edit (a card's own menu) share it, so
    // there is one dialog for "what is this output": its name, its mode, the
    // test pattern, and — for a physical output — the display it leaves on and
    // the two window rules that decide how it sits there. Cancel on an ADD
    // leaves nothing behind: the row is created by Save, not by the pick.
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

        // The form Settings · Screens uses — the display picker inside it lists
        // the real QScreens, which is the "external monitor" choice this dialog
        // exists for. `showType` is off: the transport was chosen in the picker
        // that opened this, and it is the row's identity from then on.
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
                // A picked display brings its MODE with it, exactly as it does
                // on Settings · Screens: the row starts at the resolution and
                // refresh that screen is actually running, not at 1080p/60.
                if (res !== "") {
                    root.outRes = res
                    root.outRefresh = refresh
                }
            }
            onLockToggled: (l) => root.outLocked = l
            onPatternPicked: (k) => root.outPattern = k
        }

        // WHAT THE TWO FIELDS MEAN FOR A SENDER. An NDI output is discovered by
        // the NAME its network sender announces and is watched at the mode it
        // sends, both of which this form already sets (the sender is named after
        // the output row — see LiveOutputService's ndiSenderDisplayName), so the
        // dialog says so instead of leaving "resolution" reading like something
        // about a monitor it does not have.
        Text {
            width: parent.width
            visible: root.outKind.toUpperCase() === "NDI"
            text: qsTr("Other machines see this name on the network, and the picture is sent at the resolution and frame rate above.")
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }

        // The one thing ScreenForm does not carry: whether the real output
        // window stays above other windows. Physical outputs only — a sink with
        // no window has nothing to keep on top.
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
