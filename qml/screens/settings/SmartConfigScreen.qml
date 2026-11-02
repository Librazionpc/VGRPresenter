import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root

    readonly property var values: SettingsService.values
    readonly property var modeChoices: SettingsService.definitions["smart.mode"].choices
    readonly property bool manual: root.values["smart.mode"] === "manual"
    readonly property bool lockedIn: root.values["appearance.lockInMode"] === true

    function profileLabel(value) {
        const choices = SettingsService.definitions["resources.profile"].choices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === value)
                return choices[i].label
        return ""
    }
    // ---- Glow-dial thresholds ----
    function budgetColor(used, cap) {
        if (used === undefined || used < 0)
            return Theme.textMuted
        if (cap > 0 && used > cap)
            return Theme.danger
        if (cap > 0 && used >= cap * 0.85)
            return Theme.warning
        return Theme.success
    }
    // Charge: green comfortably charged, amber getting low, red near empty.
    function batteryColor(pct) {
        if (pct === undefined || pct < 0)
            return Theme.textMuted
        return pct < 20 ? Theme.danger
             : pct < 40 ? Theme.warning : Theme.success
    }

    // Look at the machine again whenever this page is shown (a device may have been plugged in).
    Component.onCompleted: SettingsService.refreshHardware()

    Flickable {
        id: flick
        objectName: "selfTestSmartFlick"
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
                    text: qsTr("Smart Config")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
            }

            // ---- Configuration mode card ----
            Rectangle {
                width: parent.width
                height: modeCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: modeCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    Text {
                        text: qsTr("Configuration mode")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Row {
                        width: parent.width
                        spacing: 16

                        Repeater {
                            model: root.modeChoices
                            delegate: Rectangle {
                                id: modeCell
                                required property var modelData
                                readonly property bool active: root.values["smart.mode"] === modeCell.modelData.value

                                width: (parent.width - 32) / 3
                                height: 62
                                radius: Theme.radiusMd
                                color: modeCell.active ? Theme.accentSoft : (modeArea.containsMouse ? Theme.chip : Theme.inset)
                                border.width: 1
                                border.color: modeCell.active ? Theme.accent : Theme.border
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                                Column {
                                    x: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 3

                                    Text {
                                        text: modeCell.modelData.label
                                        color: modeCell.active ? Theme.accentLight : Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        font.weight: modeCell.active ? Font.DemiBold : Font.Medium
                                        opacity: root.lockedIn ? 0.6 : 1
                                    }
                                }

                                MouseArea {
                                    id: modeArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: SettingsService.setValue("smart.mode", modeCell.modelData.value)
                                }
                            }
                        }
                    }

                    Row {
                        visible: root.lockedIn
                        spacing: 6

                        Rectangle {
                            width: 8; height: 8; radius: 4
                            color: Theme.warning
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            width: modeCol.width - 14
                            text: qsTr("Lock In Mode is on \u2014 running in Presentation mode regardless of the pick here.")
                            color: Theme.warning
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSub
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---- Hardware detected card ----
            Rectangle {
                width: parent.width
                height: hwCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: hwCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Hardware detected")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }

                    Repeater {
                        model: SettingsService.hardwareRows
                        delegate: Column {
                            id: hwRow
                            required property var modelData
                            required property int index
                            width: hwCol.width

                            Item {
                                width: hwRow.width
                                height: 32

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: hwRow.modelData.label
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }

                                Row {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 8

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: hwRow.modelData.ok ? "✓" : "!"
                                        color: hwRow.modelData.ok ? Theme.success : Theme.warning
                                        font.pixelSize: 13
                                        font.weight: Font.Bold
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: hwRow.modelData.value
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                }
                            }
                            Rectangle { width: hwRow.width; height: 1; color: Theme.border; visible: hwRow.index < SettingsService.hardwareRows.length - 1 }
                        }
                    }
                }
            }

            // ---- Recommended profile card: the engine's pick for this machine, one click to put it in force ----
            Rectangle {
                width: parent.width
                height: 74
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1
                visible: SettingsService.recommendedProfile !== ""

                readonly property bool inForce: {
                    if (root.values["resources.profile"] !== SettingsService.recommendedProfile)
                        return false
                    if (!root.manual)
                        return true
                    const prof = SettingsService.capsFor(SettingsService.recommendedProfile)
                    return root.values["smart.gpuBudgetPct"] === prof.gpu
                        && root.values["smart.cpuBudgetPct"] === prof.cpu
                }

                Column {
                    x: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 180
                    spacing: 4

                    Text {
                        text: qsTr("Recommended profile: %1").arg(root.profileLabel(SettingsService.recommendedProfile))
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: SettingsService.hardwareDetail
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.Wrap
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: 116
                    height: 34
                    radius: Theme.radiusMd
                    opacity: parent.inForce ? 0.5 : 1
                    color: applyArea.containsMouse && !parent.inForce ? Theme.accentLight : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: parent.parent.inForce ? qsTr("In use") : qsTr("Apply")
                        color: Theme.controlInk
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Medium
                    }
                    MouseArea {
                        id: applyArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !parent.parent.inForce
                        cursorShape: Qt.PointingHandCursor
                        onClicked: SettingsService.applyRecommendedProfile()
                    }
                }
            }

            // ---- Configuration advice card: what the engine found MEANS for running the app ----
            Rectangle {
                width: parent.width
                height: adviceCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1
                visible: SettingsService.hardwareAdvice.length > 0

                Column {
                    id: adviceCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Configuration advice")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: SettingsService.hardwareAdviceSummary
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        bottomPadding: 14
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: SettingsService.hardwareAdvice
                        delegate: Item {
                            id: adviceRow
                            required property var modelData
                            width: adviceCol.width
                            height: adviceText.implicitHeight + 18

                            // A severity stripe: amber = act on it, accent =
                            // worth knowing, green = nothing to do.
                            Rectangle {
                                width: 3
                                height: adviceText.implicitHeight
                                radius: 1.5
                                anchors.verticalCenter: parent.verticalCenter
                                color: adviceRow.modelData.severity === "warn" ? Theme.warning
                                     : adviceRow.modelData.severity === "ok" ? Theme.success : Theme.accent
                            }

                            Text {
                                id: adviceText
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 14
                                text: adviceRow.modelData.text
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textMd
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            // ---- Resource budgets card ----
            Rectangle {
                width: parent.width
                height: budgetsCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: budgetsCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 10

                    Item {
                        width: parent.width
                        height: healthBtn.height

                        Text {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Resource budgets")
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }

                        Item {
                            id: healthBtn
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: healthRow.width
                            height: healthRow.height

                            Row {
                                id: healthRow
                                spacing: 6

                                IconGlyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "gauge"
                                    color: healthArea.containsMouse ? Theme.accentLight : Theme.accent
                                    implicitWidth: 14
                                    implicitHeight: 14
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Live health")
                                    color: healthArea.containsMouse ? Theme.accentLight : Theme.accent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.Medium
                                }
                            }

                            MouseArea {
                                id: healthArea
                                anchors.fill: parent
                                anchors.margins: -6
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: healthModal.open()
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        text: root.manual
                              ? qsTr("Manual mode: drag to set how much of the machine the engine may use. The meter follows what it is actually using.")
                              : qsTr("The %1 profile sets these limits. The bright bar is what is being used now; the pale area is how much the profile allows.").arg(root.profileLabel(root.values["resources.profile"]))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSub
                        wrapMode: Text.Wrap
                    }

                    component BudgetRow: Item {
                        id: budget
                        property string label: ""
                        property string settingKey: ""       // the engine's setting behind it in Manual mode
                        property int cap: 0                  // the share allowed (percent)
                        property int used: -1                // live usage (percent); -1 = not measured
                        property real draft: budget.cap
                        property bool dragging: false
                        width: parent.width
                        height: root.manual ? 52 : 30

                        Binding { target: budget; property: "draft"; value: budget.cap; when: !budget.dragging }

                        // While a slider is being dragged the meter tracks the
                        // drag, so the cap line moves with the thumb.
                        readonly property int capNow: budget.dragging ? Math.round(budget.draft) : budget.cap

                        LabeledMeter {
                            visible: !root.manual
                            width: parent.width
                            height: 30
                            label: budget.label
                            pct: budget.used < 0 ? 0 : budget.used
                            capPct: budget.capNow
                            // The platform cannot always report a usage figure
                            // (no driver counter); say so instead of a fake 0.
                            readout: budget.used < 0 ? qsTr("Not measured · cap %1%").arg(budget.capNow) : ""
                        }

                        LabeledSlider {
                            visible: root.manual
                            width: parent.width
                            height: 34
                            label: budget.label
                            suffix: "%"
                            minValue: 10
                            maxValue: 100
                            value: budget.draft
                            onDragStarted: budget.dragging = true
                            onMoved: (v) => budget.draft = Math.round(v)
                            onDragFinished: {
                                budget.dragging = false
                                SettingsService.setValue(budget.settingKey, Math.round(budget.draft))
                            }
                        }

                        Text {
                            visible: root.manual
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            text: budget.used < 0 ? qsTr("Usage not measured on this machine")
                                                  : qsTr("Using %1% now · cap %2%").arg(budget.used).arg(budget.capNow)
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSub
                        }
                    }

                    BudgetRow {
                        label: qsTr("GPU")
                        settingKey: "smart.gpuBudgetPct"
                        cap: SettingsService.caps.gpu
                        used: TelemetryService.health.gpuPct === undefined ? -1 : TelemetryService.health.gpuPct
                    }
                    BudgetRow {
                        label: qsTr("CPU")
                        settingKey: "smart.cpuBudgetPct"
                        cap: SettingsService.caps.cpu
                        used: TelemetryService.health.cpuPct === undefined ? -1 : TelemetryService.health.cpuPct
                    }
                }
            }

            // ---- Live resource gauges card ----
            Rectangle {
                width: parent.width
                height: liveCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: liveCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    Text {
                        text: qsTr("Live resources")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Row {
                        id: liveRow
                        width: parent.width
                        spacing: 12
                        readonly property real dialW: (width - spacing * 2) / 3
                        // Every dial is the same height (arc + caption), so the
                        // row's baseline is one line, not three.

                        GlowDial {
                            width: liveRow.dialW
                            height: implicitHeight
                            measured: TelemetryService.health.cpuPct === undefined || TelemetryService.health.cpuPct >= 0
                            value: measured ? TelemetryService.health.cpuPct : 0
                            unit: "%"
                            caption: qsTr("CPU")
                            dialColor: root.budgetColor(TelemetryService.health.cpuPct, SettingsService.caps.cpu)
                        }
                        GlowDial {
                            width: liveRow.dialW
                            height: implicitHeight
                            measured: TelemetryService.health.gpuPct === undefined || TelemetryService.health.gpuPct >= 0
                            value: measured ? TelemetryService.health.gpuPct : 0
                            unit: "%"
                            caption: qsTr("GPU")
                            dialColor: root.budgetColor(TelemetryService.health.gpuPct, SettingsService.caps.gpu)
                        }
                        GlowDial {
                            width: liveRow.dialW
                            height: implicitHeight
                            measured: TelemetryService.health.batteryPct === undefined || TelemetryService.health.batteryPct >= 0
                            value: measured ? TelemetryService.health.batteryPct : 0
                            unit: "%"
                            // Three states, like the engine's battery row: on battery,
                            // charging (plugged in and topping up), and on AC holding.
                            caption: TelemetryService.health.onBattery === true ? qsTr("Battery \u00B7 on battery")
                                   : TelemetryService.health.charging === true ? qsTr("Battery \u00B7 charging")
                                   : qsTr("Battery")
                            dialColor: root.batteryColor(TelemetryService.health.batteryPct)
                        }
                    }
                }
            }
        }
    }

    // The live resource-health modal ("Live health" in the budgets card).
    // Reads TelemetryService.health on its own, so it stays live while open.
    ResourceHealthModal {
        id: healthModal
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