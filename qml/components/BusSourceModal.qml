import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false
    property string mode: "bus"
    readonly property bool creating: root.mode === "newBus"
    readonly property string hookPrefix: root.objectName !== "" ? root.objectName : "busPicker"

    property int lightTick: 0

    // ---- the create mode's own state ---------------------------------------
    // Which roster and which row in it. -1 = nothing picked yet. Kept apart from
    // selectedId so a mode switch can never leave the other job's pick standing.
    property string chosenKind: ""   // "video" | "audio"
    property int chosenRow: -1
    property string chosenLabel: ""
    // A refusal or a missing pick, said in the dialog instead of only the log.
    property string problem: ""

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
    function createBus() {
        if (root.chosenSource === null) {
            root.problem = root.chosenRow >= 0
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

                    ModalCloseButton {
                        id: closeBtn
                        onClicked: root.cancelled()
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
                            readonly property bool hovered: tileHover.hovered
                            // Test hook (see tools/selftest/run_newbus_test.sh):
                            // one name per pickable SOURCE card.
                            objectName: root.creating
                                ? root.hookPrefix + "_row_" + busTile.modelData.kind + "_" + busTile.modelData.row
                                : ""
                            readonly property int alreadyBus: root.creating
                                ? root.sourceBus(busTile.modelData.kind, busTile.modelData.row)
                                : -1
                            readonly property bool alreadyFed: busTile.alreadyBus >= 0
                            readonly property bool selectable: true
                            readonly property color lightColor: monitor.statusColor
                            readonly property bool lightRouted: !root.creating
                                || busTile.alreadyFed
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
                                color: busTile.picked ? Qt.alpha(Theme.accent, 0.16)
                                     : Theme.inset
                                border.width: busTile.picked ? 1.5 : 1
                                border.color: busTile.picked ? Theme.accent : Theme.border
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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

                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        width: 9; height: 9; radius: 4.5
                                        color: busTile.lightRouted ? busTile.lightColor
                                                                   : "transparent"
                                        border.width: busTile.lightRouted ? 1 : 2
                                        border.color: busTile.lightRouted
                                                      ? Qt.alpha(Theme.windowBg, 0.45)
                                                      : busTile.lightColor
                                    }
                                }

                                // ---- PICKED, at a glance -------------------
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

                            HoverHandler { id: tileHover }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: busTile.selectable ? Qt.PointingHandCursor : Qt.ArrowCursor
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