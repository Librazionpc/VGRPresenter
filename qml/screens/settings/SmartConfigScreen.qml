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

                readonly property bool inForce: root.values["resources.profile"] === SettingsService.recommendedProfile

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

                    Text {
                        text: qsTr("Resource budgets")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }

                    Text {
                        width: parent.width
                        text: root.manual ? qsTr("Manual mode: drag to set how much of the machine the engine may use.")
                                          : qsTr("Set by the %1 profile. Switch to Manual to choose your own.").arg(root.profileLabel(root.values["resources.profile"]))
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        wrapMode: Text.Wrap
                    }

                    // One budget: a meter of what the engine uses (the profile's number), or - in Manual - a slider that sets it.
                    component BudgetRow: Item {
                        id: budget
                        property string label: ""
                        property string settingKey: ""       // the engine's setting behind it in Manual mode
                        property int shown: 0                // the number in force (percent)
                        property real draft: budget.shown
                        property bool dragging: false
                        width: parent.width
                        height: 34

                        Binding { target: budget; property: "draft"; value: budget.shown; when: !budget.dragging }

                        LabeledMeter { visible: !root.manual; anchors.fill: parent; label: budget.label; pct: budget.shown }
                        LabeledSlider {
                            visible: root.manual
                            anchors.fill: parent
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
                    }

                    BudgetRow { label: qsTr("GPU"); settingKey: "smart.gpuBudgetPct"; shown: SettingsService.caps.gpu }
                    BudgetRow { label: qsTr("CPU"); settingKey: "smart.cpuBudgetPct"; shown: SettingsService.caps.cpu }
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
