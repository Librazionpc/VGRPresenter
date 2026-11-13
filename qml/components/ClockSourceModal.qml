import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // ---- Esc = cancel, Enter = the Add button (qml/settings/Hotkeys.qml) --
    // The picker's own pair, pushed as a Hotkeys scope while it is up.
    property int hotkeyScope: 0
    function accept() {
        root.applied({
            format: root.format,
            style: root.style,
            showSeconds: root.showSeconds,
            showDate: root.showDate,
            color: root.tint,
            background: root.background
        })
    }
    onOpenChanged: {
        // Above whatever dialog is already up: a flat z would open this BEHIND an Edit/Add card (ModalCard bumps its own
        // z on every open), which is how the colour card appeared below the Edit Style card.
        if (root.open)
            root.z = ModalStack.nextZ()
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Clock"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

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

                    ModalCloseButton {
                        id: closeBtn
                        onClicked: root.cancelled()
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
                color: root.background === "none" ? Theme.surface : Theme.windowBg
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
                            color: (Math.floor(index / 20) + (index % 20)) % 2 === 0 ? "#1c1e29" : Theme.panelBg
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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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
                        onClicked: root.accept()
                    }
                }
            }
        }
    }
}