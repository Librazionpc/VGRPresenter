import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Smart Config — rebuilt from the reference screenshot
// (1BBTIwaya's VGRPresenter_Settings_Smart_Config.qml is a PNG placeholder
// export — a bare modal.png image like SizeStyleCard's source was — so the
// visible design in the screenshot is ground truth): a Configuration mode
// selector (Strict / Smart / Manual), a Hardware detected card whose rows
// show a green check + a value, and Resource budgets meters.
//
// Same structure as GeneralScreen: an Item root (scrollbar stays fixed
// at the edge while content scrolls), Flickable + Column inside, shared
// AppScrollBar, Theme tokens throughout. Content is short enough that it
// doesn't scroll at the dialog's default size, but the Flickable stays so
// it degrades gracefully at small window sizes.
//
// It is the ENGINE's page: the hardware rows, their wording and the recommended profile come from the engine's own detection
// (SettingsService.hardwareRows, from bps::settings::BuildHardwareReport), the three modes and their descriptions are the engine's
// list, and the numbers are the engine's defaults - each resource profile has its own GPU / CPU share (80% / 60% for Performance),
// which the engine applies to its adaptive runtime (the CPU share bounds the worker threads, the GPU share the texture budget).
// In Manual mode the two budgets are yours to set; in Smart and Strict they follow the profile.
Item {
    id: root

    readonly property var values: SettingsService.values
    readonly property var modeChoices: SettingsService.definitions["smart.mode"].choices
    readonly property bool manual: root.values["smart.mode"] === "manual"

    function profileLabel(value) {
        const choices = SettingsService.definitions["resources.profile"].choices
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].value === value)
                return choices[i].label
        return ""
    }
    function modeText(key) {
        for (let i = 0; i < root.modeChoices.length; ++i)
            if (root.modeChoices[i].value === key)
                return root.modeChoices[i].description
        return ""
    }

    // ---- Glow-dial thresholds ----
    // The CPU / GPU dials are the BUDGET the sliders set: green while there is
    // headroom, amber once usage is within 15% of the cap, red once it is past
    // it. `cap` is the setting in force (the profile's number, or the slider's
    // own in Manual) — so moving the slider moves the glow.
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
                    text: qsTr("Smart Config")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Let VGR analyze your hardware and optimize the production pipeline.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
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
                                Behavior on color { ColorAnimation { duration: 100 } }

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
                                    }
                                    Text {
                                        width: modeCell.width - 28
                                        text: modeCell.modelData.description
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        elide: Text.ElideRight
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

                // Is the engine running on the recommendation? Selecting the profile is only half of it: in Manual mode the
                // caps IN FORCE are the sliders' own numbers, so moving one off the profile's numbers means the recommendation is
                // no longer what the engine uses - the button goes back to "Apply" the moment you touch a slider.
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
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: parent.parent.inForce ? qsTr("In use") : qsTr("Apply")
                        color: "#ffffff"
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
                        font.pixelSize: Theme.textXs
                        bottomPadding: 10
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: SettingsService.hardwareAdvice
                        delegate: Item {
                            id: adviceRow
                            required property var modelData
                            width: adviceCol.width
                            height: adviceText.implicitHeight + 12

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
                                anchors.leftMargin: 12
                                width: parent.width - 12
                                text: adviceRow.modelData.text
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textSm
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

                        // The live resource-health modal (CPU / GPU / memory /
                        // temperature plus the throttling speedometer).
                        //
                        // A WRAPPING Item holds the hit area, NOT the Row: a Row
                        // lays out its children horizontally and refuses the
                        // left/right/fill/centerIn anchors a MouseArea needs to
                        // cover the pill ("Row will not function"). The Row sits
                        // inside the Item and keeps its own layout; the MouseArea
                        // fills the Item (allowed), so the whole icon+caption is
                        // the click target.
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
                              : qsTr("Set by the %1 profile. The bright bar is what the engine is using right now; the pale band is the share this profile allows.").arg(root.profileLabel(root.values["resources.profile"]))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        wrapMode: Text.Wrap
                    }

                    // One budget: the LIVE usage against the share allowed (the
                    // profile's number, or — in Manual — your own, set with the
                    // slider under the meter).
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
                            font.pixelSize: Theme.textXs
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
            // The glowing-dial readout, under the sliders: CPU, GPU and
            // battery as the Audio & Video channel knob's glow arc. The CPU /
            // GPU dials glow amber as usage nears the budget the sliders set
            // and red once past it — the dial IS the budget, read live, not a
            // separate number. Package temperature is NOT here: plenty of
            // machines expose no package sensor, so that tile sat permanently
            // empty next to three live ones. It stays in the health modal,
            // where its own "Not measured" wording has room to explain itself.
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
                        // Three dials now (the Package-temp tile was removed -
                        // a desktop reports no package sensor on many machines,
                        // so it sat empty while the rest of the row was live).
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
                            caption: TelemetryService.health.onBattery === true ? qsTr("Battery \u00B7 on battery") : qsTr("Battery")
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
