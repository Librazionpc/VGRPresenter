import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// "Add Timer" configurator — pops up when "Timer" is picked from the Edit
// screen's "+" Add Content menu, matching
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Add_Timer.qml's timer_modal_*/
// timer_mode_*/timer_{hh,mm,ss}_*/timer_toggle*_*/timer_btn_* elements.
// Same convention as CameraSourceModal.qml/MediaSourceModal.qml: scrim +
// centered card, `open` to show/hide, `applied`/`cancelled` signals, no
// application state of its own beyond the picker UI.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // "countdown" | "countup" | "timeofday"
    property string mode: "countdown"
    property int hh: 0
    property int mm: 5
    property int ss: 0
    property bool autoStart: true
    property bool showOnStage: false

    readonly property int durationSeconds: root.hh * 3600 + root.mm * 60 + root.ss

    // { mode, durationSeconds, autoStart, showOnStage } — the consumer
    // decides what a "timer" canvas item does with it (see LiveClock.qml's
    // timerSeconds() for the shared countdown/count-up/time-of-day math).
    signal applied(var config)
    signal cancelled()

    function clamp(v, lo, hi) {
        return Math.max(lo, Math.min(hi, v))
    }
    function pad(n) {
        return n < 10 ? "0" + n : String(n)
    }

    anchors.fill: parent
    visible: root.open

    // The dialog belongs to the WINDOW, not to the pane that declares it. A modal
    // declared inside a settings page (every Add / Edit dialog in Settings) must dim
    // the whole app, not just that page's content column: parented in place its scrim
    // covered one side of the settings card, so the dialog read as half-modal (user
    // report: "the black box doesn't cover the whole settings modal, only one side").
    // Reparented to the window overlay, this component has no scaled ancestor to
    // inherit a scale from, so its CARD applies the app scale itself (see AppScale.qml).
    parent: Overlay.overlay

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        // The app's one scale, on the card (see AppScale.qml) — never on the root,
        // whose scrim IS the dialog's black box and has to stay window-sized.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        anchors.centerIn: parent
        width: 480
        height: content.height + 40
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1

        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 16

            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Add Timer")
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
                        Behavior on color { ColorAnimation { duration: 100 } }

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
                    text: qsTr("Configure a countdown for this slide")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("Mode")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                Row {
                    width: parent.width
                    spacing: 8

                    Repeater {
                        model: [
                            { key: "countdown", label: qsTr("Countdown") },
                            { key: "countup", label: qsTr("Count Up") },
                            { key: "timeofday", label: qsTr("Time of Day") }
                        ]
                        delegate: Rectangle {
                            id: modeBtn
                            required property var modelData
                            readonly property bool active: root.mode === modeBtn.modelData.key

                            width: (parent.width - 16) / 3
                            height: 32
                            radius: 8
                            color: modeBtn.active ? Theme.accent : (modeArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.color: modeBtn.active ? Theme.accent : Theme.borderSubtle
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                anchors.centerIn: parent
                                text: modeBtn.modelData.label
                                color: modeBtn.active ? "#ffffff" : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: modeBtn.active ? Font.DemiBold : Font.Medium
                            }

                            MouseArea {
                                id: modeArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.mode = modeBtn.modelData.key
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: root.mode === "timeofday" ? qsTr("Time of Day") : qsTr("Duration")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                Row {
                    spacing: 8

                    Repeater {
                        model: [
                            { key: "hh", label: qsTr("HH"), value: root.hh, max: 23 },
                            { key: "mm", label: qsTr("MM"), value: root.mm, max: 59 },
                            { key: "ss", label: qsTr("SS"), value: root.ss, max: 59 }
                        ]
                        delegate: Rectangle {
                            id: fieldBox
                            required property var modelData
                            readonly property bool focused: numInput.activeFocus

                            width: 64
                            height: 48
                            radius: 8
                            color: fieldBox.focused ? Qt.alpha(Theme.accent, 0.12) : Theme.inset
                            border.color: fieldBox.focused ? Theme.accent : Theme.borderSubtle
                            border.width: fieldBox.focused ? 1.5 : 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Column {
                                anchors.centerIn: parent
                                spacing: 2

                                TextInput {
                                    id: numInput
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: fieldBox.modelData.value < 10 ? "0" + fieldBox.modelData.value : String(fieldBox.modelData.value)
                                    color: fieldBox.focused ? Theme.accentLight : Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 23
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    validator: IntValidator { bottom: 0; top: fieldBox.modelData.max }
                                    selectByMouse: true
                                    onEditingFinished: {
                                        const v = root.clamp(parseInt(text || "0", 10), 0, fieldBox.modelData.max)
                                        if (fieldBox.modelData.key === "hh") root.hh = v
                                        else if (fieldBox.modelData.key === "mm") root.mm = v
                                        else root.ss = v
                                        text = root.pad(v)
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: fieldBox.modelData.label
                                    color: fieldBox.focused ? Theme.accentLight : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }
                }
            }

            // Toggles — "Start automatically..." / "Show on stage display".
            Repeater {
                model: [
                    { key: "autoStart", label: qsTr("Start automatically when slide goes live") },
                    { key: "showOnStage", label: qsTr("Show on stage display") }
                ]
                delegate: Item {
                    required property var modelData
                    width: content.width
                    height: 24

                    Text {
                        anchors.left: parent.left
                        anchors.right: toggleTrack.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    Rectangle {
                        id: toggleTrack
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property bool on: modelData.key === "autoStart" ? root.autoStart : root.showOnStage
                        width: 44
                        height: 24
                        radius: 12
                        color: toggleTrack.on ? Theme.accent : Theme.borderSubtle
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Rectangle {
                            x: toggleTrack.on ? parent.width - width - 2 : 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            radius: 10
                            color: "#ffffff"
                            Behavior on x { NumberAnimation { duration: 100 } }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (modelData.key === "autoStart")
                                    root.autoStart = !root.autoStart
                                else
                                    root.showOnStage = !root.showOnStage
                            }
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 90
                    height: 32
                    radius: 9
                    color: cancelArea.containsMouse ? Theme.hoverBg : Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelled()
                    }
                }

                Rectangle {
                    width: 100
                    height: 32
                    radius: 9
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add Timer")
                        color: "#ffffff"
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied({
                            mode: root.mode,
                            durationSeconds: root.durationSeconds,
                            autoStart: root.autoStart,
                            showOnStage: root.showOnStage
                        })
                    }
                }
            }
        }
    }
}
