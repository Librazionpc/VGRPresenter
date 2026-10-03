import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Plugins — the installed-plugins list, same minimal skeleton
// as the other settings screens (Item root, Flickable, shared
// AppScrollBar, Theme tokens). One card of rows: name + version/tag
// subtitle, a status pill, and an enable toggle. The pill is DERIVED, not
// stored — "Active" (green) while enabled, "Installed" (amber) when
// switched off — so the row can never show contradictory state.
Item {
    id: root

    // The UI-only plugin roster (name/version/tag facts) MERGED with the
    // ENGINE's real feature registry (EngineBridge.pluginFeatures — entries
    // { id, name, enabled, defaultEnabled }): an entry with a `feature` id
    // has its pill AND its switch driven by the engine's own state, so the
    // toggle turns the subsystem on or off for real (NDI's broadcast stack
    // honors it) and the state survives a restart. Entries without one stay
    // inert UI rows with their honest "not wired up" toast.
    // Every row needs a real `enabled` boolean, even the ones the engine
    // merge below is expected to replace — modelData.enabled feeds `checked`
    // (SettingsToggle) and the `live` pill directly, and an unset field
    // reads as JS `undefined`, not a QML bool: assigning it to a `property
    // bool` throws "Unable to assign [undefined] to bool" the moment this
    // row renders before the engine registry has populated (e.g. still
    // booting), which then broke construction of everything under it in the
    // same delegate — reported live as the toggle throwing a ReferenceError
    // on an id that WAS in scope, just inside a half-constructed component.
    readonly property var uiPlugins: [
        { name: qsTr("Song Provider"), version: "v1.2", tag: qsTr("chords + transpose"), enabled: true },
        { name: qsTr("Bible Provider"), version: "v1.0", tag: qsTr("KJV + BBE"), enabled: true },
        // "ndi" defaults off now (AdaptiveRuntime) — this fallback only
        // shows before the engine row replaces it, so it matches that.
        { name: qsTr("NDI Broadcast"), version: "", tag: qsTr("network output"), engine: "ndi", feature: "ndi", enabled: false },
        { name: qsTr("MIDI Control"), version: "v0.9", tag: qsTr("hardware triggers"), enabled: false },
        { name: qsTr("Flow Automation"), version: "v1.0", tag: qsTr("service flows"), enabled: true }
    ]
    // UI-only rows have no engine feature to flip (the registry only carries
    // "ndi"), so their switch keeps HONEST LOCAL state here — keyed by name —
    // while the toast says it isn't wired up. The merged `plugins` binding
    // below reads this; the toggle writes it. (It used to assign straight to
    // the read-only `plugins`, which threw "Cannot assign to read-only
    // property 'plugins'" the moment one of these rows was switched.)
    property var uiOnlyStates: ({})
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

    // Persist a flip: key → enabled, merged over the last saved map (every
    // switch leaves its own trace; an unlisted feature keeps its default).
    // Reads the ENGINE's registry, NOT root.plugins — the merged binding is
    // still stale at call time (setPluginFeatureEnabled returned before
    // pluginFeaturesChanged re-evaluated it), so reading it here saved the
    // PRE-toggle value: a switch-ON rebooted as OFF (self-test caught it:
    // SendFrame refused "NDI is switched off" on a fresh boot).
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
        contentHeight: layout.height + 24
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
                Text {
                    text: qsTr("Extend VGR with providers, devices and integrations.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
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
                            readonly property string subtitle: isNdi
                                ? (EngineBridge.ndiVersion !== "" ? EngineBridge.ndiVersion : qsTr("runtime not detected")) + " · " + modelData.tag
                                : modelData.version + " · " + modelData.tag

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
                                    Text {
                                        text: pluginRow.subtitle
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
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
                                        // A deliberate "off" (the switch, ndiState
                                        // "disabled") is neutral-amber "Switched
                                        // off" — not the red "Runtime missing"/
                                        // "Error" of a runtime problem.
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
                                    // A `feature` row IS the engine's registry
                                    // entry: the toggle calls the bridge, which
                                    // flips the AdaptiveRuntime feature — NDI's
                                    // broadcast stack honors that for real (its
                                    // senders/receivers come down with the
                                    // switch). Everything else stays an inert
                                    // UI row with the honest toast.
                                    onToggled: {
                                        // Capture everything needed BEFORE
                                        // calling setPluginFeatureEnabled —
                                        // it emits pluginFeaturesChanged()
                                        // synchronously, which recomputes
                                        // root.plugins, which makes the
                                        // Repeater destroy/recreate THIS
                                        // delegate (pluginRow) while this
                                        // very handler is still running on
                                        // it. Anything read from pluginRow
                                        // AFTER that call is reading from a
                                        // dying object — reported live as
                                        // "root is not defined" a few lines
                                        // later, inside a context whose
                                        // owning component had already been
                                        // torn down. Qt.callLater defers the
                                        // rest to a fresh call stack, once
                                        // the destroy/recreate has settled.
                                        // A plain QML id lookup (bare "root")
                                        // inside a Qt.callLater callback does
                                        // NOT reliably resolve — empirically
                                        // confirmed live (still threw "root
                                        // is not defined" from INSIDE the
                                        // deferred callback even after moving
                                        // the risky reads there). Capturing
                                        // the object into a plain JS const
                                        // BEFORE deferring sidesteps this
                                        // entirely — ordinary variable
                                        // closures don't depend on QML's own
                                        // id-resolution machinery the way a
                                        // bare id reference does.
                                        const rootRef = root
                                        const want = !pluginRow.modelData.enabled
                                        const feature = pluginRow.modelData.feature
                                        const name = pluginRow.modelData.name
                                        if (feature) {
                                            EngineBridge.setPluginFeatureEnabled(feature, want)
                                            Qt.callLater(function () {
                                                rootRef.persistFeatureStates()
                                                // NDI specifically needs INBOUND
                                                // firewall access to actually SEND
                                                // (receiving is outbound-only and
                                                // always works) — check it right
                                                // when the user turns the feature
                                                // on, instead of leaving them to
                                                // find a separate button elsewhere
                                                // in Settings > Outputs. Probes
                                                // first (EngineBridge.request
                                                // NdiFirewallAccess's own
                                                // contract) — a no-op toast-wise
                                                // when access is already there.
                                                if (want && feature === "ndi") {
                                                    const outcome = EngineBridge.requestNdiFirewallAccess()
                                                    if (outcome === "prompt") {
                                                        EventBus.notify(qsTr("NDI switched on — approve the Windows permission prompt so other computers can receive your video."),
                                                            "info", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
                                                        return
                                                    } else if (outcome === "denied" || outcome === "failed") {
                                                        EventBus.notify(qsTr("NDI switched on, but network access was refused — sending to other computers won't work until you allow it (Settings › Outputs)."),
                                                            "warning", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
                                                        return
                                                    } else if (outcome === "unavailable") {
                                                        EventBus.notify(qsTr("NDI switched on — this platform can't grant network access automatically; allow it manually if sending doesn't reach other computers."),
                                                            "warning", qsTr("Plugins"), "plugins.toggle.ndi.firewall")
                                                        return
                                                    }
                                                    // "added": access already confirmed — the
                                                    // plain "switched on" toast below covers it.
                                                }
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
                Behavior on color { ColorAnimation { duration: 100 } }

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
