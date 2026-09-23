import QtQuick
import VGRPresenterUI
import "."

// The Add/Edit screen form (VGRPresenter · Settings · Outputs · Add/Edit):
// Screen name, Output type chips, Resolution + Refresh rate selects, Screen
// placement (a geometry mini-map of the REAL displays — FreeShow's Screens
// model: tiles sit at each display's actual x/y, resolution derives from the
// assigned display, Identify flashes the number on every physical screen,
// Lock freezes the assignment), Test patterns. One implementation so the Add
// and Edit dialogs can't drift — Edit appends its style/content sections
// below this form in the dialog.
//
// Contract: consumer binds the plain properties (seeding current values) and
// handles the *Edited/*Picked signals by writing its own state back. The form
// never assigns its own properties, so the consumer's bindings stay intact.
Item {
    id: root

    // ---- Seeded state (consumer-bound) ----
    property string name: ""
    // "HDMI" | "NDI" | "SDI" | "REC" | "STREAM"
    property string type: "HDMI"
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

    // ---- Change signals (consumer writes state on each) ----
    signal nameEdited(string text)
    signal typePicked(string key)
    signal resPicked(string value)
    signal refreshPicked(string value)
    // Picking a display carries its current mode so the consumer can snap
    // res/refresh to it (resolution derives from the display, like
    // FreeShow's outputLabel). Picking "windowed" carries "" for both —
    // the consumer keeps whatever resolution is already set.
    signal placementPicked(string screenName, string res, string refresh)
    signal lockToggled(bool locked)
    signal patternPicked(string key)

    readonly property var resOptions: ["1920 × 1080", "3840 × 2160", "1280 × 720", "1280 × 800"]
    readonly property var refreshOptions: ["60 Hz", "50 Hz", "30 Hz", "24 Hz"]

    // ---- Real displays ----
    // OutputListModel enumerates QGuiApplication::screens(). Q_INVOKABLE
    // reads aren't binding-tracked, so this revision counter is what makes
    // the map and its LIVE marker follow model changes while open.
    property int displayRev: 0
    Connections {
        target: OutputListModel
        function onDataChanged() { root.displayRev++ }
        function onRowsInserted() { root.displayRev++ }
        function onRowsRemoved() { root.displayRev++ }
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

    // Identify (FreeShow's): flash each display's number on the physical
    // screen for a few seconds. One frameless topmost window per display,
    // sized exactly to its geometry — real windows, but only CREATED while
    // identifying (an Instantiator with active:false builds nothing, and
    // fresh Window objects each time avoids re-showing stale ones).
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
                    color: "#ffffff"
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

        Column {
            width: parent.width
            spacing: Theme.space2

            Text {
                text: "Screen placement"
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
            }

            // A plain Item, not a Row — mapArea's width used to be computed
            // as `parent.width - placeControls.width - spacing`, which
            // looks equivalent to anchoring but isn't quite: any transient
            // moment where placeControls hasn't settled its own width yet
            // (or simply not enough room at a narrower card width) let
            // placeControls get pushed outside the row's own bounds, where
            // the card's clip:true silently cut it off — exactly the
            // "Lock position" chip clipping that was reported. Anchoring
            // mapArea's right edge directly to placeControls' actual left
            // edge can't drift out of sync the same way.
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
                    height: Math.max(96, mapItem.height)

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

                    readonly property real scale:
                        (root.displayList.length === 0 || bounds.w <= 0 || bounds.h <= 0)
                            ? 0
                            : Math.min((width - 12) / bounds.w, maxH / bounds.h)

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
                        x: (mapArea.width - width) / 2
                        y: (mapArea.height - height) / 2
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
                                width: Math.max(64, modelData.width * mapArea.scale)
                                height: Math.max(44, modelData.height * mapArea.scale)
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
                // A matched pair of icon+label rows (fixed width, consistent
                // height, a border that lights up on the active state) —
                // previously a plain ghost AppButton stacked over a generic
                // SelectableChip, which read as an unstyled afterthought
                // next to the mini-map. Icons are hand-drawn from
                // primitives (a target ring for Identify, a padlock body +
                // shackle for Lock), matching NavIcon.qml's own icon
                // language rather than reaching for emoji glyphs.
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
                        Behavior on color { ColorAnimation { duration: 100 } }
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

    DropdownPanel {
        id: refreshMenu
        visible: false
        model: root.refreshOptions.map((r) => { return { label: r } })
        onItemActivated: (label) => {
            root.refreshPicked(label)
            refreshMenu.visible = false
        }
    }
}
