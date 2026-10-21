import QtQuick
import VGRPresenterUI

// The Functions tab — the service-flow automation surface (FreeShow's
// Functions palette grown into the engine's Flow model). The pane lists
// every flow the ENGINE has loaded ({ id, name, nodeCount } from
// FlowService.flows), runs one with a real transport (Run / Pause / Skip /
// Stop), and draws the active execution's node timeline live: each step
// lights pending → running → completed (or failed), driven by the service's
// 250 ms poll of the engine's executor.
Item {
    id: root

    readonly property var flows: FlowService.flows
    readonly property var execution: FlowService.execution
    readonly property bool hasExecution: FlowService.executionId !== ""
    readonly property bool running: FlowService.running
    readonly property string execState: String(execution.state ?? "")

    // Emitted by an action row's Run mark; the window owns the dispatcher.
    signal runActionRequested(string actionId)

    // ---- The action roster, with its CUSTOM ACTIVATIONS -------------------
    // Settings · Appearance & Actions keeps the actions; this pane is where an
    // operator stands while running the show, so it shows what will fire and
    // when. A trigger two actions share is a genuine trap (both run on the
    // same event), so those rows carry a warning badge naming the other one.
    readonly property var actionRows: {
        void ActionListModel.rosterChanged
        const out = []
        for (let i = 0; i < ActionListModel.rowCount(); ++i) {
            const a = ActionListModel.getAction(i)
            out.push({ id: a.id, name: a.name, icon: a.icon, shortcut: a.shortcut,
                       trigger: a.trigger })
        }
        return out
    }
    function triggerLabel(id) {
        if (id === "")
            return ""
        const entries = ActionListModel.triggerEntries()
        for (let i = 0; i < entries.length; ++i)
            if (entries[i].id === id) return entries[i].name
        return id
    }
    // Names of the OTHER actions bound to the same trigger ("" when none).
    function sharedNames(row) {
        if (!row || !row.trigger) return ""
        const names = []
        for (let i = 0; i < root.actionRows.length; ++i) {
            const other = root.actionRows[i]
            if (other.id !== row.id && other.trigger === row.trigger)
                names.push(other.name !== "" ? other.name : qsTr("Untitled"))
        }
        return names.join(", ")
    }

    Component.onCompleted: FlowService.refreshFlows()
    Connections {
        target: FlowService
        function onFlowsChanged() { /* flows binding re-evaluates via the property */ }
    }

    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // ---- Header + quick actions ----------------------------------------
        Item {
            width: parent.width
            height: 34

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Functions — Service Flows")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                text: qsTr("Load a service as a flow, run it step by step — you can always pause, skip or stop.")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // Quick actions — one-shot engine commands (the same registry the
            // flows use; handy without composing a whole flow).
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Repeater {
                    model: [
                        { action: "go_live",         label: qsTr("Go live"),    icon: "play" },
                        { action: "next_slide",      label: qsTr("Next"),       icon: "next" },
                        { action: "previous_slide",  label: qsTr("Prev"),       icon: "previous" },
                        { action: "clear_air",       label: qsTr("Clear"),      icon: "close" }
                    ]
                    delegate: Rectangle {
                        id: quickBtn
                        required property var modelData
                        width: quickRow.width + 18
                        height: 30
                        radius: Theme.radiusMd
                        color: quickArea.containsMouse ? Theme.hoverBg : Theme.inset
                        border.width: 1
                        border.color: Theme.border
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        Row {
                            id: quickRow
                            anchors.centerIn: parent
                            spacing: 6
                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                name: quickBtn.modelData.icon
                                color: Theme.textSecondary
                                fit: true; width: 13; height: 13
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: quickBtn.modelData.label
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
                            }
                        }
                        MouseArea {
                            id: quickArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: FlowService.executeAction(quickBtn.modelData.action, "")
                        }
                    }
                }
            }
        }

        // ---- Flow cards ------------------------------------------------------
        Flickable {
            id: flowFlick
            width: parent.width
            height: parent.height - flowFlick.y - (root.hasExecution ? execCard.height + 10 : 0)
            clip: true
            contentHeight: flowsCol.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: flowsCol
                width: parent.width
                spacing: 8

                Repeater {
                    model: root.flows

                    delegate: Rectangle {
                        id: flowCard
                        required property var modelData
                        width: flowsCol.width
                        height: flowRow.height + 20
                        radius: Theme.radiusLg
                        color: Theme.rowBg
                        border.width: root.hasExecution
                                      && String(root.execution.flowId ?? "") === flowCard.modelData.id ? 1 : 0
                        border.color: Theme.accent

                        Row {
                            id: flowRow
                            x: 12
                            y: 10
                            width: parent.width - 24
                            spacing: 10

                            Column {
                                width: parent.width - runBtn.width - 20
                                spacing: 3
                                Text {
                                    width: parent.width
                                    text: flowCard.modelData.name
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                }
                                Text {
                                    width: parent.width
                                    text: flowCard.modelData.nodeCount + qsTr(" steps · ")
                                          + flowCard.modelData.id
                                    color: Theme.textMuted
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                            }

                            Rectangle {
                                id: runBtn
                                anchors.verticalCenter: parent.verticalCenter
                                width: runLabel.width + 26
                                height: 30
                                radius: Theme.radiusMd
                                color: root.hasExecution
                                       && String(root.execution.flowId ?? "") === flowCard.modelData.id
                                           ? Theme.armedStopBg : Theme.armedReadyBg
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 7
                                    IconGlyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: root.hasExecution
                                              && String(root.execution.flowId ?? "") === flowCard.modelData.id
                                              ? "stop" : "play"
                                        color: root.hasExecution
                                               && String(root.execution.flowId ?? "") === flowCard.modelData.id
                                               ? Theme.dangerLight : Theme.armedReadyInk
                                        fit: true; width: 12; height: 12
                                    }
                                    Text {
                                        id: runLabel
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.hasExecution
                                              && String(root.execution.flowId ?? "") === flowCard.modelData.id
                                              ? qsTr("Stop") : qsTr("Run")
                                        color: root.hasExecution
                                               && String(root.execution.flowId ?? "") === flowCard.modelData.id
                                               ? Theme.dangerLight : Theme.armedReadyInk
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        font.weight: Font.DemiBold
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (root.hasExecution
                                            && String(root.execution.flowId ?? "") === flowCard.modelData.id)
                                            FlowService.stop()
                                        else
                                            FlowService.start(flowCard.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }

                // Empty state (engine booting, or every flow removed).
                Column {
                    visible: root.flows.length === 0
                    width: parent.width
                    spacing: 8
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("No flows loaded yet.")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }
                    AppButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Load the starter Sunday flow")
                        variant: "secondary"
                        onClicked: FlowService.loadStarterFlow()
                    }
                }

                // ---- Actions & their custom activations --------------------
                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.border
                    opacity: 0.7
                }
                Row {
                    width: parent.width
                    spacing: 8
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Actions")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.Medium
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("what runs on a keypress, and on an event")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                }

                Repeater {
                    model: root.actionRows
                    delegate: Rectangle {
                        id: actionRow
                        required property var modelData
                        width: flowsCol.width
                        // A shared trigger makes the row taller: the badge and
                        // the trigger line both need their own space.
                        readonly property string shared: root.sharedNames(modelData)
                        height: 52
                        radius: Theme.radiusMd
                        color: Theme.rowBg
                        border.width: actionRow.shared !== "" ? 1 : 0
                        border.color: Qt.alpha(Theme.warning, 0.5)
                        clip: true

                        // Right-hand cluster: shared badge, keypress, run.
                        Row {
                            id: actionTrailing
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Rectangle {
                                visible: actionRow.shared !== ""
                                width: sharedRow.width + 16
                                height: 22
                                radius: 11
                                color: Qt.alpha(Theme.warning, 0.14)
                                border.width: 1
                                border.color: Qt.alpha(Theme.warning, 0.45)
                                Row {
                                    id: sharedRow
                                    anchors.centerIn: parent
                                    spacing: 5
                                    IconGlyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "info"
                                        color: Theme.warningLight
                                        width: 10; height: 10; fit: true
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("Shared trigger")
                                        color: Theme.warningLight
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                    }
                                }
                            }

                            Rectangle {
                                visible: actionRow.modelData.shortcut !== ""
                                width: keyLabel.width + 14
                                height: 22
                                radius: 6
                                color: Theme.inset
                                border.width: 1
                                border.color: Theme.border
                                Text {
                                    id: keyLabel
                                    anchors.centerIn: parent
                                    text: actionRow.modelData.shortcut
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                            }

                            Rectangle {
                                width: 26; height: 26; radius: 6
                                color: runHover.hovered ? Theme.hoverBg : "transparent"
                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: "play"
                                    color: runHover.hovered ? Theme.textPrimary : Theme.textSecondary
                                    width: 13; height: 13; fit: true
                                }
                                HoverHandler { id: runHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: root.runActionRequested(actionRow.modelData.id)
                                }
                            }
                        }

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: actionTrailing.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                name: actionRow.modelData.icon || "flag"
                                color: Theme.accentLight
                                width: 15; height: 15; fit: true
                            }
                            Column {
                                width: parent.width - 25
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: actionRow.modelData.name !== "" ? actionRow.modelData.name
                                                                         : qsTr("Unnamed action")
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 14
                                }
                                Text {
                                    width: parent.width
                                    text: actionRow.modelData.trigger !== ""
                                        ? root.triggerLabel(actionRow.modelData.trigger)
                                          + (actionRow.shared !== ""
                                             ? qsTr("  ·  also on %1").arg(actionRow.shared) : "")
                                        : qsTr("No custom activation")
                                    color: actionRow.shared !== "" ? Theme.warningLight
                                         : actionRow.modelData.trigger !== "" ? Theme.textSecondary
                                         : Theme.textMuted
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                            }
                        }
                    }
                }

                Text {
                    visible: root.actionRows.length === 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No actions yet — create them in Settings · Appearance & Actions.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }
        }
        AppScrollBar {
            anchors.right: parent.right
            width: 8
            height: flowFlick.height
            flickable: flowFlick
        }

        // ---- Live execution card --------------------------------------------
        Rectangle {
            id: execCard
            width: parent.width
            height: root.hasExecution ? execCol.height + 24 : 0
            visible: root.hasExecution
            radius: Theme.radiusLg
            color: "#14151c"
            border.width: 1
            // A running flow is a LIVE surface, so its card wears the live red the
            // GO LIVE pill wears (Theme.liveBgOnAir) — this border used to be its own
            // near-copy of that red, which is how the live red drifted into six values.
            border.color: root.running ? Theme.liveBgOnAir : Theme.border

            Column {
                id: execCol
                x: 12
                y: 12
                width: parent.width - 24
                spacing: 8

                Item {
                    width: parent.width
                    height: 22

                    Row {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8; height: 8; radius: 4
                            color: root.running ? Theme.liveBg : Theme.warningLight
                            SequentialAnimation on opacity {
                                running: root.running
                                loops: Animation.Infinite
                                NumberAnimation { from: 1; to: 0.3; duration: 600 }
                                NumberAnimation { from: 0.3; to: 1; duration: 600 }
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: {
                                const s = root.execState
                                return s === "running" ? qsTr("RUNNING")
                                     : s === "paused" ? qsTr("PAUSED")
                                     : s === "completed" ? qsTr("COMPLETED")
                                     : s.toUpperCase()
                            }
                            color: root.running ? Theme.dangerLight : Theme.warningLight
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: String(root.execution.waitingFor ?? "") !== ""
                            text: "· waiting for " + root.execution.waitingFor
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }

                    // Transport.
                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Rectangle {
                            visible: root.execState === "paused" || root.execState === "running"
                            width: 30; height: 30; radius: Theme.radiusMd
                            color: Theme.chip
                            IconGlyph {
                                anchors.centerIn: parent
                                name: root.execState === "paused" ? "play" : "pause"
                                color: Theme.textPrimary
                                fit: true; width: 13; height: 13
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.execState === "paused" ? FlowService.resume()
                                                                       : FlowService.pause()
                            }
                        }
                        Rectangle {
                            visible: root.execState === "running" || root.execState === "paused"
                            width: 30; height: 30; radius: Theme.radiusMd
                            color: Theme.chip
                            Text {
                                anchors.centerIn: parent
                                text: "⏭"
                                color: Theme.textPrimary
                                font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: FlowService.skip()
                            }
                        }
                        Rectangle {
                            width: 30; height: 30; radius: Theme.radiusMd
                            color: Theme.armedStopBg
                            IconGlyph {
                                anchors.centerIn: parent
                                name: "stop"
                                color: Theme.dangerLight
                                fit: true; width: 13; height: 13
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: FlowService.stop()
                            }
                        }
                    }
                }

                // The node timeline — one pill per step, colored by state.
                Flow {
                    width: parent.width
                    spacing: 6

                    Repeater {
                        model: root.execution.nodes ?? []

                        delegate: Rectangle {
                            id: stepPill
                            required property var modelData
                            readonly property string st: String(modelData.state ?? "pending")
                            width: stepRow.width + 20
                            height: 26
                            radius: 13
                            color: st === "completed" ? "#173326"
                                 : st === "running" ? Theme.armedLiveBg
                                 : st === "failed" ? "#3a1410"
                                 : st === "skipped" ? Theme.card
                                 : Theme.inset
                            border.width: st === "running" ? 1 : 0
                            border.color: Theme.liveBg

                            Row {
                                id: stepRow
                                anchors.centerIn: parent
                                spacing: 6
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6; height: 6; radius: 3
                                    color: st === "completed" ? "#3ddc84"
                                         : st === "running" ? Theme.liveBg
                                         : st === "failed" ? Theme.danger
                                         : st === "skipped" ? Theme.textMuted
                                         : "#3a3c48"
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: stepPill.modelData.label
                                    color: st === "completed" ? "#8fe0b0"
                                         : st === "running" ? Theme.dangerLight
                                         : st === "failed" ? Theme.dangerLight
                                         : Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: FlowService.jumpTo(stepPill.modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }
}
