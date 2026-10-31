import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// THE BUS PICKER — one modal, two jobs.
//
//   mode: "bus"    (default) Edit's + ring asks WHICH BUS this slide should
//                  carry. Picking is a look: each card is the bus's live feed,
//                  its name, and the three-state light (green connected · amber
//                  routed but offline-or-starved · red nothing routed).
//
//   mode: "newBus" Media › Inputs' "New bus" asks WHICH SOURCE the new bus is
//                  built from, and creates it with that route already made.
//                  Same card grid, same windows, same light, same footer — the
//                  tiles are the sources (screen / camera / NDI / audio input)
//                  instead of buses, because a brand-new bus has no feed of its
//                  own yet: the source it will carry is what can be looked at.
//                  Picking a source is what creates the bus, so a bus that feeds
//                  nothing is no longer something this app can make — no card
//                  that can never light, no monitor whose first click has to be
//                  refused, and nothing to open a device for. The route is made
//                  through the model's own connectVideoRoute/connectAudioRoute
//                  (the call the mix board's drag uses); if it is refused the
//                  bus is taken back out again rather than left behind empty.
//
// Both jobs read the cards through one component: BusMonitor.qml answers "is
// this feed flowing, is its device there, what colour is the light" for a bus OR
// a roster row — the same probe behind both, so a card here can never disagree
// with the card for the same device in Media › Inputs. And nothing here ever
// STARTS work to fill a picture: a card shows a feed only when the app is
// already pulling those frames.
//
// What the bus pick carries is the bus's IDENTITY (its stable engine number),
// not its row: a bus row can be re-ordered or renamed between opening this
// picker and the insert landing, and the item must still point at the same bus.
//
// THE CALLER CLOSES IT — on `cancelled()` AND on `created()`. This component
// writes nothing to `open` itself: EditScreen binds `open` to its own flag, and
// assigning to a bound property from inside would break that binding (the picker
// then never opened again). A caller that forgets the handler leaves a picker on
// screen whose buttons all look dead: add `onCancelled: <pick>.open = false`
// (and the same on `created`) — see MediaLibraryPane.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false
    // "bus" = attach an existing bus to an item;
    // "newBus" = create a bus from the source the operator picks — Media › Inputs'
    // "New bus" AND Edit's + ring both open in this mode (see each caller).
    property string mode: "bus"
    readonly property bool creating: root.mode === "newBus"
    // CREATE MODE CAN BE OPEN IN TWO PLACES AT ONCE (the pane's instance and the
    // Edit screen's both exist, and both are in create mode), so the test hooks
    // below are named after the INSTANCE, from the objectName its caller gives
    // it — otherwise a scenario asking for a source card gets whichever instance
    // the window finds first, and "the card is not laid out" was really "that is
    // the other picker's card, in a screen that is not open".
    readonly property string hookPrefix: root.objectName !== "" ? root.objectName : "busPicker"

    // Re-asks every tile's light when the picker opens: the delegates are built
    // at boot (a hidden Repeater still creates them), so without this a picker
    // could greet the operator with the answer it read before the device
    // rosters existed.
    property int lightTick: 0

    // ---- the create mode's own state ---------------------------------------
    // Which roster and which row in it. -1 = nothing picked yet. Kept apart from
    // selectedId so a mode switch can never leave the other job's pick standing.
    property string chosenKind: ""   // "video" | "audio"
    property int chosenRow: -1
    // ...and the DEVICE that row named, as the roster spells it. A row is a
    // POSITION: a camera unplugged (or a source added anywhere in the app) while
    // this picker is open moves the rows under the pick, and a pick that only
    // remembered the row would then route a DIFFERENT device into the new bus.
    // The label is what the operator actually clicked, so the pick only stands
    // while its row still carries it (see prunePick).
    property string chosenLabel: ""
    // A refusal or a missing pick, said in the dialog instead of only the log.
    property string problem: ""
    // (No template chooser here: a bus is a signal path, and a bus TEMPLATE is a
    // design — made in Edit · Templates, listed on Audio & Video, composed onto an
    // output. This picker only ever picks a source for a bus.)

    // ---- Esc = cancel, Enter = accept (qml/settings/Hotkeys.qml) -----------
    property int hotkeyScope: 0
    function accept() {
        if (!root.creating) {
            if (root.selectedBus !== null)
                root.applied(root.selectedBus)
            return
        }
        root.createBus()
    }
    onOpenChanged: {
        if (root.open) {
            root.selectedId = -1
            root.chosenKind = ""
            root.chosenRow = -1
            root.chosenLabel = ""
            root.problem = ""
            field.text = ""
            root.lightTick++
        }
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: root.creating ? qsTr("New Bus") : qsTr("Bus"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    // ---- the rosters, re-read whenever a model moves -----------------------
    // (The same revision trick the Media pane uses — the models' accessors are
    // Q_INVOKABLE, so QML does not track them as dependencies on its own.)
    property int rev: 0
    Connections {
        target: BusListModel
        function onDataChanged() { root.rev++ }
        function onRowsInserted() { root.rev++ }
        function onRowsRemoved() { root.rev++ }
        function onModelReset() { root.rev++ }
    }
    Connections {
        target: VideoSourceListModel
        function onDataChanged() { root.rev++; root.prunePick() }
        function onRowsInserted() { root.rev++; root.prunePick() }
        function onRowsRemoved() { root.rev++; root.prunePick() }
        function onModelReset() { root.rev++; root.prunePick() }
    }
    Connections {
        target: AudioInputListModel
        function onDataChanged() { root.rev++; root.prunePick() }
        function onRowsInserted() { root.rev++; root.prunePick() }
        function onRowsRemoved() { root.rev++; root.prunePick() }
        function onModelReset() { root.rev++; root.prunePick() }
    }
    // The preview membership is a plain call too, so the cards' live half needs
    // the pump's tick as its own dependency (same reason as the pane's cards).
    // ONLY WHILE OPEN: this picker exists twice (the Media pane's and the Edit
    // screen's) and the pump ticks ~15x/s whenever anything is previewing, so an
    // unconditional bump would re-build both instances' source lists all session
    // for a card nobody is looking at. The tiles' own BusMonitor reads the tick
    // directly, so a shut picker notices nothing (on open, `lightTick` re-asks).
    Connections {
        target: LiveOutputService
        function onInputChanged() { if (root.open) root.rev++ }
    }

    readonly property var rows: {
        void root.rev
        const out = []
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) {
            const b = BusListModel.getBus(i)
            out.push({ row: i, id: b.id !== undefined ? b.id : 0 })
        }
        return out
    }

    // ---- what can feed a NEW bus --------------------------------------
    // Video: the kinds the production graph and the bus feed path both know (a
    // media FILE is not a bus source — it plays into the compositor's media
    // layer, not through a bus — so it is deliberately not offered). Audio
    // inputs come after them, in ONE flat list: a single selection across both
    // rosters, three cards to a row, the way the bus grid already lays out.
    readonly property var videoSources: {
        void root.rev
        const out = []
        for (let i = 0; i < VideoSourceListModel.rowCount(); ++i) {
            const v = VideoSourceListModel.getSource(i)
            const kind = v.kind !== undefined ? String(v.kind) : ""
            if (["camera", "screen", "ndi"].indexOf(kind) < 0)
                continue
            out.push({
                kind: "video",
                row: i,
                sourceKind: kind,
                name: v.name !== undefined && String(v.name) !== "" ? String(v.name) : kind,
                sub: v.sublabel !== undefined ? String(v.sublabel).trim() : ""
            })
        }
        return out
    }
    readonly property var audioSources: {
        void root.rev
        const out = []
        for (let i = 0; i < AudioInputListModel.rowCount(); ++i) {
            const a = AudioInputListModel.getInput(i)
            const kind = a.kind !== undefined ? String(a.kind) : "audio"
            out.push({
                kind: "audio",
                row: i,
                sourceKind: kind,
                name: a.name !== undefined && String(a.name) !== "" ? String(a.name) : kind,
                sub: a.sublabel !== undefined ? String(a.sublabel).trim() : ""
            })
        }
        return out
    }
    readonly property var sources: {
        void root.rev
        return root.videoSources.concat(root.audioSources)
    }
    readonly property bool anySource: root.sources.length > 0

    // Selected by the bus's STABLE id (see the header): -1 = nothing picked.
    // NOT 0 — bus numbers start at 0, so a default of 0 made the first bus (and
    // any row whose id failed to resolve) read as PICKED the moment the picker
    // opened: every tile came up with the accent border before anyone clicked.
    property int selectedId: -1
    readonly property var selectedBus: {
        if (root.selectedId < 0)
            return null
        for (let i = 0; i < root.rows.length; ++i)
            if (Number(root.rows[i].id) === root.selectedId)
                return root.rows[i]
        return null
    }
    readonly property var chosenSource: {
        if (root.chosenRow < 0)
            return null
        const list = root.chosenKind === "audio" ? root.audioSources : root.videoSources
        for (let i = 0; i < list.length; ++i) {
            if (list[i].row !== root.chosenRow)
                continue
            // The pick is valid only while its row still carries the device that
            // was clicked (see chosenLabel) — a shifted roster must not turn one
            // pick into a different source's route.
            if (root.chosenLabel !== "" && list[i].sub !== root.chosenLabel)
                return null
            return list[i]
        }
        return null
    }
    // Drops a pick whose source is gone (unplugged, deleted elsewhere), with the
    // reason on screen rather than a silently different route at Create.
    function prunePick() {
        if (!root.creating || root.chosenRow < 0 || root.chosenSource !== null)
            return
        root.chosenKind = ""
        root.chosenRow = -1
        root.chosenLabel = ""
        root.problem = qsTr("That source is gone — pick another one.")
    }
    // What the footer's button does, as one fact both modes share.
    readonly property bool hasPick: root.creating ? root.chosenSource !== null
                                                  : root.selectedBus !== null

    // ---- a source that ALREADY feeds a bus ---------------------------------
    // A source may feed SEVERAL buses (audio and video alike), so no card is held
    // back. The card only says which bus already carries it — the first one found,
    // or -1 — so the operator knows the source is already in use.
    function sourceBus(kind, row) {
        void root.rev
        const audio = kind === "audio"
        const n = BusListModel.rowCount()
        for (let i = 0; i < n; ++i) {
            const b = BusListModel.getBus(i)
            const field = audio ? b.routedAudioInputs : b.routedVideoSources
            const routed = field !== undefined ? field : []
            for (let r = 0; r < routed.length; ++r)
                if (Number(routed[r]) === Number(row))
                    return i
        }
        return -1
    }
    function busNameAt(row) {
        if (row < 0 || row >= BusListModel.rowCount())
            return ""
        const b = BusListModel.getBus(row)
        return b.name !== undefined ? String(b.name) : ""
    }

    // ---- create the bus (mode "newBus") ------------------------------------
    // The order is deliberate: refuse early (no pick) with the reason on screen;
    // otherwise ADD the bus, ROUTE the pick into it, and take the bus back out
    // again if the graph would not take the route — a half-made bus that feeds
    // nothing is exactly what this picker exists to stop being created.
    function createBus() {
        if (root.chosenSource === null) {
            root.problem = root.chosenRow >= 0
                // A pick that no longer resolves: the roster moved under it (see
                // chosenLabel / prunePick) — say THAT, not "pick a source" to an
                // operator looking at a card still wearing its pick mark.
                ? qsTr("That source is gone — pick another one.")
                : qsTr("Pick the source that feeds this bus — a bus is created with its source selected.")
            return
        }
        const source = root.chosenSource
        const before = BusListModel.rowCount()
        // The type FOLLOWS the source (see the header); the model's own default
        // naming fills an empty name.
        BusListModel.addBus(field.text.trim(),
                            source.kind === "audio" ? "audio" : "video")
        if (BusListModel.rowCount() <= before) {
            root.problem = qsTr("The bus could not be created. Try again.")
            return
        }
        const row = BusListModel.rowCount() - 1
        const routed = source.kind === "audio"
            ? BusListModel.connectAudioRoute(row, source.row)
            : BusListModel.connectVideoRoute(row, source.row)
        if (!routed) {
            const why = BusListModel.connectRefusal(source.kind, source.row, row)
            BusListModel.removeBus(row)
            root.problem = why === "bus-has-video"
                ? qsTr("That source is already feeding another bus. A bus takes one video source at a time.")
                : qsTr("That source could not be routed into a new bus. Choose another one.")
            return
        }
        // One line per creation, the same shape the bus monitor's own lines
        // have: which bus, which type, which source it was born with.
        console.log("[NEWBUS] created bus=" + row
                    + " type=" + (source.kind === "audio" ? "audio" : "video")
                    + " source=" + source.name + " (" + source.sourceKind + ")")
        // The CALLER closes it (see the header): `open` may be a binding in the
        // caller's hands, and writing it here would break that binding.
        root.created(row)
    }

    // Fired by "Add" in bus mode, carrying the picked bus row { row, id }.
    signal applied(var bus)
    // Fired by "Create" in newBus mode, carrying the new bus's row.
    signal created(int busRow)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    // The dialog belongs to the WINDOW, not to the pane that declares it (see
    // CameraSourceModal's note): a modal declared inside a settings page must
    // dim the whole app, and the card applies the app scale itself because
    // reparenting to the overlay leaves it with no scaled ancestor.
    parent: Overlay.overlay

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        // Test hook (see tools/selftest/run_newbus_test.sh): create mode's own
        // card name, so a scenario can tell which job the modal opened as.
        objectName: root.creating ? root.hookPrefix + "_card" : ""
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        anchors.centerIn: parent
        width: 700
        height: Math.min(parent.height - 80, content.height + 40)
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        clip: true

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 14

            // ---- header ----------------------------------------------------
            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.creating ? qsTr("New Bus") : qsTr("Add Bus")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        radius: 7
                        color: closeArea.containsMouse ? Theme.hoverBg : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: Theme.textSecondary
                            font.pixelSize: 14
                        }

                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    // New Bus carries no description; only the pick-a-bus mode keeps its line.
                    visible: !root.creating
                    text: qsTr("Pick the bus this item carries. The window is the bus's live feed; the light says whether it is connected.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }

            // ---- the new bus's name (optional: the model names an unnamed one) ----
            SettingsField {
                id: field
                visible: root.creating
                width: parent.width
                showLabel: false
                placeholder: qsTr("Bus name")
                onAccepted: root.accept()
            }

            // ---- the cards -------------------------------------------------
            Flickable {
                id: gridView
                width: parent.width
                // Bus mode keeps the height it always had. Create mode has one
                // more fixed row (the name field) in front of it, so it takes
                // what the window can spare — a flat cap clipped the last card
                // and, on a short window, pushed the footer out of the card.
                height: Math.min(cardGrid.height + 2,
                                 Math.max(150, root.creating ? root.height - 380 : 360))
                contentHeight: cardGrid.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Flow {
                    id: cardGrid
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: root.creating ? root.sources : root.rows

                        delegate: Item {
                            id: busTile
                            required property var modelData

                            readonly property bool picked: root.creating
                                ? (root.chosenKind === busTile.modelData.kind
                                   && root.chosenRow === busTile.modelData.row)
                                : (root.selectedId >= 0
                                   && Number(busTile.modelData.id) === root.selectedId)
                            // Hover is the tile's OWN state (see tileHover below):
                            // the accent wash belongs to the card the pointer is
                            // on, never to every card at once — which is how a
                            // picker with nothing picked used to look. Read from a
                            // HoverHandler, not the MouseArea: `hovered` is the
                            // handler's property (the rest of the app's hover
                            // language), and a MouseArea has only containsMouse —
                            // binding to it took the whole tile's visuals down with
                            // an "assign undefined to bool" warning.
                            readonly property bool hovered: tileHover.hovered
                            // Test hook (see tools/selftest/run_newbus_test.sh):
                            // one name per pickable SOURCE card.
                            objectName: root.creating
                                ? root.hookPrefix + "_row_" + busTile.modelData.kind + "_" + busTile.modelData.row
                                : ""
                            // The bus this source already feeds (-1 = none, the
                            // normal case). Only INFORMATION: a source may feed
                            // several buses, so nothing is held back — the pick
                            // still works, the line just says what already is.
                            readonly property int alreadyBus: root.creating
                                ? root.sourceBus(busTile.modelData.kind, busTile.modelData.row)
                                : -1
                            readonly property bool alreadyFed: busTile.alreadyBus >= 0
                            readonly property bool selectable: true
                            // WHAT THE CORNER LIGHT MEANS — and its COLOUR is never a
                            // substitute for the probe.
                            //
                            // The colour is ALWAYS BusMonitor.statusColor, i.e. the
                            // probe's own answer: green connected, amber offline or
                            // starved (a closed window, an unplugged camera, a tap
                            // handing nothing over), red when the row names no device.
                            // Painting an unrouted source a fixed colour (red, muted,
                            // whatever) made the light stop answering: a camera that
                            // was gone and a camera that was fine read identically.
                            //
                            // What tells "connected" apart from "not routed" is the
                            // FORM, not the colour: a source a bus already carries is a
                            // FILLED dot (the connection light the operator learned in
                            // Media › Inputs), while a source no bus carries is a hollow
                            // RING of the same probe colour, so green still never means
                            // "connected" about a source feeding nothing.
                            readonly property color lightColor: monitor.statusColor
                            readonly property bool lightRouted: !root.creating
                                || busTile.alreadyFed
                            // Test hook: the LIGHT's own answer, so a scenario can
                            // read what each card claims instead of guessing from
                            // a colour. The probe's word, unmodified — `lightRouted`
                            // is the other half (see above).
                            readonly property string lightState: monitor.status

                            // The one thing both the window and the light read —
                            // a bus in bus mode, a roster row in create mode.
                            BusMonitor {
                                id: monitor
                                busRow: root.creating ? -1 : busTile.modelData.row
                                sourceKind: root.creating ? busTile.modelData.kind : ""
                                sourceRow: root.creating ? busTile.modelData.row : -1
                                tick: root.lightTick
                            }

                            // The name under the window: the bus's, or the source's.
                            readonly property string title: root.creating
                                ? busTile.modelData.name : monitor.busName

                            width: 208
                            height: 168

                            Rectangle {
                                id: tileBody
                                anchors.fill: parent
                                radius: 10
                                // A card is dimmed only if it cannot be picked; every
                                // source can be, so the grid is at full strength.
                                opacity: busTile.selectable ? 1 : 0.55
                                // THE SAME LANGUAGE AS THE PANE'S CARDS (Media ›
                                // Buses), in the theme's accent so it inherits the
                                // user's accent choice like every other control.
                                // The BASE never animates: the tile sits on its
                                // resting colour and the glow is the overlay below.
                                // Animating the fill AND the border at once made a
                                // pointer sweeping across two tiles strobe — each
                                // card popped off and on in ~0.1 s. One fading
                                // layer reads as light coming up under the card
                                // and settling back down.
                                color: busTile.picked ? Qt.alpha(Theme.accent, 0.16)
                                     : Theme.inset
                                border.width: busTile.picked ? 1.5 : 1
                                border.color: busTile.picked ? Theme.accent : Theme.border
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                                // THE HOVER GLOW — the app's shared layer
                                // (HoverGlow), declared before the window so it tints
                                // the card behind the picture and text, never over them.
                                HoverGlow {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    hovered: busTile.hovered
                                    edge: true
                                    edgeColor: Qt.alpha(Theme.accent, 0.50)
                                }

                                // ---- the window: the feed ------------------
                                Rectangle {
                                    id: window_
                                    x: 8; y: 8
                                    width: parent.width - 16
                                    height: Math.round(width * 9 / 16)
                                    radius: 6
                                    color: "#000000"
                                    clip: true

                                    Image {
                                        anchors.fill: parent
                                        source: monitor.previewSource
                                        fillMode: Image.PreserveAspectCrop
                                        cache: false
                                        asynchronous: false
                                        visible: monitor.showingFeed
                                    }

                                    // THE LIGHT: green connected · amber
                                    // offline/starved · red no device — and the
                                    // same colour as a RING while no bus carries
                                    // this source (see lightColor).
                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        width: 9; height: 9; radius: 4.5
                                        color: busTile.lightRouted ? busTile.lightColor
                                                                   : "transparent"
                                        // A ring has to READ as a ring: at 1 px a 9 px
                                        // dot's hollow centre is invisible, and the
                                        // form is the whole point of the distinction.
                                        border.width: busTile.lightRouted ? 1 : 2
                                        // A filled dot keeps its dark halo so it
                                        // reads against the picture; a ring IS the
                                        // probe colour, so a second outline would
                                        // just muddy it.
                                        border.color: busTile.lightRouted
                                                      ? Qt.alpha(Theme.windowBg, 0.45)
                                                      : busTile.lightColor
                                    }
                                }

                                // ---- PICKED, at a glance -------------------
                                // The pick has to look different from the hover
                                // glow, or a clicked tile reads as a stuck hover
                                // (the report: "click it, it just remains cursor
                                // hover"). A tick in the accent, nothing more.
                                // It sits in the WINDOW's bottom-right corner.
                                Rectangle {
                                    anchors.right: window_.right
                                    anchors.bottom: window_.bottom
                                    anchors.margins: 6
                                    width: 16; height: 16; radius: 8
                                    visible: busTile.picked
                                    color: Theme.accent

                                    Text {
                                        anchors.centerIn: parent
                                        text: "✓"
                                        color: Theme.controlInk
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                    }
                                }

                                // ---- name + type/level under the window ----
                                Text {
                                    x: 10
                                    anchors.top: window_.bottom
                                    anchors.topMargin: 7
                                    width: parent.width - 20
                                    text: busTile.title
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                            }

                            // The hover truth the tile's looks read — the app's
                            // HoverHandler pattern (DesignCard, MediaTile, the
                            // MonitorWall rows), so the glow follows the pointer
                            // on this tile only.
                            HoverHandler { id: tileHover }

                            MouseArea {
                                anchors.fill: parent
                                // The pointer shape lives here, not on the handler:
                                // the MouseArea is the child the pointer is over, so
                                // its cursor wins. A held-back card (its source
                                // already feeds a bus) keeps the plain arrow, so the
                                // shape says what the click will do before it is made.
                                cursorShape: busTile.selectable ? Qt.PointingHandCursor : Qt.ArrowCursor
                                // ONE place that takes the pick, so the label and
                                // the row can never drift apart (see chosenLabel:
                                // the row is a position, the label is the device).
                                function takePick() {
                                    if (!root.creating) {
                                        root.selectedId = Number(busTile.modelData.id)
                                        return
                                    }
                                    root.chosenKind = busTile.modelData.kind
                                    root.chosenRow = busTile.modelData.row
                                    root.chosenLabel = busTile.modelData.sub
                                    root.problem = ""
                                }
                                onClicked: takePick()
                                onDoubleClicked: {
                                    takePick()
                                    if (busTile.selectable)
                                        root.accept()
                                }
                            }
                        }
                    }
                }
            }

            // The roster's own scrollbar (the grid can be taller than the
            // window). A sibling of the content Column's children is not allowed
            // to anchor to them, so it is positioned from the grid's geometry.
            AppScrollBar {
                id: gridScroll
                flickable: gridView
                visible: cardGrid.height > gridView.height
                x: gridView.x + gridView.width - width
                y: gridView.y
                height: gridView.height
            }

            // An install with no buses yet says so instead of showing an empty
            // box — the row that fixes it is one screen away.
            Text {
                width: parent.width
                visible: !root.creating && root.rows.length === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("No buses yet — add one in Settings · Audio & Video.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
            // ...and the same for a machine with nothing to feed a new bus.
            Text {
                width: parent.width
                visible: root.creating && !root.anySource
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("No sources are installed yet. Add a camera, a screen, an NDI source or an audio input in Settings · Audio & Video, then make the bus.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // The refusal / missing pick, where the click was (create mode).
            Text {
                objectName: root.creating ? root.hookPrefix + "_problem" : ""
                width: parent.width
                wrapMode: Text.WordWrap
                visible: root.creating && root.problem !== ""
                text: root.problem
                color: Theme.warning
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // ---- footer ----------------------------------------------------
            Item {
                width: parent.width
                height: 36

                Rectangle {
                    objectName: root.creating ? root.hookPrefix + "_create" : ""
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 88
                    height: 36
                    radius: 10
                    // Dimmed, NOT disabled: a disabled parent disables its
                    // MouseArea too, so in create mode a click on the grey button
                    // would do nothing and say nothing. Clicking it sets
                    // root.problem, which is the sentence that says what is
                    // still missing.
                    opacity: root.hasPick ? 1 : 0.4
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: root.creating ? qsTr("Create") : qsTr("Add")
                        color: Theme.controlInk
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.accept()
                    }
                }

                Rectangle {
                    objectName: root.creating ? root.hookPrefix + "_cancel" : ""
                    anchors.right: parent.right
                    anchors.rightMargin: 88 + 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 88
                    height: 36
                    radius: 10
                    color: cancelArea.containsMouse ? Theme.hoverBg : "transparent"
                    border.color: Theme.border
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelled()
                    }
                }
            }
        }
    }
}
