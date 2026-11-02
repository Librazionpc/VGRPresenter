import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root

    readonly property var uiPlugins: [
        { name: qsTr("Song Provider"), version: "v1.2", tag: qsTr("chords + transpose"), enabled: true },
        { name: qsTr("Bible Provider"), version: "v1.0", tag: qsTr("KJV + BBE"), enabled: true },
        // "ndi" defaults off now (AdaptiveRuntime) — this fallback only
        // shows before the engine row replaces it, so it matches that.
        { name: qsTr("NDI Broadcast"), version: "", tag: qsTr("network output"), engine: "ndi", feature: "ndi", enabled: false },
        { name: qsTr("MIDI Control"), version: "v0.9", tag: qsTr("hardware triggers"), enabled: false },
        { name: qsTr("Flow Automation"), version: "v1.0", tag: qsTr("service flows"), enabled: true }
    ]
    property var uiOnlyStates: ({})

    // ---- The NDI toggle's firewall answer (see EngineBridge::
    property bool ndiTogglePending: false
    function handleNdiFirewallResult(outcome) {
        if (!root.ndiTogglePending)
            return
        root.ndiTogglePending = false
        if (outcome === "prompt") {
            EventBus.notify(qsTr("NDI switched on — approve the Windows permission prompt so other computers can receive your video."),
                            "info", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
            return
        }
        if (outcome === "denied" || outcome === "failed") {
            EventBus.notify(qsTr("NDI switched on, but network access was refused — sending to other computers won't work until you allow it (Settings › Outputs)."),
                            "warning", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
            return
        }
        if (outcome === "unavailable") {
            EventBus.notify(qsTr("NDI switched on — this platform can't grant network access automatically; allow it manually if sending doesn't reach other computers."),
                            "warning", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
            return
        }
        // "allowed" / "added": access is in place — the plain notice says it.
        EventBus.notify(qsTr("NDI switched on."), "info", qsTr("Plugins"), "plugins.toggle.ndi")
    }
    Connections {
        target: EngineBridge
        function onNdiFirewallAccessResult(outcome) { root.handleNdiFirewallResult(outcome) }
    }
    // Engine rows REPLACE their UI twin (no duplicates when the registry
    // grows); UI-only rows keep their local `enabled` flag.
    readonly property var plugins: {
        void EngineBridge.pluginFeatures   // the reactivity dependency
        const engine = EngineBridge.pluginFeatures
        const merged = []
        const engineIds = {}
        for (let i = 0; i < engine.length; ++i) {
            engineIds[engine[i].id] = true
            merged.push({ name: engine[i].name, version: "engine", tag: engine[i].id,
                          enabled: engine[i].enabled, engine: engine[i].id, feature: engine[i].id })
        }
        for (let j = 0; j < root.uiPlugins.length; ++j) {
            const row = root.uiPlugins[j]
            if (row.feature && engineIds[row.feature])
                continue   // the engine row above already represents it
            const local = root.uiOnlyStates[row.name]
            merged.push(local === undefined ? row : Object.assign({}, row, { enabled: local }))
        }
        return merged
    }

    function persistFeatureStates() {
        let saved = {}
        try { saved = JSON.parse(SettingsService.value("plugins.featureStates") || "{}") } catch (e) { saved = {} }
        const engine = EngineBridge.pluginFeatures
        for (let i = 0; i < engine.length; ++i)
            saved[engine[i].id] = engine[i].enabled
        SettingsService.setValue("plugins.featureStates", JSON.stringify(saved))
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.y + layout.height + Theme.space6   // the column's own top offset counts, or the last card sits on the window edge
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space5
            width: flick.width - Theme.space6
            spacing: 16

            // ---- Page header ----
            Column {
                spacing: 2

                Text {
                    text: qsTr("Plugins")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
            }

            // ---- Installed plugins card ----
            Rectangle {
                width: parent.width
                height: pluginsCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: pluginsCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Installed plugins")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }

                    Repeater {
                        model: root.plugins
                        delegate: Column {
                            id: pluginRow
                            required property var modelData
                            required property int index
                            width: pluginsCol.width

                            // Engine-backed rows show the runtime's real state; the
                            // toggle is only the user's wish, never proof it works.
                            readonly property bool isNdi: modelData.engine === "ndi"
                            readonly property string ndiState: EngineBridge.ndiState
                            readonly property bool runtimeOk: !isNdi || ndiState === "ready"
                            readonly property bool live: modelData.enabled && runtimeOk

                            Item {
                                width: pluginRow.width
                                height: 46

                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Text {
                                        text: pluginRow.modelData.name
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                }

                                // Status pill — derived from the toggle.
                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: pillLabel.width + 20
                                    height: 20
                                    radius: 10
                                    color: pluginRow.live ? "#264ade80" : "#26f5c26b"

                                    Text {
                                        id: pillLabel
                                        anchors.centerIn: parent
                                        text: !pluginRow.runtimeOk
                                              ? (pluginRow.ndiState === "notInstalled" ? qsTr("Runtime missing")
                                                 : pluginRow.ndiState === "disabled"   ? qsTr("Switched off")
                                                 : qsTr("Error"))
                                              : pluginRow.live ? qsTr("Active") : qsTr("Installed")
                                        color: pluginRow.live ? Theme.success : Theme.warning
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                }

                                SettingsToggle {
                                    objectName: pluginRow.isNdi ? "selfTestNdiToggle" : ""
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: pluginRow.modelData.enabled
                                    onToggled: {
                                        const rootRef = root
                                        const want = !pluginRow.modelData.enabled
                                        const feature = pluginRow.modelData.feature
                                        const name = pluginRow.modelData.name
                                        if (feature) {
                                            EngineBridge.setPluginFeatureEnabled(feature, want)
                                            Qt.callLater(function () {
                                                rootRef.persistFeatureStates()
                                                if (want && feature === "ndi") {
                                                    rootRef.ndiTogglePending = true
                                                    EngineBridge.requestNdiFirewallAccess()
                                                    return
                                                }
                                                rootRef.ndiTogglePending = false
                                                EventBus.notify(want
                                                    ? qsTr("%1 switched on.").arg(name)
                                                    : qsTr("%1 switched off — its outputs and inputs stop with it.").arg(name),
                                                    "info", qsTr("Plugins"),
                                                    "plugins.toggle." + feature)
                                            })
                                            return
                                        }
                                        Qt.callLater(function () {
                                            // Honest local flip (see uiOnlyStates): the row
                                            // has no engine feature to turn on or off.
                                            const states = Object.assign({}, rootRef.uiOnlyStates)
                                            states[name] = want
                                            rootRef.uiOnlyStates = states
                                            EventBus.notify(qsTr("Plugin enable/disable isn't wired up yet — this doesn't actually turn %1 on or off.").arg(name),
                                                            "warning", qsTr("Not implemented"),
                                                            "plugins.toggle.notImplemented")
                                        })
                                    }
                                }
                            }
                            // The way out when a runtime is missing/broken: reason,
                            // a link to the download page, and a re-check.
                            NdiRuntimeNotice {
                                id: ndiNotice
                                width: pluginRow.width
                                active: pluginRow.isNdi
                            }
                            Item { width: 1; height: ndiNotice.visible ? 10 : 0 }

                            Rectangle { width: pluginRow.width; height: 1; color: Theme.border; visible: pluginRow.index < root.plugins.length - 1 }
                        }
                    }
                }
            }

            // ---- Browse plugin store ----
            Rectangle {
                width: 160
                height: 36
                radius: Theme.radiusMd
                color: browseArea.containsMouse ? Theme.chip : Theme.inset
                border.color: Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Browse plugin store")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: browseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // The plugin store is its own future surface — no
                    // ground-truth design exists for it yet.
                    onClicked: EventBus.notify(qsTr("The plugin store isn't built yet."),
                                               "warning", qsTr("Not implemented"),
                                               "plugins.store.notImplemented")
                }
            }
        }
    }

    // Shared app scrollbar at the fixed right edge (sibling of the
    // Flickable — see GeneralScreen's note).
    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }
}