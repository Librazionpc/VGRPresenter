import QtQuick
import VGRPresenterUI
import "."

Item {
    id: root

    // ---- Seeded state (consumer-bound) ----
    property string name: ""
    // "HDMI" | "NDI" | "SDI" | "REC" | "STREAM"
    property string type: "HDMI"
    property bool showType: true
    property string res: "1920 × 1080"
    property string refresh: "60 Hz"
    // Physical display the output sits on (QScreen::name, "" = windowed —
    // NDI/REC/STREAM outputs don't live on a display).
    property string placement: ""
    // FreeShow's boundsLocked: a locked output keeps its display; the map
    // refuses reassignment while on.
    property bool locked: false
    // "smpte" | "gradient" | "checker" | "solidred"
    property string pattern: "smpte"
    property string audioDevice: ""

    // ---- Change signals (consumer writes state on each) ----
    signal nameEdited(string text)
    signal typePicked(string key)
    signal resPicked(string value)
    signal refreshPicked(string value)
    signal placementPicked(string screenName, string res, string refresh)
    signal lockToggled(bool locked)
    signal patternPicked(string key)
    signal audioDevicePicked(string deviceId)

    readonly property var resOptions: ["1920 × 1080", "3840 × 2160", "1280 × 720", "1280 × 800"]
    readonly property var refreshOptions: ["60 Hz", "50 Hz", "30 Hz", "24 Hz"]

    // ---- Output endpoints ----
    readonly property var audioDeviceOptions: {
        const out = [{ label: qsTr("Same as program"), key: "" }]
        void root.deviceRev
        const list = EngineBridge.audioDevices
        for (let i = 0; i < list.length; ++i) {
            const d = list[i]
            if (d.isInput === true)
                continue
            const id = d.id === undefined ? "" : String(d.id)
            if (id === "")
                continue
            out.push({ label: d.label === undefined ? id : String(d.label), key: id })
        }
        return out
    }
    readonly property string audioDeviceLabel: {
        const opts = root.audioDeviceOptions
        for (let i = 0; i < opts.length; ++i)
            if (opts[i].key === root.audioDevice)
                return opts[i].label
        return root.audioDevice === "" ? qsTr("Same as program")
                                       : qsTr("%1 (not connected)").arg(root.audioDevice)
    }
    // Bumped by devicesChanged so the option list re-reads when a device is
    // plugged in or pulled while the dialog is open.
    property int deviceRev: 0
    Connections {
        target: EngineBridge
        function onDevicesChanged() { root.deviceRev++ }
    }

    // ---- Real displays ----
    property int displayRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.displayRev++ }
        function onRowsInserted() { root.displayRev++ }
        function onRowsRemoved() { root.displayRev++ }
        function onScreensChanged() { root.displayRev++ }
    }
    readonly property var displayList: { root.displayRev; return OutputListModel.displays() }

    // The one active output's display (setActive is exclusive) — the red
    // LIVE chip on a map tile means this display is on air.
    readonly property string activeScreenName: {
        root.displayRev
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const d = OutputListModel.getOutput(i)
            if (d.active)
                return d.screenName
        }
        return ""
    }

    property bool identifying: false
    Timer {
        interval: 3000
        running: root.identifying
        onTriggered: root.identifying = false
    }
    // Closing the dialog must never leave flash windows up.
    onVisibleChanged: if (!visible) root.identifying = false

    Instantiator {
        id: identifyWindows
        active: root.identifying
        model: root.displayList

        delegate: Window {
            required property var modelData
            required property int index

            visible: true
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            flags: Qt.SplashScreen | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
            color: "#000000"

            Column {
                anchors.centerIn: parent
                spacing: 16

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: index + 1
                    color: Theme.controlInk
                    font.pixelSize: 368
                    font.weight: Font.Bold
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: modelData.width + " × " + modelData.height
                    color: "#9aa0b5"
                    font.pixelSize: 35
                }
            }
        }
    }

    height: formCol.implicitHeight

    Column {
        id: formCol
        width: parent.width
        spacing: Theme.space4

        SettingsField {
            width: parent.width
            label: "Screen name"
            placeholder: "e.g. Stage Left Projector"
            text: root.name
            onTextEdited: (t) => root.nameEdited(t)
        }

        Column {
            width: parent.width
            spacing: Theme.space2
            visible: root.showType
            height: root.showType ? implicitHeight : 0

            Text {
                text: "Output type"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Row {
                spacing: Theme.space2

                Repeater {
                    model: ["HDMI", "NDI", "SDI", "REC", "STREAM"]

                    delegate: SelectableChip {
                        required property string modelData
                        label: modelData
                        selected: root.type === modelData
                        onPicked: root.typePicked(modelData)
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: Theme.space4

            Column {
                width: (parent.width - Theme.space4) / 2
                spacing: Theme.space2

                Text {
                    text: "Resolution"
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }

                Rectangle {
                    id: resBox
                    width: parent.width
                    height: 34
                    radius: Theme.radiusMd
                    color: Theme.inset
                    border.width: 1
                    border.color: Theme.border

                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.res
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }

                    IconGlyph {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        name: "chevronDown"
                        color: Theme.textMuted
                        width: 12; height: 12
                        }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: resMenu.openAt(resBox, 0, resBox.height + 4, root.Window.contentItem)
                    }
                }
            }

            Column {
                width: (parent.width - Theme.space4) / 2
                spacing: Theme.space2

                Text {
                    text: "Refresh rate"
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                }

                Rectangle {
                    id: refreshBox
                    width: parent.width
                    height: 34
                    radius: Theme.radiusMd
                    color: Theme.inset
                    border.width: 1
                    border.color: Theme.border

                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.refresh
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                    }

                    IconGlyph {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        name: "chevronDown"
                        color: Theme.textMuted
                        width: 12; height: 12
                        }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: refreshMenu.openAt(refreshBox, 0, refreshBox.height + 4, root.Window.contentItem)
                    }
                }
            }
        }

        // ---- Where this output's sound comes out ----------------------------
        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: qsTr("Plays audio on")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Rectangle {
                id: audioDeviceBox
                width: parent.width
                height: 34
                radius: Theme.radiusMd
                color: Theme.inset
                border.width: 1
                border.color: Theme.border

                Text {
                    x: 12
                    width: parent.width - 36
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.audioDeviceLabel
                    color: root.audioDevice === "" ? Theme.textMuted : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }

                IconGlyph {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevronDown"
                    color: Theme.textMuted
                    width: 12; height: 12
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: audioDeviceMenu.openAt(audioDeviceBox, 0,
                                                      audioDeviceBox.height + 4,
                                                      root.Window.contentItem)
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2
            visible: root.type === "HDMI" || root.type === "SDI"

            Text {
                text: "Screen placement"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Item {
                id: placementRow
                width: parent.width
                height: Math.max(mapArea.height, placeControls.height)

                // ---- Geometry mini-map: one tile per real display, laid
                // out at its actual x/y relative to the others — the same
                // arrangement as the desktop, not a fixed mock list. ----
                Item {
                    id: mapArea
                    anchors.left: parent.left
                    anchors.right: windowedTile.left
                    anchors.rightMargin: Theme.space3
                    height: maxH

                    readonly property real maxH: 120

                    // Union bounds of every display, so tiles position
                    // relative to each other exactly like the desktop.
                    readonly property var bounds: {
                        let minX = 0, minY = 0, maxX = 0, maxY = 0, first = true
                        const list = root.displayList
                        for (let i = 0; i < list.length; ++i) {
                            const d = list[i]
                            if (first || d.x < minX) minX = d.x
                            if (first || d.y < minY) minY = d.y
                            if (first || d.x + d.width > maxX) maxX = d.x + d.width
                            if (first || d.y + d.height > maxY) maxY = d.y + d.height
                            first = false
                        }
                        return { minX: minX, minY: minY, w: maxX - minX, h: maxY - minY }
                    }

                    readonly property var smallestDisplay: {
                        let w = 0, h = 0
                        for (let i = 0; i < root.displayList.length; ++i) {
                            const d = root.displayList[i]
                            if (w === 0 || d.width < w) w = d.width
                            if (h === 0 || d.height < h) h = d.height
                        }
                        return { w: w, h: h }
                    }
                    readonly property real fitScale:
                        (bounds.w <= 0 || bounds.h <= 0)
                            ? 0
                            : Math.min((width - 12) / bounds.w, maxH / bounds.h)
                    readonly property real minScale:
                        (smallestDisplay.w <= 0 || smallestDisplay.h <= 0)
                            ? 0
                            : Math.max(64 / smallestDisplay.w, 44 / smallestDisplay.h)
                    readonly property real scale: root.displayList.length === 0
                        ? 0 : Math.max(fitScale, minScale)

                    Flickable {
                        anchors.fill: parent
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.AutoFlickDirection
                        interactive: contentWidth > width || contentHeight > height
                        contentWidth: Math.max(width, mapItem.width + 12)
                        contentHeight: Math.max(height, mapItem.height + 12)

                        Text {
                            anchors.centerIn: parent
                            visible: root.displayList.length === 0
                            width: parent.width
                            text: "No external displays detected — outputs stay windowed."
                            horizontalAlignment: Text.AlignHCenter
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs
                            wrapMode: Text.WordWrap
                        }

                        Item {
                            id: mapItem
                            visible: root.displayList.length > 0
                            x: width + 12 > mapArea.width ? 6 : (mapArea.width - width) / 2
                            y: height + 12 > mapArea.height ? 6 : (mapArea.height - height) / 2
                            width: mapArea.bounds.w * mapArea.scale
                            height: mapArea.bounds.h * mapArea.scale

                        Repeater {
                            model: root.displayList

                            delegate: Rectangle {
                                id: displayTile
                                required property var modelData
                                required property int index

                                readonly property bool selected: root.placement === modelData.name
                                readonly property bool hostsLive:
                                    modelData.name !== "" && modelData.name === root.activeScreenName
                                readonly property string modeText:
                                    modelData.width + " × " + modelData.height

                                x: (modelData.x - mapArea.bounds.minX) * mapArea.scale
                                y: (modelData.y - mapArea.bounds.minY) * mapArea.scale
                                width: modelData.width * mapArea.scale
                                height: modelData.height * mapArea.scale
                                radius: Theme.radiusSm
                                color: displayTile.selected
                                       ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                                       : Theme.surface
                                border.width: displayTile.selected ? 2 : 1
                                border.color: displayTile.selected ? Theme.accent : Theme.border
                                clip: true

                                Column {
                                    x: 8
                                    y: 6
                                    width: parent.width - 16
                                    spacing: 2

                                    Text {
                                        width: parent.width
                                        text: (displayTile.index + 1) + " · Display " + (displayTile.index + 1)
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        width: parent.width
                                        text: displayTile.modeText
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                // Red LIVE chip — this display is carrying
                                // the on-air output right now.
                                Rectangle {
                                    visible: displayTile.hostsLive
                                    anchors.top: parent.top
                                    anchors.topMargin: 6
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    width: liveLabel.implicitWidth + 10
                                    height: 14
                                    radius: 3
                                    color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.22)

                                    Text {
                                        id: liveLabel
                                        anchors.centerIn: parent
                                        text: "LIVE"
                                        color: Theme.dangerLight
                                        font.pixelSize: 9
                                        font.weight: Font.Bold
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    // A locked output keeps its display —
                                    // the map refuses reassignment.
                                    enabled: !root.locked
                                    cursorShape: root.locked ? Qt.ForbiddenCursor : Qt.PointingHandCursor
                                    onClicked: root.placementPicked(
                                                   displayTile.modelData.name,
                                                   displayTile.modeText,
                                                   displayTile.modelData.refresh + " Hz")
                                }
                            }
                        }
                    }
                }
                }

                // ---- Windowed: the output renders from no display at all
                // (NDI/REC/STREAM style) — clears the assignment. ----
                Rectangle {
                    id: windowedTile
                    width: 84
                    height: 56
                    anchors.right: placeControls.left
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    radius: Theme.radiusSm
                    color: root.placement === ""
                           ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                           : Theme.surface
                    border.width: root.placement === "" ? 2 : 1
                    border.color: root.placement === "" ? Theme.accent : Theme.border

                    Column {
                        anchors.centerIn: parent
                        spacing: 2

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "+"
                            color: root.placement === "" ? Theme.accent : Theme.textMuted
                            font.pixelSize: Theme.textLg
                            font.weight: Font.Bold
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "No display"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.locked
                        cursorShape: root.locked ? Qt.ForbiddenCursor : Qt.PointingHandCursor
                        onClicked: root.placementPicked("", "", "")
                    }
                }

                // ---- Identify + Lock (FreeShow's Screens controls) ----
                Column {
                    id: placeControls
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space2

                    component ControlRow: Rectangle {
                        id: ctrl
                        property bool active: false
                        property alias hovered: ctrlArea.containsMouse
                        signal picked()

                        width: 152
                        height: 34
                        radius: Theme.radiusMd
                        color: ctrl.active ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                             : ctrl.hovered ? Qt.lighter(Theme.inset, 1.25)
                             : Theme.inset
                        border.width: 1
                        border.color: ctrl.active ? Theme.accent : Theme.borderSubtle
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        MouseArea {
                            id: ctrlArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: ctrl.picked()
                        }
                    }

                    ControlRow {
                        id: identifyRow
                        active: root.identifying
                        onPicked: root.identifying = !root.identifying

                        Row {
                            anchors.centerIn: parent
                            spacing: 7

                            // Target ring — a live-broadcast metaphor for
                            // "flash this display's number".
                            Item {
                                width: 12; height: 12
                                anchors.verticalCenter: parent.verticalCenter

                                Rectangle {
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: "transparent"
                                    border.width: 1.4
                                    border.color: identifyRow.active ? Theme.accent : Theme.textSecondary
                                }
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 4; height: 4
                                    radius: 2
                                    color: identifyRow.active ? Theme.accent : Theme.textSecondary
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: identifyRow.active ? qsTr("Stop identify") : qsTr("Identify screens")
                                color: identifyRow.active ? Theme.accentLight : Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Medium
                            }
                        }
                    }

                    ControlRow {
                        id: lockRow
                        active: root.locked
                        onPicked: root.lockToggled(!root.locked)

                        Row {
                            anchors.centerIn: parent
                            spacing: 7

                            // Padlock — a rounded body with a shackle ring
                            // peeking above it.
                            Item {
                                width: 11; height: 12
                                anchors.verticalCenter: parent.verticalCenter

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 11; height: 7
                                    radius: 2
                                    color: lockRow.active ? Theme.accent : Theme.textSecondary
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 6
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 7; height: 7
                                    radius: 3.5
                                    color: "transparent"
                                    border.width: 1.6
                                    border.color: lockRow.active ? Theme.accent : Theme.textSecondary
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Lock position")
                                color: lockRow.active ? Theme.accentLight : Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Medium
                            }
                        }
                    }
                }
            }
        }

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: "Test patterns"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            Row {
                spacing: Theme.space2

                Repeater {
                    model: [
                        { key: "smpte", label: "SMPTE bars" },
                        { key: "gradient", label: "Gradient" },
                        { key: "checker", label: "Checker" },
                        { key: "solidred", label: "Solid red" }
                    ]

                    delegate: TestPatternChip {
                        required property var modelData
                        pattern: modelData.key
                        label: modelData.label
                        selected: root.pattern === modelData.key
                        onPicked: root.patternPicked(modelData.key)
                    }
                }
            }
        }
    }

    // Option menus for the two select boxes — shared DropdownPanel, opened
    // under their fields and clamped to the window.
    DropdownPanel {
        id: resMenu
        visible: false
        model: root.resOptions.map((r) => { return { label: r } })
        onItemActivated: (label) => {
            root.resPicked(label)
            resMenu.visible = false
        }
    }
    MenuCatcher { menu: resMenu; closesOnWheel: false }

    DropdownPanel {
        id: refreshMenu
        visible: false
        model: root.refreshOptions.map((r) => { return { label: r } })
        onItemActivated: (label) => {
            root.refreshPicked(label)
            refreshMenu.visible = false
        }
    }
    MenuCatcher { menu: refreshMenu; closesOnWheel: false }

    DropdownPanel {
        id: audioDeviceMenu
        visible: false
        model: root.audioDeviceOptions.map((o) => {
            return { label: o.label, key: o.key, accent: o.key === root.audioDevice }
        })
        onItemActivated: (label, item) => {
            void label
            root.audioDevicePicked(item && item.key !== undefined ? String(item.key) : "")
            audioDeviceMenu.visible = false
        }
    }
    MenuCatcher { menu: audioDeviceMenu; closesOnWheel: false }
}