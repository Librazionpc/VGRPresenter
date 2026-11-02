import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root

    // Bumped on any change to either model so the usage counts and the empty
    // state recompute (plain Q_INVOKABLE reads in bindings are not tracked).
    property int modelsRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
    }
    Connections {
        target: PresetListModel
        function onDataChanged() { root.modelsRev++ }
        function onRowsInserted() { root.modelsRev++ }
        function onRowsRemoved() { root.modelsRev++ }
        function onModelReset() { root.modelsRev++ }
    }

    readonly property var kinds: [
        { key: "recording", label: qsTr("Recording") },
        { key: "audio", label: qsTr("Audio") },
        { key: "streaming", label: qsTr("Streaming") }
    ]
    function kindLabel(key) { return key === "streaming" ? qsTr("Streaming") : key === "audio" ? qsTr("Audio") : qsTr("Recording") }

    readonly property var platforms: [
        { key: "youtube", label: qsTr("YouTube"), url: "rtmp://a.rtmp.youtube.com/live2" },
        { key: "twitch", label: qsTr("Twitch"), url: "rtmp://live.twitch.tv/app" },
        { key: "facebook", label: qsTr("Facebook"), url: "rtmps://live-api-s.facebook.com:443/rtmp/" },
        { key: "rtmp", label: qsTr("Custom RTMP"), url: "" }
    ]
    readonly property var containers: ["MKV", "MP4"]
    readonly property var encoders: ["NVENC", "AMF", "QuickSync", "Software"]
    readonly property var frameRates: ["24 fps", "30 fps", "50 fps", "60 fps"]
    readonly property var resolutions: ["720p", "1080p", "4K"]

    function containerOk(name) {
        const list = RecordingService.availableContainers
        return list.length === 0 || list.indexOf(String(name).toUpperCase()) >= 0
    }
    function encoderOk(name) {
        const list = RecordingService.availableEncoders
        return list.length === 0 || list.indexOf(String(name)) >= 0
    }
    // The warning for a saved choice the engine cannot honour ("" = fine).
    function unavailableNote(kind) {
        const bad = []
        if (kind === "audio") {
            const format = root.cfgText("format")
            if (!root.containerOk(format)) bad.push(format)
        } else {
            const container = root.cfgText("container")
            const encoder = root.cfgText("encoder")
            if (kind === "recording" && !root.containerOk(container)) bad.push(container)
            if (!root.encoderOk(encoder)) bad.push(encoder)
        }
        if (bad.length === 0)
            return ""
        return qsTr("This build can't use %1 yet — a take with it is refused when it starts. Pick one of the available choices.")
               .arg(bad.join(" + "))
    }

    // How many outputs on Settings · Routing use the preset.
    function usage(presetId) {
        void root.modelsRev
        return PresetListModel.usageCount(presetId)
    }

    // ---- Right-click menu (Edit / Duplicate / Delete) --------------------
    // The same drop-down Styles uses, so a row keeps its whole width.
    property int contextRow: -1
    property string contextId: ""
    property string contextName: ""
    MenuCatcher {
        menu: presetContextMenu
    }
    DropdownPanel {
        id: presetContextMenu
        z: 50
        visible: false
        model: [
            { label: qsTr("Edit") },
            { label: qsTr("Duplicate") },
            { divider: true },
            { label: qsTr("Delete"), danger: true }
        ]
        onItemActivated: (label) => {
            presetContextMenu.visible = false
            if (root.contextRow < 0)
                return
            if (label === qsTr("Edit"))
                root.openEdit(root.contextRow)
            else if (label === qsTr("Duplicate"))
                PresetListModel.duplicatePreset(root.contextRow)
            else if (label === qsTr("Delete"))
                root.requestDelete(root.contextRow, root.contextId, root.contextName)
        }
    }

    // ---- Delete: straight away when nothing uses it, else after asking ----
    property int deleteRow: -1
    property string deleteName: ""
    property int deleteUsers: 0
    function requestDelete(row, presetId, name) {
        const users = PresetListModel.usageCount(presetId)
        if (users === 0) {
            PresetListModel.removePreset(row)
            return
        }
        root.deleteRow = row
        root.deleteName = name
        root.deleteUsers = users
        deleteConfirm.open()
    }
    ConfirmDialog {
        id: deleteConfirm
        title: qsTr("Delete “%1”?").arg(root.deleteName)
        message: root.deleteUsers === 1
                 ? qsTr("1 output on Routing uses this preset. It stays on the canvas with no preset, and will not record or stream until you pick another.")
                 : qsTr("%1 outputs on Routing use this preset. They stay on the canvas with no preset, and will not record or stream until you pick another.").arg(root.deleteUsers)
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            if (root.deleteRow >= 0)
                PresetListModel.removePreset(root.deleteRow)
            root.deleteRow = -1
            deleteConfirm.close()
        }
        onDismissed: {
            root.deleteRow = -1
            deleteConfirm.close()
        }
    }

    // ---- Add / Edit dialog -------------------------------------------------
    // Buffered copies, loaded when the dialog opens — the model is only written
    // on Save (every dialog's Cancel/Save contract). `dlgRow` < 0 = adding.
    property bool dlgShown: false
    property int dlgRow: -1
    readonly property bool dlgIsAdd: root.dlgRow < 0
    property string dlgKind: "recording"
    property string dlgName: ""
    property var dlgCfg: ({})

    function setCfg(key, value) {
        root.clearError(key)
        const next = Object.assign({}, root.dlgCfg)
        next[key] = value
        root.dlgCfg = next
    }
    function cfgText(key) {
        const v = root.dlgCfg[key]
        return v === undefined || v === null ? "" : String(v)
    }

    // Adding starts on Recording; the type chips can switch it before Add.
    function openAdd(kind) {
        root.dlgErrors = ({})
        root.dlgRow = -1
        root.dlgKind = kind === "streaming" ? "streaming" : kind === "audio" ? "audio" : "recording"
        root.dlgName = ""
        root.dlgCfg = PresetListModel.defaultConfig(root.dlgKind)
        root.dlgShown = true
    }
    function openEdit(row) {
        root.dlgErrors = ({})
        const p = PresetListModel.getPreset(row)
        if (p.id === undefined)
            return
        root.dlgRow = row
        root.dlgKind = p.kind
        root.dlgName = p.name
        root.dlgCfg = Object.assign({}, p.config)
        root.dlgShown = true
    }
    // Switching type while adding swaps in that type's own fields (a stream key
    // has no place on a recording). The name typed so far stays.
    function pickKind(kind) {
        if (!root.dlgIsAdd || root.dlgKind === kind)
            return
        root.dlgKind = kind
        root.dlgCfg = PresetListModel.defaultConfig(kind)
        root.dlgErrors = ({})
    }
    function pickPlatform(plat) {
        root.setCfg("platform", plat.key)
        // A named platform has a well-known ingest; Custom RTMP keeps whatever is typed.
        if (plat.url !== "")
            root.setCfg("serverUrl", plat.url)
    }
    property var dlgErrors: ({})
    function errorOf(key) { return root.dlgErrors[key] ?? "" }
    function clearError(key) {
        if (root.dlgErrors[key] === undefined)
            return
        const next = Object.assign({}, root.dlgErrors)
        delete next[key]
        root.dlgErrors = next
    }
    function saveDialog() {
        const cfg = Object.assign({}, root.dlgCfg)
        const errors = Object.assign({}, PresetListModel.fieldErrors(root.dlgKind, cfg))
        const nameWhy = PresetListModel.validateName(root.dlgName, root.dlgRow)
        if (nameWhy !== "")
            errors.name = nameWhy
        if (Object.keys(errors).length > 0) {
            root.dlgErrors = errors
            return
        }
        root.dlgErrors = ({})
        const ok = root.dlgIsAdd
            ? PresetListModel.addPreset(root.dlgKind, root.dlgName, cfg)
            : PresetListModel.updatePreset(root.dlgRow, root.dlgName, cfg)
        // A refusal has already toasted its reason (name taken, bad URL, ...) —
        // the dialog stays open so nothing typed is lost.
        if (ok)
            root.dlgShown = false
    }
    function pickFolder() {
        Qt.callLater(root.applyPickedFolder)
    }
    function applyPickedFolder() {
        const path = RecordingService.pickFolder(qsTr("Choose where recordings are saved"))
        if (path !== "")
            root.setCfg("outputFolder", path)
    }

    // ---- The list ------------------------------------------------------------
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.y + layout.height + Theme.space6   // the column's own top offset counts, or the last card sits on the window edge
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            // Below the pinned title / Add-preset header (same as Styles).
            y: Theme.space6 * 2 + addPresetBtn.height
            width: flick.width - Theme.space6
            spacing: 16

            Rectangle {
                width: parent.width
                height: presetsCol.height + 28
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: presetsCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 14

                    // Empty state — the roster starts empty, nothing is hardcoded.
                    Text {
                        // The Repeater's own count — a tracked property, unlike a
                        // rowCount() call, so adding the first preset hides this at once.
                        visible: presetRepeater.count === 0
                        width: parent.width
                        text: qsTr("No presets yet. Add a Recording preset (a video file on this machine), an Audio preset (WAV, M4A or MP3) or a Streaming preset (an RTMP target), then add it as an output in Settings · Routing.")
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        topPadding: 4
                    }

                    Repeater {
                        id: presetRepeater
                        model: PresetListModel

                        delegate: Rectangle {
                            id: presetRow
                            required property int index
                            required property string presetId
                            required property string name
                            required property string kind
                            required property string summary

                            readonly property bool streaming: presetRow.kind === "streaming"
                            readonly property bool audio: presetRow.kind === "audio"
                            // One look per kind: a recording is red, a stream is the accent, audio is the audio wire's purple.
                            readonly property color tint: presetRow.streaming ? Theme.accentLight : presetRow.audio ? Theme.wireAudio : Theme.dangerLight
                            readonly property color wash: presetRow.streaming ? Theme.accentSoft : presetRow.audio ? Theme.chip : Theme.armedLiveBg
                            readonly property color edge: presetRow.streaming ? Theme.accent : presetRow.audio ? Theme.wireAudio : Theme.liveBg

                            width: presetsCol.width
                            height: 64
                            radius: Theme.radiusMd
                            color: Theme.inset
                            // The same row as Settings · Styles: inset ground, hairline border, 40px tile.
                            border.width: 1
                            border.color: Theme.borderSubtle

                            // The kind's own glyph — the same pair Routing's Outputs
                            // column uses for a recording and a streaming card.
                            Rectangle {
                                id: tile
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 40
                                height: 40
                                radius: Theme.radiusMd
                                color: presetRow.wash
                                border.color: Theme.borderSubtle
                                border.width: 1

                                IconGlyph {
                                    anchors.centerIn: parent
                                    name: presetRow.streaming ? "activity" : presetRow.audio ? "volume2" : "fileText"
                                    color: presetRow.tint
                                    fit: true
                                    width: 18
                                    height: 18
                                }
                            }

                            Column {
                                anchors.left: tile.right
                                anchors.leftMargin: 12
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Row {
                                    spacing: 8

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: presetRow.name
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                        font.weight: Font.Medium
                                    }

                                    // The kind, as the same outlined tag Styles uses for its layout badge,
                                    // in the kind's own colour.
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: kindLabelText.implicitWidth + 12
                                        height: 18
                                        radius: 4
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Qt.alpha(presetRow.tint, 0.55)

                                        Text {
                                            id: kindLabelText
                                            anchors.centerIn: parent
                                            text: root.kindLabel(presetRow.kind)
                                            color: presetRow.tint
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }
                                }

                                Row {
                                    spacing: 8

                                    Text {
                                        text: presetRow.summary
                                        color: Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        text: {
                                            const u = root.usage(presetRow.presetId)
                                            return "·  " + (u === 0 ? qsTr("not on Routing yet")
                                                          : u === 1 ? qsTr("used by 1 output")
                                                          : qsTr("used by %1 outputs").arg(u))
                                        }
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                            }

                            // Double-click = Edit; right-click = the menu. Nothing
                            // sits on the row itself, so the summary keeps the width.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                cursorShape: Qt.ArrowCursor
                                onDoubleClicked: (mouse) => {
                                    if (mouse.button === Qt.LeftButton)
                                        root.openEdit(presetRow.index)
                                }
                                onClicked: (mouse) => {
                                    if (mouse.button !== Qt.RightButton)
                                        return
                                    root.contextRow = presetRow.index
                                    root.contextId = presetRow.presetId
                                    root.contextName = presetRow.name
                                    presetContextMenu.openAt(presetRow, mouse.x, mouse.y, root)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // The header's own ground — without it a scrolled card would show straight
    // through the pinned title / button (same as Styles).
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.space6 * 2 + addPresetBtn.height
        color: Theme.surface
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.verticalCenter: addPresetBtn.verticalCenter
        text: qsTr("Recording, Streaming & Audio")
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXl
        font.weight: Font.DemiBold
    }

    PrimaryAddButton {
        id: addPresetBtn
        objectName: "selfTestAddPreset"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.space6
        anchors.rightMargin: Theme.space6 + Theme.space2
        text: qsTr("Add preset")
        onClicked: root.openAdd("recording")
    }

    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }

    // ---- Add / Edit preset dialog ------------------------------------------
    ModalCard {
        id: presetDialog
        objectName: "selfTestPresetDialog"
        shown: root.dlgShown
        title: root.dlgIsAdd ? qsTr("Add preset") : qsTr("Edit preset")
        cardWidth: 600
        saveText: root.dlgIsAdd ? qsTr("Add preset") : qsTr("Save Changes")
        onCancelled: root.dlgShown = false
        onAccepted: root.saveDialog()

        SettingsField {
            width: parent.width
            label: qsTr("Preset name")
            placeholder: root.dlgKind === "streaming" ? qsTr("Name this preset — e.g. Sunday stream") : root.dlgKind === "audio" ? qsTr("Name this preset — e.g. Sermon audio") : qsTr("Name this preset — e.g. Sunday recording")
            // Why Add / Save was refused, right under the field (red border + text).
            error: root.errorOf("name")
            text: root.dlgName
            onTextEdited: (t) => { root.dlgName = t; root.clearError("name") }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Type")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Row {
                spacing: Theme.space2

                Repeater {
                    model: root.kinds
                    delegate: SelectableChip {
                        required property var modelData
                        label: modelData.label
                        selected: root.dlgKind === modelData.key
                        enabled: root.dlgIsAdd
                        opacity: root.dlgIsAdd || selected ? 1 : 0.4
                        onPicked: root.pickKind(modelData.key)
                    }
                }
            }
        }

        // ---- RECORDING (picture + sound) and AUDIO (sound only): how the take is written.
        // They share the output-folder block at the foot; each has its own fields above it.
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: root.dlgKind !== "streaming"

            // ---- AUDIO: which file format, and how compressed ----
            Column {
                width: parent.width
                spacing: Theme.space4
                visible: root.dlgKind === "audio"

                Column {
                    spacing: Theme.space2
                    Text {
                        text: qsTr("Format")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    Row {
                        spacing: Theme.space2
                        Repeater {
                            model: [
                                { key: "WAV", label: qsTr("WAV") },
                                { key: "M4A", label: qsTr("M4A (AAC)") },
                                { key: "MP3", label: qsTr("MP3") }
                            ]
                            delegate: SelectableChip {
                                required property var modelData
                                label: modelData.label
                                selected: root.cfgText("format") === modelData.key
                                // Greyed out when this machine has no writer for it (MP3 needs an encoder).
                                enabled: root.containerOk(modelData.key)
                                opacity: enabled ? 1 : 0.35
                                onPicked: root.setCfg("format", modelData.key)
                            }
                        }
                    }
                }

                Text {
                    width: parent.width
                    visible: root.cfgText("format") === "WAV"
                    text: qsTr("WAV is uncompressed (about 11 MB a minute at 48 kHz) — it has no bitrate.")
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                Text {
                    width: parent.width
                    visible: root.unavailableNote("audio") !== ""
                    text: root.unavailableNote("audio")
                    wrapMode: Text.WordWrap
                    color: Theme.warningLight
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                SettingsField {
                    width: (parent.width - 16) / 2
                    visible: root.cfgText("format") !== "WAV"
                    label: qsTr("Audio bitrate")
                    error: root.errorOf("audioBitrate")
                    suffix: "kbps"
                    text: root.cfgText("audioBitrate")
                    onTextEdited: (t) => root.setCfg("audioBitrate", t)
                }
            }

            Row {
                visible: root.dlgKind === "recording"
                spacing: Theme.space6

                Column {
                    spacing: Theme.space2
                    Text {
                        text: qsTr("Container")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    Row {
                        spacing: Theme.space2
                        Repeater {
                            model: root.containers
                            delegate: SelectableChip {
                                required property string modelData
                                label: modelData
                                // Greyed out when this build has no writer for it.
                                enabled: root.containerOk(modelData)
                                opacity: enabled ? 1 : 0.35
                                selected: root.cfgText("container") === modelData
                                onPicked: root.setCfg("container", modelData)
                            }
                        }
                    }
                }

                Column {
                    spacing: Theme.space2
                    Text {
                        text: qsTr("Encoder")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                    }
                    Row {
                        spacing: Theme.space2
                        Repeater {
                            model: root.encoders
                            delegate: SelectableChip {
                                required property string modelData
                                label: modelData
                                enabled: root.encoderOk(modelData)
                                opacity: enabled ? 1 : 0.35
                                selected: root.cfgText("encoder") === modelData
                                onPicked: root.setCfg("encoder", modelData)
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                visible: root.dlgKind === "recording" && root.unavailableNote("recording") !== ""
                text: root.unavailableNote("recording")
                wrapMode: Text.WordWrap
                color: Theme.warningLight
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Grid {
                visible: root.dlgKind === "recording"
                width: parent.width
                columns: 2
                columnSpacing: 16
                rowSpacing: 10

                SettingsField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Video bitrate")
                    error: root.errorOf("videoBitrate")
                    suffix: "kbps"
                    text: root.cfgText("videoBitrate")
                    onTextEdited: (t) => root.setCfg("videoBitrate", t)
                }
                SelectField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Frame rate")
                    value: root.cfgText("frameRate")
                    options: root.frameRates.map((f) => ({ label: f, value: f }))
                    onValuePicked: (v) => root.setCfg("frameRate", v)
                }
                SettingsField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Audio bitrate")
                    error: root.errorOf("audioBitrate")
                    suffix: "kbps"
                    text: root.cfgText("audioBitrate")
                    onTextEdited: (t) => root.setCfg("audioBitrate", t)
                }
                SelectField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Resolution")
                    value: root.cfgText("resolution")
                    options: root.resolutions.map((f) => ({ label: f, value: f }))
                    onValuePicked: (v) => root.setCfg("resolution", v)
                }
            }

            // Where the file lands: empty = the app's own recordings folder.
            Item {
                width: parent.width
                height: folderField.height

                SettingsField {
                    id: folderField
                    anchors.left: parent.left
                    anchors.right: browseBtn.left
                    anchors.rightMargin: 10
                    label: qsTr("Output folder")
                    placeholder: root.dlgKind === "audio" ? qsTr("Default — the app's recording/audio folder") : qsTr("Default — the app's recording/video folder")
                    text: root.cfgText("outputFolder")
                    onTextEdited: (t) => root.setCfg("outputFolder", t)
                }

                Rectangle {
                    id: browseBtn
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: browseLabel.implicitWidth + 28
                    height: 34
                    radius: Theme.radiusMd
                    color: browseArea.containsMouse ? Theme.chip : Theme.inset
                    border.width: 1
                    border.color: Theme.border
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        id: browseLabel
                        anchors.centerIn: parent
                        text: qsTr("Browse…")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }
                    MouseArea {
                        id: browseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.pickFolder()
                    }
                }
            }

            // Where "default" is, spelled out (the app's data folder + "recording").
            Text {
                width: parent.width
                visible: root.cfgText("outputFolder") === "" && RecordingService.defaultFolder !== ""
                text: root.dlgKind === "audio" ? RecordingService.defaultAudioFolder : RecordingService.defaultFolder
                elide: Text.ElideMiddle
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            LabeledMeter {
                width: 260
                label: (RecordingService.storage.freeGb ?? 0).toFixed(0) + " GB free · "
                       + (RecordingService.storage.capacityHours ?? 0).toFixed(1) + " h"
                pct: RecordingService.storage.freePercent ?? 100
                barColor: (RecordingService.storage.freePercent ?? 100) < 10
                            ? Theme.danger
                            : (RecordingService.storage.freePercent ?? 100) < 20
                              ? Theme.warningLight : Theme.success
            }
        }

        // ---- STREAMING: where the feed goes ----
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: root.dlgKind === "streaming"

            Column {
                width: parent.width
                spacing: Theme.space2
                Text {
                    text: qsTr("Platform")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                Row {
                    spacing: Theme.space2
                    Repeater {
                        model: root.platforms
                        delegate: SelectableChip {
                            required property var modelData
                            label: modelData.label
                            selected: root.cfgText("platform") === modelData.key
                            onPicked: root.pickPlatform(modelData)
                        }
                    }
                }
            }

            SettingsField {
                width: parent.width
                label: qsTr("Server URL")
                error: root.errorOf("serverUrl")
                placeholder: "rtmp://"
                text: root.cfgText("serverUrl")
                onTextEdited: (t) => {
                    root.setCfg("serverUrl", t)
                    // Typing a server of your own makes it a custom target.
                    if (root.cfgText("platform") !== "rtmp")
                        root.setCfg("platform", "rtmp")
                }
            }
            SettingsField {
                width: parent.width
                label: qsTr("Stream key")
                error: root.errorOf("streamKey")
                echoMode: TextInput.Password
                placeholder: "•••• •••• ••••"
                text: root.cfgText("streamKey")
                onTextEdited: (t) => root.setCfg("streamKey", t)
            }

            Column {
                width: parent.width
                spacing: Theme.space2
                Text {
                    text: qsTr("Encoder")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }
                Row {
                    spacing: Theme.space2
                    Repeater {
                        model: root.encoders
                        delegate: SelectableChip {
                            required property string modelData
                            label: modelData
                            enabled: root.encoderOk(modelData)
                            opacity: enabled ? 1 : 0.35
                            selected: root.cfgText("encoder") === modelData
                            onPicked: root.setCfg("encoder", modelData)
                        }
                    }
                }
            }

            Grid {
                width: parent.width
                columns: 2
                columnSpacing: 16
                rowSpacing: 10

                SettingsField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Video bitrate")
                    error: root.errorOf("videoBitrate")
                    suffix: "kbps"
                    text: root.cfgText("videoBitrate")
                    onTextEdited: (t) => root.setCfg("videoBitrate", t)
                }
                SelectField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Frame rate")
                    value: root.cfgText("frameRate")
                    options: root.frameRates.map((f) => ({ label: f, value: f }))
                    onValuePicked: (v) => root.setCfg("frameRate", v)
                }
                SettingsField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Audio bitrate")
                    error: root.errorOf("audioBitrate")
                    suffix: "kbps"
                    text: root.cfgText("audioBitrate")
                    onTextEdited: (t) => root.setCfg("audioBitrate", t)
                }
                SelectField {
                    width: (parent.width - 16) / 2
                    label: qsTr("Resolution")
                    value: root.cfgText("resolution")
                    options: root.resolutions.map((f) => ({ label: f, value: f }))
                    onValuePicked: (v) => root.setCfg("resolution", v)
                }
            }
        }
    }
}