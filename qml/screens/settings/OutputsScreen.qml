import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Screens — the screens a show is sent to: the roster (Add / Edit /
// Duplicate / Delete), each screen's live state and its on/off toggle. It reads
// the same C++ singletons the Edit screen's monitor wall renders
// (OutputListModel), so a rename, duplicate or delete here is reflected
// everywhere at once.
//
// WHAT THIS PAGE NO LONGER OWNS. Two things moved to Settings · Routing, and
// they are gone from here on purpose:
//   • the KIND. A row on this page is a SCREEN — an HDMI display surface. The
//     transports that are not screens (NDI, SDI, recording, streaming) are the
//     Routing canvas's OUTPUTS, where you add one and wire its video and audio
//     buses. Picking a kind belonged with the wiring, not with the screen it
//     ended up on.
//   • the STYLE. A look is a wire on that canvas (drag a screen onto a style) —
//     the same style id the engine composes with, set where the routing is
//     visible, instead of a pill on a roster row.
// Both are still real OutputListModel state; this page just stopped being the
// place that decides them, so there is one editor for each fact.
Item {
    id: root

    // Index currently open in the edit dialog; -1 = closed.
    property int editIndex: -1
    // Index pending deletion (confirm dialog); -1 = none.
    property int deleteIndex: -1
    // Index the right-click context menu was opened for; -1 = closed.
    property int contextMenuIndex: -1
    property string defaultMediaFit: "contain"

    // ---- "Edit screen" asked for from Settings · Routing --------------------
    // The canvas right-clicks a card; a screen's editor is THIS page's, so the
    // shell switches here with the row it wants open. -1 = nothing pending.
    property int editRequestRow: -1
    // Told to the shell once the request has been acted on, so a LATER visit to
    // this page does not pop the same editor again (the shell owns the value; a
    // page that reset only its own copy would reopen the dialog next time).
    signal editRequestConsumed()
    function consumeEditRequest() {
        if (root.editRequestRow < 0)
            return
        const row = root.editRequestRow
        root.editRequestConsumed()
        // The shell clears its own pending value, which drives this property back
        // to -1 through its binding — and the binding must SURVIVE so the next
        // request still arrives. Only a page nobody wired (no shell) resets it
        // itself, once, here.
        if (root.editRequestRow === row)
            root.editRequestRow = -1
        if (row < OutputListModel.rowCount())
            root.openEdit(row)
    }
    onEditRequestRowChanged: root.consumeEditRequest()

    Component.onCompleted: {
        const saved = String(SettingsService.value("outputs.defaultMediaFit") ?? "contain")
        root.defaultMediaFit = ["contain", "cover", "fill"].indexOf(saved) >= 0 ? saved : "contain"
        LiveOutputService.setDefaultMediaFit(root.defaultMediaFit)
        // A row asked for before this page existed (the canvas set it, then the
        // shell switched sections) opens as soon as the page does.
        root.consumeEditRequest()
    }

    function chooseDefaultMediaFit(fit) {
        if (["contain", "cover", "fill"].indexOf(fit) < 0)
            return
        root.defaultMediaFit = fit
        SettingsService.setValue("outputs.defaultMediaFit", fit)
        LiveOutputService.setDefaultMediaFit(fit)
    }

    // ---- NDI firewall authorization (see EngineBridge::
    // requestNdiFirewallAccess) ------------------------------------------
    // Asked at most once per session, only while the firewall rule is
    // actually missing: the RULE is the persisted permission, so once it is
    // in place (or if the user refused the OS prompt, once they revisit)
    // this re-fires exactly when it should and never otherwise. The
    // elevated verdict is confirmed by a short retry timer.
    //
    // BOTH HALVES ARE ASYNC. Asking is a hidden `netsh` child process and so
    // is every probe of the retry timer — work that must not ride inside a
    // click — so the outcome arrives on ndiFirewallAccessResult and each
    // tick's verdict on ndiFirewallRuleChecked. These two handlers are the
    // whole of what used to be "read the return value".
    property bool firewallPending: false
    // Probe cap: each tick runs a netsh child process — unbounded retries
    // while a UAC dialog sits unanswered (or was denied) would spawn one
    // every 2 s for as long as this screen stays open. 15 tries (~30 s)
    // then give up with an honest notice; the manual re-run remains in
    // this screen. Counted on the ASK side as well as against an answer, so
    // a lagging verdict can never overshoot the cap.
    property int firewallProbes: 0
    function giveUpNdiFirewall() {
        if (!root.firewallPending)
            return
        root.firewallPending = false
        firewallConfirmTimer.stop()
        EventBus.notify(qsTr("Still waiting on the Windows Firewall decision — NDI output may not reach other computers until it is allowed. Re-run from Settings · Screens."),
                        "warning", qsTr("NDI firewall"), "outputs.ndi.firewall.pending")
    }
    function handleNdiFirewallRule(allowed) {
        if (!root.firewallPending)
            return   // a verdict for a probe the screen already gave up on
        if (allowed) {
            root.firewallPending = false
            firewallConfirmTimer.stop()
            EventBus.notify(qsTr("Windows Firewall now allows NDI receivers to connect — other computers can find this app's output."),
                            "success", qsTr("NDI ready"), "outputs.ndi.firewall.added")
            return
        }
        if (root.firewallProbes >= 15)
            root.giveUpNdiFirewall()
    }
    function handleNdiFirewallResult(outcome) {
        if (outcome === "allowed")
            return   // rule already in place: no banner, no toast, nothing to grant
        if (outcome === "added") {
            EventBus.notify(qsTr("Windows Firewall is set — NDI receivers on other computers can connect."),
                            "success", qsTr("NDI ready"), "outputs.ndi.firewall.added")
        } else if (outcome === "prompt") {
            root.firewallPending = true
            root.firewallProbes = 0
            EventBus.notify(qsTr("Allow the Windows Firewall request so other computers can receive this app's NDI output."),
                            "info", qsTr("NDI ready"), "outputs.ndi.firewall.prompt")
        } else if (outcome === "denied" || outcome === "failed") {
            EventBus.notify(qsTr("Windows Firewall wasn't set — NDI output may not reach other computers. Re-run it from Settings · Screens."),
                            "warning", qsTr("NDI firewall"), "outputs.ndi.firewall.failed")
        }
        // "unavailable" (non-Windows): silent — no firewall concept to grant.
    }
    Connections {
        target: EngineBridge
        function onNdiFirewallAccessResult(outcome) { root.handleNdiFirewallResult(outcome) }
        function onNdiFirewallRuleChecked(allowed) { root.handleNdiFirewallRule(allowed) }
    }
    Timer {
        id: firewallConfirmTimer
        interval: 2000
        repeat: true
        running: root.firewallPending
        onTriggered: {
            // The CAP is enforced on the asking side: a probe whose verdict is
            // still in flight must not let the timer spawn another child
            // process every 2 s (the answer side gives up as well, so the
            // notice goes out whichever arrives first).
            if (root.firewallProbes >= 15) {
                root.giveUpNdiFirewall()
                return
            }
            root.firewallProbes++
            EngineBridge.checkNdiFirewallRule()   // verdict → ndiFirewallRuleChecked
        }
    }
    function maybeRequestNdiFirewall() {
        if (EngineBridge.ndiFirewallPrompted)
            return
        // The outcome lands on ndiFirewallAccessResult (see above).
        EngineBridge.requestNdiFirewallAccess()
    }

    // Add-dialog form state (VGRPresenter · Settings · Outputs · Add).
    property bool addShown: false
    property string addName: ""
    property string addType: "HDMI"
    property string addRes: "1920 × 1080"
    property string addRefresh: "60 Hz"
    // Which existing screen the new one is placed against (-1 = none).
    // Physical display the new screen is placed on (QScreen::name,
    // "" = windowed), and whether its assignment starts locked.
    property string addScreenName: ""
    property bool addLocked: false
    // Whether this output's real destination window (OutputWindow.qml)
    // stays above other windows — off by default.
    property bool addStayOnTop: false
    // "smpte" | "gradient" | "checker" | "solidred"
    property string addPattern: "smpte"
    // Where the new row's audio plays ("" = the program stream).
    property string addAudioDevice: ""

    function submitAdd() {
        OutputListModel.addScreen(root.addName, root.addType, root.addRes, root.addRefresh, root.addPattern)
        // Placement/lock/stayOnTop are post-insert setters — the row must
        // exist first.
        if (root.addScreenName !== "" || root.addLocked || root.addStayOnTop
                || root.addAudioDevice !== "") {
            const row = OutputListModel.rowCount() - 1
            OutputListModel.setScreenName(row, root.addScreenName)
            OutputListModel.setBoundsLocked(row, root.addLocked)
            OutputListModel.setStayOnTop(row, root.addStayOnTop)
            // Its own endpoint, if one was picked: the routing pass opens the
            // stream once a bus is wired to this row.
            OutputListModel.setAudioDevice(row, root.addAudioDevice)
        }
        // Reset for next time, close.
        root.addName = ""
        root.addType = "HDMI"
        root.addScreenName = ""
        root.addLocked = false
        root.addStayOnTop = false
        root.addPattern = "smpte"
        root.addAudioDevice = ""
        root.addShown = false
    }

    // Editable copies loaded when the dialog opens — the model is only
    // written on Save, matching the dialog's Cancel/Save contract.
    property string editName: ""
    property string editType: "HDMI"
    // "1920 × 1080", matching the Add dialog's own default (addRes) and
    // ScreenForm's own fallback — an empty default here left the box blank
    // for an output whose resolution was never explicitly set (user report:
    // "it[s] the default value not empty").
    property string editRes: "1920 × 1080"
    property string editRefresh: "60 Hz"
    property string editPattern: "smpte"
    property string editScreenName: ""
    property bool editLocked: false
    // Whether this output's real destination window (OutputWindow.qml)
    // stays above other windows — off by default.
    property bool editStayOnTop: false
    // This row's audio endpoint ("" = the program stream).
    property string editAudioDevice: ""
    // Media fit is a shared default above, with optional per-file overrides;
    // it is not stored separately on each output.
    property string editDefaultMediaFit: "contain"

    function openEdit(index) {
        const data = OutputListModel.getOutput(index)
        root.editIndex = index
        root.editName = data.name
        root.editType = data.kind
        root.editRes = data.res !== "" ? data.res : "1920 × 1080"
        root.editRefresh = data.refresh !== "" ? data.refresh : "60 Hz"
        root.editPattern = data.testPattern !== "none" ? data.testPattern : "smpte"
        root.editScreenName = data.screenName
        root.editLocked = data.boundsLocked
        root.editStayOnTop = data.stayOnTop
        root.editAudioDevice = data.audioDevice !== undefined && data.audioDevice !== null
            ? String(data.audioDevice) : ""
        root.editDefaultMediaFit = root.defaultMediaFit
        root.editOriginalName = data.name
    }

    // The Edit dialog saves the row it was OPENED for, by identity, not by the
    // row index captured then: the roster is re-hydrated from the engine store
    // on boot completion (and on any other reload), which renumbers rows, and a
    // stale index then wrote the dialog's values onto a different output (or
    // nothing at all) — the reported "Save does nothing, the dialog stays open".
    property string editOriginalName: ""
    function editRow() {
        if (root.editIndex >= 0) {
            const here = OutputListModel.getOutput(root.editIndex)
            if (here.name === root.editOriginalName)
                return root.editIndex
        }
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            if (OutputListModel.getOutput(i).name === root.editOriginalName)
                return i
        }
        return -1
    }

    function saveEdit() {
        if (root.editIndex < 0)
            return
        const row = root.editRow()
        if (row < 0) {
            // The output this dialog was opened for is gone (deleted in another
            // view): say so and close — an open dialog with a dead Save button
            // is the bug being fixed here.
            EventBus.notify(qsTr("That output no longer exists, so nothing was saved."),
                            "error", qsTr("Output not saved"), "outputs.save.missing")
            root.editIndex = -1
            return
        }
        root.editIndex = row
        OutputListModel.renameOutput(root.editIndex, root.editName)
        // The KIND is deliberately not written any more (it is chosen and
        // changed on Settings · Routing, beside the buses that feed it), so a
        // save here can never quietly re-transport a row — the round-trip that
        // used to resurrect the HDMI window on a row switched to NDI is gone
        // with the picker. Empty editScreenName on a form that never touched
        // the picker must also not CLEAR a valid binding, so it is normalized
        // to the row's current one before writing.
        if (root.editScreenName === "")
            root.editScreenName = OutputListModel.getOutput(root.editIndex).screenName
        OutputListModel.setScreenName(root.editIndex, root.editScreenName)
        OutputListModel.setResolution(root.editIndex, root.editRes)
        OutputListModel.setRefresh(root.editIndex, root.editRefresh)
        OutputListModel.setTestPattern(root.editIndex, root.editPattern)
        OutputListModel.setBoundsLocked(root.editIndex, root.editLocked)
        OutputListModel.setStayOnTop(root.editIndex, root.editStayOnTop)
        // The endpoint this row's mix leaves on. Written last among the routing
        // fields because setAudioDevice re-asks the routing pass, which reads the
        // row as it now stands.
        OutputListModel.setAudioDevice(root.editIndex, root.editAudioDevice)
        // The STYLE is not written here either: a screen's look is the style
        // wire on Settings · Routing, and this dialog no longer carries a
        // control that could silently restyle a screen on an unrelated save.
        root.chooseDefaultMediaFit(root.editDefaultMediaFit)
        root.editIndex = -1
    }

    // Bumped on any change to the outputs model so derived values recompute.
    // Plain Q_INVOKABLE reads in bindings aren't tracked, so the revision
    // counter is the honest way.
    property int modelsRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }

    // ---- What Settings · Routing has wired to the row being edited ----------
    // The canvas is the ONE editor for a wire, so this page only REPORTS one:
    // every label below is read from RoutingService's published snapshot (the
    // canvas's own truth), never from a copy kept here. `revision` is that
    // snapshot's version, so making or cutting a wire on the canvas re-derives
    // these labels on the next binding pass.
    function busLabel(busRow) {
        if (busRow < 0)
            return qsTr("not wired")
        const bus = BusListModel.getBus(busRow)
        const name = bus && bus.name ? String(bus.name) : ""
        return name.length > 0 ? name : qsTr("Bus %1").arg(busRow + 1)
    }
    function styleLabel(styleId) {
        if (!styleId || String(styleId).length === 0)
            return qsTr("none")
        const at = StyleListModel.rowForId(String(styleId))
        if (at < 0)
            return qsTr("missing (deleted)")
        const style = StyleListModel.getStyle(at)
        const name = style && style.name ? String(style.name) : ""
        return name.length > 0 ? name : qsTr("Style %1").arg(at + 1)
    }
    readonly property var editRouting: {
        void RoutingService.revision
        const rows = RoutingService.rows
        for (let i = 0; i < rows.length; ++i) {
            const entry = rows[i]
            if (Number(entry.row) !== root.editIndex)
                continue
            return {
                video: root.busLabel(Number(entry.videoBus)),
                // A row that does not accept audio cannot be "unwired" for it —
                // saying "not wired" on an HDMI screen would read as a fault
                // the operator could fix by dragging, and there is nothing to
                // drag onto it.
                audio: entry.acceptsAudio ? root.busLabel(Number(entry.audioBus))
                                           : qsTr("not carried"),
                style: root.styleLabel(String(entry.styleId || "")),
                wired: !!entry.wired
            }
        }
        return { video: qsTr("not wired"), audio: qsTr("not carried"),
                 style: qsTr("none"), wired: false }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + Theme.space6 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            // Below the fixed title/Add-Output header (pinned to root, same
            // as addOutputBtn) so the tiles flow underneath it instead of
            // scrolling the title away with them.
            y: Theme.space6 * 2 + addOutputBtn.height
            width: flick.width - Theme.space6
            spacing: Theme.space6

            // ---- Screens list ----
            Repeater {
                model: OutputListModel

                delegate: Rectangle {
                    id: card
                    // Named so a self-test scenario can read a ROW the way it is
                    // rendered (its style note included) instead of only through
                    // the model — see the VGR_OUTS_TEST block in Main.qml.
                    objectName: "selfTestOutputRow_" + card.index
                    required property int index
                    required property string name
                    required property string badge
                    required property string kind
                    required property string res
                    required property bool active
                    required property bool isEnabled
                    required property string screenName
                    required property bool onAirOnly

                    // Each enabled NDI output owns an independent sender/feed.
                    readonly property bool ndiFeedIsThisRow: {
                        const state = LiveOutputService.ndiOutputStates[card.name]
                        return card.kind === "NDI" && !!state && state.sending === true
                    }
                    readonly property bool ndiWarmingIsThisRow: {
                        const state = LiveOutputService.ndiOutputStates[card.name]
                        return card.kind === "NDI" && !!state && state.warming === true
                    }
                    // LIVE only while a GO LIVE session is actually running:
                    // an NDI row whose frames verifiably flow right now, an
                    // HDMI row that OutputWindowManager/OutputWindow actually
                    // puts a window on (OutputWindow.qml's own `visible` is
                    // the ground truth: enabled + bound to a screen + live —
                    // ALL such rows go live simultaneously per
                    // OutputWindowManager.qml's documented behavior, not just
                    // the one selected in this screen), or — for kinds with
                    // no screen window of their own (SDI) — the selected row
                    // during a live session, same as before.
                    readonly property bool live: ndiFeedIsThisRow
                        || (card.kind === "HDMI" && card.isEnabled && !card.onAirOnly
                            && card.screenName !== "" && LiveOutputService.live)
                        || (card.kind !== "HDMI" && card.kind !== "NDI"
                            && card.active && LiveOutputService.live)

                    // ---- THIS ROW'S OWN RUNTIME (every kind, not just NDI) --
                    // The live line under a row used to exist only for the kinds
                    // with a sender of their own (NDI, SDI), so an HDMI row said
                    // nothing about itself while an NDI row reported frames —
                    // read by the operator as "the notification only exists for
                    // one output". Every output has a feed, and the engine
                    // measures the same two facts for all of them
                    // (RenderEngine::OutputFeedAges: frames delivered + how long
                    // since the last present), which LiveOutputService publishes
                    // as outputRuntime. So every row reports the same way.
                    readonly property var runtime: LiveOutputService.outputRuntime[card.name] || null
                    // outputFeedStalled() is an invokable — an invokable alone is
                    // opaque to QML's dependency tracking, so the revision is read
                    // to make this binding re-run when the stall set changes.
                    readonly property bool stalledNow: {
                        void LiveOutputService.feedHealthRev
                        return LiveOutputService.live && card.isEnabled
                            && LiveOutputService.outputFeedStalled(card.name)
                    }
                    readonly property bool delivering: !!card.runtime
                        && card.runtime.presented === true && !card.stalledNow
                    // ONE line for every row, in the kind's own words. Deliberate
                    // states first (switched off / off air), then the feed: no
                    // feed beats a stale frame count, a frame count beats a guess.
                    // The STAGE-WIDE facts are appended to every kind afterwards,
                    // because a blackout is exactly as true of the HDMI screen
                    // as of the NDI feed — that is the whole point of one line
                    // per row instead of one line per kind.
                    readonly property string statusText: {
                        // The KIND is not printed: a row here is a screen, and
                        // what transports its picture is the Routing canvas's
                        // business. What is left is the row's own feed — the
                        // engine's own frame count and stall age.
                        let text = ""
                        if (!card.isEnabled)
                            text = qsTr("Switched off")
                        else if (!LiveOutputService.live)
                            text = qsTr("Ready (go live to send)")
                        else if (card.delivering)
                            text = qsTr("Live · %1 frames delivered").arg(card.runtime.frames)
                        else
                            text = qsTr("Warming up · waiting for the first frame")
                        if (card.isEnabled && LiveOutputService.blackedOut
                                && text.indexOf("blackout") < 0)
                            text += " · " + qsTr("blackout up")
                        if (card.isEnabled && card.stalledNow && text.indexOf("no feed") < 0)
                            text += " · " + qsTr("no feed")
                        return text
                    }
                    // Where this screen's picture comes from: the video bus
                    // Settings · Routing has wired to this row (the canvas is
                    // the one editor for that wire). `revision` is the routing
                    // snapshot's version, so a wire made or cut there re-derives
                    // this line.
                    readonly property int videoBusRow: {
                        void RoutingService.revision
                        const rows = RoutingService.rows
                        for (let i = 0; i < rows.length; ++i) {
                            const entry = rows[i]
                            if (Number(entry.row) === card.index && entry.acceptsVideo)
                                return Number(entry.videoBus)
                        }
                        return -1
                    }
                    readonly property bool hasVideoFeed: card.videoBusRow >= 0
                    readonly property string feedText: card.hasVideoFeed
                        ? qsTr("Fed by %1").arg(root.busLabel(card.videoBusRow))
                        : qsTr("No video wired")

                    readonly property color statusColor: !card.isEnabled
                        ? Theme.textMuted
                        : (card.stalledNow ? Theme.dangerLight
                            : (card.ndiFeedIsThisRow || card.delivering ? "#3ddc84"
                                : (card.ndiWarmingIsThisRow ? Theme.warning : Theme.textMuted)))

                    width: layout.width
                    height: cardCol.height + Theme.space5 * 2
                    radius: Theme.radiusLg
                    color: Theme.card
                    // A disabled screen reads dimmed at a glance.
                    opacity: card.isEnabled ? 1 : 0.45
                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    Column {
                        id: cardCol
                        x: 20
                        y: 20
                        width: parent.width - Theme.space5 * 2
                        spacing: Theme.space3

                        Row {
                            spacing: Theme.space3

                            Pill {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.badge
                                tint: false
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textMd
                                font.weight: Font.Medium
                            }

                        }

                        // The screen's own facts: the display mode it renders
                        // at, and what feeds it. The kind and the style are
                        // wires on Settings · Routing, so the feed below is a
                        // READ of that canvas (feedText), never a second copy
                        // of it.
                        Row {
                            spacing: Theme.space2

                            Text {
                                text: card.res
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }

                            Text {
                                text: "·"
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }

                            Text {
                                text: card.feedText
                                color: card.hasVideoFeed ? Theme.textSecondary : Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }

                        // EVERY OUTPUT'S OWN RUNTIME — real state, not a label
                        // (see statusText above for the per-kind wording).
                        // NDI: the live sender's frame counter while the active
                        // output is sending (the counter only moves when frames
                        // actually flow) with the SDK's connected-monitor count
                        // deciding what "sending" means. SDI: the DeckLink
                        // runtime/device truth from the engine. HDMI and every
                        // other kind: this row's own buffer, counted by the
                        // engine — frames delivered and whether they stopped.
                        Row {
                            spacing: Theme.space2
                            // The once-per-session firewall ask rides the NDI
                            // row's presence: an NDI output on the roster +
                            // a usable runtime is exactly the moment it matters.
                            Component.onCompleted: if (card.kind === "NDI") root.maybeRequestNdiFirewall()

                            // The same dot the monitor wall shows (components/
                            // OutputRuntimeDot.qml): one component owns the state,
                            // the colour and the hover detail (this row's own
                            // numbers on hover), so the two surfaces cannot
                            // describe one output differently. The row keeps its
                            // text; the dot pulses here because a row is a status
                            // surface rather than a picture.
                            OutputRuntimeDot {
                                anchors.verticalCenter: parent.verticalCenter
                                outputName: card.name
                                outputEnabled: card.isEnabled
                                dotSize: 6
                                width: 6; height: 6
                                dotColor: card.statusColor
                                pulse: card.isEnabled && LiveOutputService.live
                                       && (card.ndiFeedIsThisRow || card.ndiWarmingIsThisRow
                                           || card.delivering || card.stalledNow)
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: cardCol.width - 20
                                elide: Text.ElideRight
                                text: card.statusText
                                color: card.statusColor
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }

                    // Status pill + on/off toggle, anchored top-right.
                    // Live = the live red wash (Theme.liveBg, the GO LIVE
                    // pill's own red — a live output is a state, not a fault)
                    // + light-red text; Inactive = flat neutral chip. Pill's `tint` flag
                    // picks which colors apply: tint=true uses
                    // baseColor/lightColor (washed bg + colored text),
                    // tint=false renders the flat neutral chip and IGNORES
                    // baseColor — hence the flip between the two states.
                    // Disabled screens are dimmed and can't go live (model
                    // guards it; the click is inert here too).
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space5
                        anchors.top: parent.top
                        anchors.topMargin: Theme.space5
                        spacing: Theme.space3

                        // Quick edit affordance — same dialog the right-click
                        // menu's Edit opens.
                        AppButton {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Edit"
                            variant: "ghost"
                            onClicked: root.openEdit(card.index)
                        }

                        Pill {
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.live ? "LIVE" : "Inactive"
                            baseColor: Theme.liveBg
                            lightColor: Theme.dangerLight
                            tint: card.live
                            tintAlpha: card.live ? 0.22 : 0.14

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: card.isEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: OutputListModel.setActive(card.index)
                            }
                        }

                        SettingsToggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: card.isEnabled
                            onToggled: {
                                if (!OutputListModel.setEnabled(card.index, !card.isEnabled))
                                    EventBus.notify(qsTr("At least one output card must remain enabled."),
                                                    "warning", qsTr("Outputs"), "outputs.lastEnabled")
                            }
                        }
                    }

                    // Right-click menu — same DropdownPanel pattern as the
                    // slide rows in EditScreen (Edit / Duplicate / Delete).
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        cursorShape: Qt.ArrowCursor
                        onClicked: (mouse) => {
                            screenContextMenu.openAt(card, mouse.x, mouse.y, root)
                            root.contextMenuIndex = card.index
                        }
                    }
                }
            }

        }
    }

    // The header needs its OWN ground, not just a higher paint order: without
    // this, a scrolled-up card shows straight through the title/button (the
    // text read as laid INSIDE the first row instead of pinned above it).
    // Same surface the settings card itself sits on, so there's no seam.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.space6 * 2 + addOutputBtn.height
        color: Theme.surface
    }

    // "Screens" title — pinned alongside the Add-Screen button (sits above
    // the Flickable in paint order, same as that button) so the header
    // stays put while the roster scrolls underneath it, instead of
    // scrolling away with the tiles it used to sit inside.
    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.verticalCenter: addOutputBtn.verticalCenter
        text: "Screens"
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXl
        font.weight: Font.DemiBold
    }

    // "+ Add Screen" pinned top-right — always visible no matter how far
    // the roster scrolls (sits above the Flickable in paint order). The control
    // itself is PrimaryAddButton, shared with the Routing canvas's column
    // footers, so both surfaces create things with the same-looking button (see
    // that file for why it is not plain AppButton).
    PrimaryAddButton {
        id: addOutputBtn
        objectName: "selfTestAddOutput"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space6
        anchors.rightMargin: Theme.space6 + Theme.space2
        text: qsTr("Add Screen")
        onClicked: root.addShown = true
    }

    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }

    // ---- Card context menu (Edit / Duplicate / Delete) ----
    // MenuCatcher under the menu: any click or scroll outside dismisses it
    // instead of leaving it stuck over the page.
    MenuCatcher {
        menu: screenContextMenu
    }

    DropdownPanel {
        id: screenContextMenu
        z: 50
        visible: false
        model: [
            { label: "Edit" },
            { label: "Duplicate" },
            { divider: true },
            { label: "Delete", danger: true }
        ]
        onItemActivated: (label) => {
            switch (label) {
            case "Edit":
                root.openEdit(root.contextMenuIndex)
                break
            case "Duplicate":
                OutputListModel.duplicateOutput(root.contextMenuIndex)
                break
            case "Delete":
                root.deleteIndex = root.contextMenuIndex
                break
            }
            screenContextMenu.visible = false
        }
    }

    // ---- Delete confirm ----
    ConfirmDialog {
        id: deleteDialog
        shown: root.deleteIndex >= 0
        title: "Delete screen?"
        message: {
            if (root.deleteIndex < 0)
                return ""
            const data = OutputListModel.getOutput(root.deleteIndex)
            return "\u201C" + data.name + "\u201D will be removed from the screens roster. This affects every view that renders it."
        }
        confirmLabel: "Delete"
        onConfirmed: {
            OutputListModel.removeOutput(root.deleteIndex)
            root.deleteIndex = -1
        }
        onDismissed: root.deleteIndex = -1
    }

    // ---- Edit dialog ----
    ModalCard {
        id: editDialog
        objectName: "selfTestEditOutputDialog"
        shown: root.editIndex >= 0
        title: "Edit screen"
        cardWidth: 760
        saveText: "Save"
        onCancelled: root.editIndex = -1
        onAccepted: root.saveEdit()

        // Same form the Add dialog shows — one design, one implementation
        // (ScreenForm), seeded with this screen's current values. Placement
        // writes the real display assignment; setScreenName snaps res and
        // refresh to the display's mode on save.
        ScreenForm {
            width: parent.width
            name: root.editName
            type: root.editType
            // No kind picker: a row on this page is a screen, and the
            // transport behind it is wired on Settings · Routing.
            showType: false
            res: root.editRes
            refresh: root.editRefresh
            pattern: root.editPattern
            placement: root.editScreenName
            locked: root.editLocked
            audioDevice: root.editAudioDevice
            onAudioDevicePicked: (id) => root.editAudioDevice = id
            onNameEdited: (t) => root.editName = t
            onResPicked: (v) => root.editRes = v
            onRefreshPicked: (v) => root.editRefresh = v
            onPatternPicked: (k) => {
                root.editPattern = k
                // Saved with the row, but nothing downstream (SceneBuilder,
                // the real OutputWindow) ever reads it to actually paint a
                // pattern — picking one has no real effect yet.
                EventBus.notify(qsTr("Test patterns aren't wired to the output yet — this pick is saved but has no visible effect."),
                                "warning", qsTr("Not implemented"), "outputs.testPattern.notImplemented")
            }
            onPlacementPicked: (s, r, f) => {
                root.editScreenName = s
                if (r !== "") {
                    root.editRes = r
                    root.editRefresh = f
                }
            }
            onLockToggled: (l) => root.editLocked = l
        }

        // Whether this output's REAL destination window (OutputWindow.qml,
        // the borderless window on the bound physical/HDMI screen — not
        // this in-app settings UI) stays above other windows. Off by
        // default: an always-on-top window with no dedicated second
        // monitor buries the app itself.
        Item {
            width: parent.width
            height: 34
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Stay on top"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            SettingsToggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: root.editStayOnTop
                onToggled: root.editStayOnTop = !root.editStayOnTop
            }
        }
        // (The style control lived here. A screen's look is now the style
        // wire on Settings · Routing, beside the buses that feed it — one
        // editor for each fact, and the one where the routing is visible.)

        // ---- What this screen is actually carrying (read-only) ------------
        // Kind and style used to be editable here; both are WIRES now, and the
        // canvas is where a wire is drawn. What is left for this dialog is the
        // thing the operator opened it to check: what feeds this screen,
        // whether it carries sound, and which look it wears — read from the
        // canvas's own snapshot (root.editRouting), so this block can never
        // disagree with the wiring on the Routing page.
        Rectangle {
            width: parent.width
            height: routingCol.height + Theme.space4 * 2
            radius: Theme.radiusMd
            color: Theme.surface

            Column {
                id: routingCol
                x: Theme.space4
                y: Theme.space4
                width: parent.width - Theme.space4 * 2
                spacing: Theme.space2

                Text {
                    text: qsTr("Signal — from Settings · Routing")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }

                Repeater {
                    model: [
                        { label: qsTr("Video"), value: root.editRouting.video },
                        { label: qsTr("Audio"), value: root.editRouting.audio },
                        { label: qsTr("Style"), value: root.editRouting.style }
                    ]

                    delegate: Row {
                        required property var modelData
                        width: routingCol.width
                        spacing: Theme.space3

                        Text {
                            text: modelData.label
                            width: 64
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }

                        Text {
                            // "not wired" / "none" / "not carried" are states
                            // of the wiring, not faults: muted, not alarming.
                            readonly property bool absent: modelData.value === qsTr("not wired")
                                || modelData.value === qsTr("none")
                                || modelData.value === qsTr("not carried")

                            width: parent.width - 64 - Theme.space3
                            text: modelData.value
                            color: absent ? Theme.textMuted : Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Default media fit")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Row {
                spacing: Theme.space2

                Repeater {
                    model: [
                        { value: "contain", label: qsTr("Contain") },
                        { value: "cover", label: qsTr("Cover") },
                        { value: "fill", label: qsTr("Stretch") }
                    ]

                    delegate: Rectangle {
                        id: editFitOption
                        required property var modelData
                        width: Math.max(92, editFitLabel.implicitWidth + 28)
                        height: 34
                        radius: Theme.radiusMd
                        color: root.editDefaultMediaFit === modelData.value
                               ? Qt.alpha(Theme.accent, 0.18) : Theme.surface
                        border.width: root.editDefaultMediaFit === modelData.value ? 1 : 0
                        border.color: Theme.accent

                        Text {
                            id: editFitLabel
                            anchors.centerIn: parent
                            text: editFitOption.modelData.label
                            color: editFitOption.modelData.value === root.editDefaultMediaFit
                                   ? Theme.accentLight : Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            font.weight: Font.Medium
                        }

                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.editDefaultMediaFit = editFitOption.modelData.value }
                    }
                }
            }
        }

    }

    // ---- Add dialog (VGRPresenter · Settings · Screens · Add) ----
    ModalCard {
        id: addDialog
        objectName: "selfTestAddOutputDialog"
        shown: root.addShown
        title: "Add Screen"
        cardWidth: 760
        saveText: "Add Screen"
        onCancelled: root.addShown = false
        onAccepted: root.submitAdd()

        ScreenForm {
            width: parent.width
            name: root.addName
            type: root.addType
            // A new row here is a screen. Other transports are added on
            // Settings · Routing, where their buses are wired.
            showType: false
            res: root.addRes
            refresh: root.addRefresh
            pattern: root.addPattern
            placement: root.addScreenName
            locked: root.addLocked
            audioDevice: root.addAudioDevice
            onAudioDevicePicked: (id) => root.addAudioDevice = id
            onNameEdited: (t) => root.addName = t
            onResPicked: (v) => root.addRes = v
            onRefreshPicked: (v) => root.addRefresh = v
            onPatternPicked: (k) => {
                root.addPattern = k
                EventBus.notify(qsTr("Test patterns aren't wired to the output yet — this pick is saved but has no visible effect."),
                                "warning", qsTr("Not implemented"), "outputs.testPattern.notImplemented")
            }
            onPlacementPicked: (s, r, f) => {
                root.addScreenName = s
                if (r !== "") {
                    root.addRes = r
                    root.addRefresh = f
                }
            }
            onLockToggled: (l) => root.addLocked = l
        }

        // Same "Stay on top" toggle as the Edit dialog — off by default.
        Item {
            width: parent.width
            height: 34
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Stay on top"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
            SettingsToggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: root.addStayOnTop
                onToggled: root.addStayOnTop = !root.addStayOnTop
            }
        }

    }

}
