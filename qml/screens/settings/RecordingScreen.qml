import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Recording & Streaming — same skeleton as the other settings
// screens (Item root, Flickable, shared AppScrollBar, Theme tokens). REAL
// recording: every field persists to session.recordingConfig through
// RecordingService.config, the free-disk meter reads the ENGINE's storage
// seam, and the transport card starts/stops a real RecordingEngine session
// on the program bus (1 Hz status: elapsed, size, encoder, disk health).
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

    // ---- persisted settings (RecordingService.config) --------------------
    // Loaded once on bind (merged over these defaults), then every change
    // writes the whole blob back — the screen comes back exactly as left.
    Component.onCompleted: {
        const c = RecordingService.config
        if (c.platform !== undefined) root.platform = c.platform
        if (c.serverUrl !== undefined) root.serverUrl = c.serverUrl
        if (c.streamKey !== undefined) root.streamKey = c.streamKey
        if (c.videoBitrate !== undefined) root.videoBitrate = c.videoBitrate
        if (c.audioBitrate !== undefined) root.audioBitrate = c.audioBitrate
        if (c.frameRate !== undefined) root.frameRate = c.frameRate
        if (c.resolution !== undefined) root.resolution = c.resolution
        if (c.container !== undefined) root.container = c.container
        if (c.encoder !== undefined) root.encoder = c.encoder
        if (c.screensIn !== undefined) root.screensIn = c.screensIn
        root.pushConfig()
    }
    function pushConfig() {
        RecordingService.config = {
            platform: root.platform, serverUrl: root.serverUrl, streamKey: root.streamKey,
            videoBitrate: root.videoBitrate, audioBitrate: root.audioBitrate,
            frameRate: root.frameRate, resolution: root.resolution,
            container: root.container, encoder: root.encoder, screensIn: root.screensIn
        }
    }
    // Any field change lands in the store (property-change funnel).
    onPlatformChanged: root.pushConfig()
    onServerUrlChanged: root.pushConfig()
    onStreamKeyChanged: root.pushConfig()
    onVideoBitrateChanged: root.pushConfig()
    onAudioBitrateChanged: root.pushConfig()
    onFrameRateChanged: root.pushConfig()
    onResolutionChanged: root.pushConfig()
    onContainerChanged: root.pushConfig()
    onEncoderChanged: root.pushConfig()
    onScreensInChanged: root.pushConfig()

    // Simple derived readiness state for the header pill.
    readonly property bool ready: platform !== "" && streamKey !== ""

    // The live transport card reads the service's 1 Hz status.
    readonly property bool recActive: RecordingService.recording
    readonly property var recStatus: RecordingService.status
    readonly property var recStorage: RecordingService.storage

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
                            font.pixelSize: 16
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
                                    font.pixelSize: 10
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
                                color: platChip.active ? Theme.accentSoft : (platArea.containsMouse ? Theme.chip : Theme.inset)
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

                    // The server URL/key are also the STREAM destination:
                    // every keystroke re-applies it to the graph (only while
                    // a destination exists — the first Apply is Enter).
                    SettingsField {
                        width: (parent.width - 16) / 2
                        label: qsTr("Server URL")
                        text: root.serverUrl
                        onTextEdited: (t) => {
                            root.serverUrl = t
                            if (RecordingService.streaming)
                                RecordingService.applyStreamTarget(root.serverUrl, root.streamKey)
                        }
                        onAccepted: if (root.ready)
                            RecordingService.applyStreamTarget(root.serverUrl, root.streamKey)
                    }
                    SettingsField {
                        width: (parent.width - 16) / 2
                        label: qsTr("Stream key")
                        text: root.streamKey
                        echoMode: TextInput.Password
                        placeholder: "•••• •••• ••••"
                        onTextEdited: (t) => {
                            root.streamKey = t
                            if (RecordingService.streaming)
                                RecordingService.applyStreamTarget(root.serverUrl, root.streamKey)
                        }
                        onAccepted: if (root.ready)
                            RecordingService.applyStreamTarget(root.serverUrl, root.streamKey)
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
                            font.pixelSize: 16
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

            // ---- Transport card — REAL recording state + controls ----------
            Rectangle {
                width: parent.width
                height: transportCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.width: 1
                border.color: root.recActive ? "#85261f" : Theme.border

                Column {
                    id: transportCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 12

                    Item {
                        width: parent.width
                        height: 22

                        Text {
                            text: qsTr("Transport")
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        // Live state pill — driven by the engine session, not
                        // a readiness guess.
                        Rectangle {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: liveLabel.width + 22
                            height: 20
                            radius: 10
                            color: root.recActive ? "#2a1210" : Theme.inset
                            border.width: 1
                            border.color: root.recActive ? "#e05a4e" : Theme.border

                            Row {
                                anchors.centerIn: parent
                                spacing: 6

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6; height: 6; radius: 3
                                    color: root.recActive ? "#e05a4e" : Theme.textMuted

                                    SequentialAnimation on opacity {
                                        running: root.recActive && !root.paused
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 1; to: 0.25; duration: 600 }
                                        NumberAnimation { from: 0.25; to: 1; duration: 600 }
                                    }
                                }
                                Text {
                                    id: liveLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: !root.recActive ? qsTr("IDLE")
                                        : root.paused ? qsTr("PAUSED")
                                        : qsTr("RECORDING")
                                    color: root.recActive ? "#ff8d7f" : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                }
                            }
                        }
                    }

                    // The controls + the live readouts. Disabled look while
                    // idle is unnecessary — START is the idle state's action.
                    Row {
                        width: parent.width
                        spacing: 10

                        Rectangle {
                            width: startLabel.width + 34
                            height: 36
                            radius: Theme.radiusMd
                            color: root.recActive ? "#241a19" : "#c8321e"
                            border.width: 1
                            border.color: root.recActive ? "#85261f" : "transparent"
                            Row {
                                anchors.centerIn: parent
                                spacing: 8
                                IconGlyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: root.recActive ? "stop" : "play"
                                    color: root.recActive ? "#ff8d7f" : "#ffffff"
                                    fit: true; width: 14; height: 14
                                }
                                Text {
                                    id: startLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.recActive ? qsTr("Stop recording") : qsTr("Start recording")
                                    color: root.recActive ? "#ff8d7f" : "#ffffff"
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.DemiBold
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (root.recActive)
                                        RecordingService.stopRecording()
                                    else
                                        RecordingService.startRecording({
                                            container: root.container,
                                            encoder: root.encoder,
                                            resolution: root.resolution,
                                            frameRate: root.frameRate,
                                            videoBitrate: root.videoBitrate,
                                            audioBitrate: root.audioBitrate,
                                            screensIn: root.screensIn
                                        })
                                }
                            }
                        }

                        // Pause/resume (only meaningful mid-session).
                        Rectangle {
                            visible: root.recActive
                            width: 36; height: 36; radius: Theme.radiusMd
                            color: Theme.inset
                            border.width: 1
                            border.color: Theme.border
                            IconGlyph {
                                anchors.centerIn: parent
                                name: root.paused ? "play" : "pause"
                                color: Theme.textPrimary
                                fit: true; width: 14; height: 14
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: RecordingService.togglePause()
                            }
                        }

                        // Marker: notes "now" into the session (sermon start,
                        // a song — searchable later via the engine's metadata).
                        Rectangle {
                            visible: root.recActive && !root.paused
                            width: markerLabel.width + 26
                            height: 36
                            radius: Theme.radiusMd
                            color: Theme.inset
                            border.width: 1
                            border.color: Theme.border
                            Row {
                                anchors.centerIn: parent
                                spacing: 7
                                IconGlyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "flag"
                                    color: Theme.textSecondary
                                    fit: true; width: 13; height: 13
                                }
                                Text {
                                    id: markerLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Mark moment")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: RecordingService.addMarker(
                                    new Date().toLocaleTimeString(Qt.locale(), "h:mm:ss"))
                            }
                        }
                    }

                    // Live readouts — every number from the engine session.
                    Row {
                        visible: root.recActive
                        width: parent.width
                        spacing: 22

                        Column {
                            spacing: 2
                            Text { text: qsTr("Elapsed"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.textXs }
                            Text {
                                text: {
                                    const s = Math.floor((root.recStatus.durationMs ?? 0) / 1000)
                                    return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
                                }
                                color: Theme.textPrimary
                                font.family: "Consolas"; font.pixelSize: 15
                            }
                        }
                        Column {
                            spacing: 2
                            Text { text: qsTr("File size (est.)"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.textXs }
                            Text {
                                text: (root.recStatus.sizeMb ?? 0).toFixed(1) + " MB"
                                color: Theme.textPrimary
                                font.family: "Consolas"; font.pixelSize: 15
                            }
                        }
                        Column {
                            spacing: 2
                            Text { text: qsTr("Encoder"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.textXs }
                            Text {
                                text: root.recStatus.encoder ?? "—"
                                color: Theme.textPrimary
                                font.family: "Consolas"; font.pixelSize: 15
                            }
                        }
                        Column {
                            spacing: 2
                            Text { text: qsTr("Dropped frames"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: Theme.textXs }
                            Text {
                                text: String(root.recStatus.droppedFrames ?? 0)
                                color: (root.recStatus.droppedFrames ?? 0) > 0 ? "#ffb454" : Theme.textPrimary
                                font.family: "Consolas"; font.pixelSize: 15
                            }
                        }
                    }

                    // The file being written (what to hand the media team).
                    Text {
                        visible: root.recActive && String(root.recStatus.filePath ?? "") !== ""
                        width: parent.width
                        text: root.recStatus.filePath ?? ""
                        color: Theme.textMuted
                        elide: Text.ElideMiddle
                        font.family: "Consolas"; font.pixelSize: Theme.textXs
                    }
                }
            }

            // ---- Recording card (format picks + REAL free-disk meter) ------
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
                        text: qsTr("Recording format")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
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
                                            color: contChip.active ? Theme.accentSoft : Theme.inset
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
                                            color: encChip.active ? Theme.accentSoft : Theme.inset
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

                        // Free disk space — REAL, from the engine's storage
                        // seam (bytes free + hours of headroom at the current
                        // bitrate). Amber under 20%, red under 10% — the same
                        // thresholds the engine's disk policy acts on.
                        LabeledMeter {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 200
                            label: (root.recStorage.freeGb ?? 0).toFixed(0) + " GB free · "
                                   + (root.recStorage.capacityHours ?? 0).toFixed(1) + " h"
                            pct: root.recStorage.freePercent ?? 100
                            barColor: (root.recStorage.freePercent ?? 100) < 10
                                        ? "#c8321e"
                                        : (root.recStorage.freePercent ?? 100) < 20
                                          ? "#ffb454" : Theme.success
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
                        font.pixelSize: 16
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
