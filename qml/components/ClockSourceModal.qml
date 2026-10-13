import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// "Add Clock" configurator — pops up when "Clock" is picked from the Edit
// screen's "+" Add Content menu, matching
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Add_Clock.qml's clock_modal_*/
// clock_fmt_*/clock_style_*/clock_toggle*_*/clock_btn_* elements. Same
// convention as CameraSourceModal.qml/MediaSourceModal.qml: scrim + centered
// card, `open` to show/hide, `applied`/`cancelled` signals, no application
// state of its own beyond the picker UI.
//
// The live preview (and the eventual canvas visual, see LiveClock.qml) both
// run off the same shared ticking component instead of duplicating a
// second-by-second Timer + formatting logic here.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    property string format: "12"
    property string style: "digital"
    property bool showSeconds: true
    property bool showDate: false
    // The digits' / hands' colour, and whether the clock sits in a dark box or on nothing (transparent - the canvas
    // shows that over its checkerboard, and it renders out transparent).
    property string tint: "#9b8ff5"
    property string background: "dark"       // "dark" | "none"
    readonly property var tints: ["#9b8ff5", "#ffffff", "#ff4d3d", "#2ecc71", "#f1c40f", "#3b82f6"]

    // { format, style, showSeconds, showDate, color, background } — the consumer decides what
    // a "clock" canvas item does with it.
    signal applied(var config)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    LiveClock {
        id: previewClock
        running: root.open
    }

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
        width: 440
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
                        text: qsTr("Add Clock")
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
                    text: qsTr("Configure a live clock display for this slide")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            // Live preview — ticks for real via previewClock above, so what
            // you see here is exactly what the canvas item will show.
            Rectangle {
                width: parent.width
                height: 90
                radius: 10
                color: root.background === "none" ? "#15161d" : "#0d0f16"
                border.color: Theme.borderSubtle
                border.width: 1
                clip: true

                // "None" is shown the way the canvas shows transparency: a checkerboard.
                Grid {
                    visible: root.background === "none"
                    anchors.fill: parent
                    columns: 20
                    Repeater {
                        model: 20 * 9
                        delegate: Rectangle {
                            required property int index
                            width: 10; height: 10
                            color: (Math.floor(index / 20) + (index % 20)) % 2 === 0 ? "#1c1e29" : "#12131a"
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    color: root.tint
                    font.family: Theme.fontFamily
                    font.pixelSize: 32
                    font.weight: Font.DemiBold
                    text: previewClock.formatClock(previewClock.now, root.format === "12", root.showSeconds)
                }
            }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("Format")
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
                            { key: "12", label: qsTr("12-hour") },
                            { key: "24", label: qsTr("24-hour") }
                        ]
                        delegate: Rectangle {
                            id: fmtBtn
                            required property var modelData
                            readonly property bool active: root.format === fmtBtn.modelData.key

                            width: (parent.width - 8) / 2
                            height: 32
                            radius: 8
                            color: fmtBtn.active ? Theme.accent : (fmtArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.color: fmtBtn.active ? Theme.accent : Theme.borderSubtle
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                anchors.centerIn: parent
                                text: fmtBtn.modelData.label
                                color: fmtBtn.active ? "#ffffff" : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: fmtBtn.active ? Font.DemiBold : Font.Medium
                            }

                            MouseArea {
                                id: fmtArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.format = fmtBtn.modelData.key
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("Style")
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
                            { key: "digital", label: qsTr("Digital") },
                            { key: "analog", label: qsTr("Analog") }
                        ]
                        delegate: Rectangle {
                            id: styleBtn
                            required property var modelData
                            readonly property bool active: root.style === styleBtn.modelData.key

                            width: (parent.width - 8) / 2
                            height: 32
                            radius: 8
                            color: styleBtn.active ? Theme.accent : (styleArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.color: styleBtn.active ? Theme.accent : Theme.borderSubtle
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                anchors.centerIn: parent
                                text: styleBtn.modelData.label
                                color: styleBtn.active ? "#ffffff" : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: styleBtn.active ? Font.DemiBold : Font.Medium
                            }

                            MouseArea {
                                id: styleArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.style = styleBtn.modelData.key
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("Colour")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                Row {
                    spacing: 10

                    Repeater {
                        model: root.tints
                        delegate: Rectangle {
                            id: tintDot
                            required property string modelData
                            readonly property bool active: root.tint === modelData

                            width: 28
                            height: 28
                            radius: 14
                            color: modelData
                            border.width: tintDot.active ? 2 : 1
                            border.color: tintDot.active ? Theme.accent : Theme.borderSubtle

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.tint = tintDot.modelData
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("Background")
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
                            { key: "dark", label: qsTr("Dark box") },
                            { key: "none", label: qsTr("None (transparent)") }
                        ]
                        delegate: Rectangle {
                            id: bgBtn
                            required property var modelData
                            readonly property bool active: root.background === bgBtn.modelData.key

                            width: (parent.width - 8) / 2
                            height: 32
                            radius: 8
                            color: bgBtn.active ? Theme.accent : (bgArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.color: bgBtn.active ? Theme.accent : Theme.borderSubtle
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                anchors.centerIn: parent
                                text: bgBtn.modelData.label
                                color: bgBtn.active ? "#ffffff" : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: bgBtn.active ? Font.DemiBold : Font.Medium
                            }

                            MouseArea {
                                id: bgArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.background = bgBtn.modelData.key
                            }
                        }
                    }
                }
            }

            // Toggles — "Show seconds" / "Show date".
            Repeater {
                model: [
                    { key: "seconds", label: qsTr("Show seconds") },
                    { key: "date", label: qsTr("Show date") }
                ]
                delegate: Item {
                    required property var modelData
                    width: content.width
                    height: 24

                    Text {
                        anchors.left: parent.left
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
                        readonly property bool on: modelData.key === "seconds" ? root.showSeconds : root.showDate
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
                                if (modelData.key === "seconds")
                                    root.showSeconds = !root.showSeconds
                                else
                                    root.showDate = !root.showDate
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
                    width: 90
                    height: 32
                    radius: 9
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add Clock")
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
                            format: root.format,
                            style: root.style,
                            showSeconds: root.showSeconds,
                            showDate: root.showDate,
                            color: root.tint,
                            background: root.background
                        })
                    }
                }
            }
        }
    }
}
