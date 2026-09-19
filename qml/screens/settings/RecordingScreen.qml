import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Recording & Streaming — same skeleton as the other settings
// screens (Item root, Flickable, shared AppScrollBar, Theme tokens), but
// deliberately MINIMAL per the design direction: only what's needed, no
// busy chrome. That means: platform choice + connection fields, a compact
// quality grid, container/encoder picks with a free-disk bar, and the four
// screens as simple toggle rows (the reference's per-screen thumbnail
// cards with LIVE/REC/STREAM badges read as clutter at settings density —
// those badges belong to the live production view, not setup).
Item {
    id: root

    // "youtube" | "twitch" | "facebook" | "rtmp"
    property string platform: "youtube"
    readonly property var platforms: [
        { key: "youtube", label: qsTr("YouTube") },
        { key: "twitch", label: qsTr("Twitch") },
        { key: "facebook", label: qsTr("Facebook") },
        { key: "rtmp", label: qsTr("Custom RTMP") }
    ]

    property string serverUrl: "rtmp://a.rtmp.youtube.com/live2"
    property string streamKey: ""
    property string videoBitrate: "6000"
    property string audioBitrate: "192"
    property string frameRate: "60 fps"
    property string resolution: "1080p"
    property string container: "MP4"
    property string encoder: "NVENC"
    // Which of the production screens are included in the recording.
    property var screensIn: ({ program: true, preview: false, audience: true, lowerThird: true })

    // Simple derived readiness state for the header pill.
    readonly property bool ready: platform !== "" && streamKey !== ""

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
                    text: qsTr("Recording & Streaming")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Record or stream your service — platform, quality, screens.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Platform card ----
            Rectangle {
                width: parent.width
                height: platformCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: platformCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 12

                    Item {
                        width: parent.width
                        height: 22

                        Text {
                            text: qsTr("Stream platform")
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        // Readiness pill — honest state: green READY only
                        // once a stream key exists.
                        Rectangle {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: statusLabel.width + 22
                            height: 20
                            radius: 10
                            color: root.ready ? "#264ade80" : Theme.inset
                            border.width: 1
                            border.color: root.ready ? Theme.success : Theme.border

                            Row {
                                anchors.centerIn: parent
                                spacing: 6

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6; height: 6; radius: 3
                                    color: root.ready ? Theme.success : Theme.textMuted
                                }
                                Text {
                                    id: statusLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.ready ? qsTr("READY") : qsTr("KEY NEEDED")
                                    color: root.ready ? Theme.success : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                }
                            }
                        }
                    }

                    Row {
                        width: parent.width
                        spacing: 10

                        Repeater {
                            model: root.platforms
                            delegate: Rectangle {
                                id: platChip
                                required property var modelData
                                readonly property bool active: root.platform === platChip.modelData.key

                                width: platLabel.width + 26
                                height: 30
                                radius: Theme.radiusMd
                                color: platChip.active ? "#266C5CE7" : (platArea.containsMouse ? Theme.chip : Theme.inset)
                                border.width: 1
                                border.color: platChip.active ? Theme.accent : Theme.border
                                Behavior on color { ColorAnimation { duration: 100 } }

                                Text {
                                    id: platLabel
                                    anchors.centerIn: parent
                                    text: platChip.modelData.label
                                    color: platChip.active ? Theme.accentLight : Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: platChip.active ? Font.DemiBold : Font.Medium
                                }

                                MouseArea {
                                    id: platArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.platform = platChip.modelData.key
                                }
                            }
                        }
                    }

                    Row {
                        width: parent.width
                        spacing: 16

                        SettingsField {
                            width: (parent.width - 16) / 2
                            label: qsTr("Server URL")
                            text: root.serverUrl
                            onTextChanged: root.serverUrl = text
                        }
                        SettingsField {
                            width: (parent.width - 16) / 2
                            label: qsTr("Stream key")
                            text: root.streamKey
                            echoMode: TextInput.Password
                            placeholder: "•••• •••• ••••"
                            onTextChanged: root.streamKey = text
                        }
                    }
                }
            }

            // ---- Quality card ----
            Rectangle {
                width: parent.width
                height: qualityCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: qualityCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 12

                    Item {
                        width: parent.width
                        height: 18

                        Text {
                            text: qsTr("Video & audio encoding")
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("shared for record + stream")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                        }
                    }

                    // One compact 2×2 grid — four values, nothing else.
                    Grid {
                        width: parent.width
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 10

                        SettingsField { width: (parent.width - 16) / 2; label: qsTr("Video bitrate"); text: root.videoBitrate; suffix: "kbps"; onTextChanged: root.videoBitrate = text }
                        SettingsField { width: (parent.width - 16) / 2; label: qsTr("Frame rate"); text: root.frameRate; onTextChanged: root.frameRate = text }
                        SettingsField { width: (parent.width - 16) / 2; label: qsTr("Audio bitrate"); text: root.audioBitrate; suffix: "kbps"; onTextChanged: root.audioBitrate = text }
                        SettingsField { width: (parent.width - 16) / 2; label: qsTr("Resolution"); text: root.resolution; onTextChanged: root.resolution = text }
                    }
                }
            }

            // ---- Recording card ----
            Rectangle {
                width: parent.width
                height: recCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: recCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 12

                    Text {
                        text: qsTr("Recording")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Item {
                        width: parent.width
                        height: recChoiceRow.height

                        Row {
                            id: recChoiceRow
                            spacing: 16

                            Column {
                                spacing: 6

                                Text {
                                    text: qsTr("Container")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                                Row {
                                    spacing: 6

                                    Repeater {
                                        model: ["MKV", "MP4"]
                                        delegate: Rectangle {
                                            id: contChip
                                            required property string modelData
                                            readonly property bool active: root.container === contChip.modelData
                                            width: contLabel.width + 22
                                            height: 28
                                            radius: Theme.radiusMd
                                            color: contChip.active ? "#266C5CE7" : Theme.inset
                                            border.width: 1
                                            border.color: contChip.active ? Theme.accent : Theme.border

                                            Text {
                                                id: contLabel
                                                anchors.centerIn: parent
                                                text: contChip.modelData
                                                color: contChip.active ? Theme.accentLight : Theme.textSecondary
                                                font.family: Theme.fontFamily
                                                font.pixelSize: Theme.textSm
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.container = contChip.modelData
                                            }
                                        }
                                    }
                                }
                            }

                            Column {
                                spacing: 6

                                Text {
                                    text: qsTr("Encoder")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                }
                                Row {
                                    spacing: 6

                                    Repeater {
                                        model: ["NVENC", "AMF", "QuickSync", "Software"]
                                        delegate: Rectangle {
                                            id: encChip
                                            required property string modelData
                                            readonly property bool active: root.encoder === encChip.modelData
                                            width: encLabel.width + 22
                                            height: 28
                                            radius: Theme.radiusMd
                                            color: encChip.active ? "#266C5CE7" : Theme.inset
                                            border.width: 1
                                            border.color: encChip.active ? Theme.accent : Theme.border

                                            Text {
                                                id: encLabel
                                                anchors.centerIn: parent
                                                text: encChip.modelData
                                                color: encChip.active ? Theme.accentLight : Theme.textSecondary
                                                font.family: Theme.fontFamily
                                                font.pixelSize: Theme.textSm
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.encoder = encChip.modelData
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Free disk space — right side, health-green.
                        LabeledMeter {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 180
                            label: qsTr("412 GB free")
                            pct: 68
                            barColor: Theme.success
                        }
                    }
                }
            }

            // ---- Screens card — four simple toggle rows ----
            Rectangle {
                width: parent.width
                height: screensCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: screensCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Screens to record")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }

                    Repeater {
                        model: [
                            { key: "program", label: qsTr("Program"), sub: qsTr("Stage output · 1080p60") },
                            { key: "preview", label: qsTr("Preview"), sub: qsTr("Operator view · 1080p30") },
                            { key: "audience", label: qsTr("Audience"), sub: qsTr("Front center · 1080p30") },
                            { key: "lowerThird", label: qsTr("Lower Third"), sub: qsTr("Screws graphics · 1080p60") }
                        ]
                        delegate: Column {
                            id: screenRow
                            required property var modelData
                            width: screensCol.width

                            Item {
                                width: screenRow.width
                                height: 40

                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1

                                    Text {
                                        text: screenRow.modelData.label
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                    Text {
                                        text: screenRow.modelData.sub
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }

                                SettingsToggle {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: root.screensIn[screenRow.modelData.key] ?? false
                                    onToggled: root.screensIn[screenRow.modelData.key] = !root.screensIn[screenRow.modelData.key]
                                }
                            }
                            Rectangle { width: screenRow.width; height: 1; color: Theme.border; visible: screenRow.index < 3 }
                        }
                    }
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
