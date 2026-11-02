import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // ---- Esc = cancel, Enter = the Save button (qml/settings/Hotkeys.qml) --
    property int hotkeyScope: 0
    function accept() { root.applied(root.withOpacity(root.selection)) }
    onOpenChanged: {
        if (root.open) {
            // Open on the palette that matches what is being edited, with the
            // custom field folded away — the compact starting state.
            root.paletteTab = root.selection.kind === "gradient" ? 1 : 0
            root.showCustomColor = false
        }
        if (root.open && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: qsTr("Background colour"),
                cancel: function () { root.cancelled() },
                accept: function () { root.accept() }
            })
        } else if (!root.open && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    property real opacityPct: 100

    // Header text — rename when reusing the picker for something other than
    // the slide background (e.g. an item's border color).
    property string title: qsTr("Background Color")

    readonly property string transparentValue: "transparent"

    property var colorSwatches: [
        root.transparentValue,
        "#ffffff", "#d4d4d4", "#9ca3af", "#ef4444", "#facc15",
        "#22c55e", "#00e676", "#4c8f6b", "#22d3ee", "#7dd3fc",
        "#2563eb", "#0b2a6b", "#a21caf", "#e11d8f", "#6b1033",
        "#000000"
    ]
    property var gradientSwatches: [
        { name: "Gradient #1", subtitle: "Purple → Blue", from: "#8b5cf6", to: "#3b82f6" },
        { name: "Gradient #2", subtitle: "Red → Orange",  from: "#e74c3c", to: "#f39c12" },
        { name: "Gradient #3", subtitle: "Green → Teal",  from: "#2ecc71", to: "#14b8a6" },
        { name: "Gradient #4", subtitle: "Navy → Black",  from: "#1a2a4a", to: "#15161d" },
        { name: "Gradient #5", subtitle: "Slate → Black", from: "#3a3f4a", to: "#15161d" }
    ]

    property int selectedColorIndex: -1
    property int selectedGradientIndex: 0
    property string customHex: "#1B2440"
    property string customGradFrom: "#3b82f6"
    property string customGradTo: "#8b5cf6"

    property int paletteTab: 0
    property bool showCustomColor: false

    property string liveKind: ""

    function seedWith(color) {
        root.selectedGradientIndex = 0
        root.selectedColorIndex = -1
        root.liveKind = ""
        const hex = String(color ?? "")
        // The slider follows the value being edited, whichever way the colour
        // arrived (a saved #89000000 tint, a typed alpha hex, a swatch).
        root.opacityPct = root.isValidHex(hex) ? root.alphaPctOf(hex) : 100
        if (root.isValidHex(hex)) {
            root.customHex = hex
            hexInput.text = hex   // (onTextChanged may set liveKind — cleared right after)
            root.liveKind = ""
            const at = root.colorSwatches.indexOf(hex)
            if (at < 0) {
                root.colorSwatches = root.colorSwatches.concat([hex])
                root.selectedColorIndex = root.colorSwatches.length - 1
            } else {
                root.selectedColorIndex = at
            }
        } else {
            root.selectedColorIndex = root.colorSwatches.indexOf(root.transparentValue)
        }
    }

    function openWith(color) {
        root.seedWith(color)
        root.open = true
    }

    function isValidHex(h) {
        return /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(h)   // 8 digits = #AARRGGBB
    }
    function alphaPctOf(color) {
        const hex = String(color ?? "").trim()
        if (hex.length === 9 && hex.charAt(0) === "#")
            return Math.round(parseInt(hex.substr(1, 2), 16) / 255 * 100)
        return 100
    }
    function normalizeHex(h) {
        let hex = h.trim()
        if (hex.length > 0 && hex[0] !== "#")
            hex = "#" + hex
        return hex
    }
    function commitCustomColor(rawHex) {
        const hex = root.normalizeHex(rawHex)
        if (!root.isValidHex(hex)) {
            hexInput.text = root.customHex
            root.liveKind = ""
            return
        }
        root.customHex = hex
        hexInput.text = hex
        root.opacityPct = root.alphaPctOf(hex)
        const existing = root.colorSwatches.indexOf(hex)
        if (existing >= 0) {
            root.selectedColorIndex = existing
        } else {
            root.colorSwatches = root.colorSwatches.concat([hex])
            root.selectedColorIndex = root.colorSwatches.length - 1
        }
        root.liveKind = ""
    }

    function commitCustomGradient(rawFrom, rawTo) {
        const from = root.normalizeHex(rawFrom)
        const to = root.normalizeHex(rawTo)
        if (!root.isValidHex(from) || !root.isValidHex(to)) {
            gradFromInput.text = root.customGradFrom
            gradToInput.text = root.customGradTo
            root.liveKind = ""
            return
        }
        root.customGradFrom = from
        root.customGradTo = to
        gradFromInput.text = from
        gradToInput.text = to

        let existing = -1
        for (let i = 0; i < root.gradientSwatches.length; ++i) {
            if (root.gradientSwatches[i].from === from && root.gradientSwatches[i].to === to) {
                existing = i
                break
            }
        }
        if (existing >= 0) {
            root.selectedGradientIndex = existing
        } else {
            root.gradientSwatches = root.gradientSwatches.concat([{
                name: qsTr("Custom Gradient"),
                subtitle: from + " → " + to,
                from: from,
                to: to
            }])
            root.selectedGradientIndex = root.gradientSwatches.length - 1
        }
        root.selectedColorIndex = -1
        root.liveKind = ""
    }

    readonly property bool opacityApplies: !(root.selection.kind === "color"
                                             && root.selection.color === root.transparentValue)

    readonly property var selection: {
        if (root.liveKind === "color")
            return { kind: "color", color: root.customHex }
        if (root.liveKind === "gradient")
            return { kind: "gradient", from: root.customGradFrom, to: root.customGradTo,
                      name: qsTr("Custom Gradient"), subtitle: root.customGradFrom + " → " + root.customGradTo }
        if (root.selectedColorIndex >= 0)
            return { kind: "color", color: root.colorSwatches[root.selectedColorIndex] }
        return { kind: "gradient",
                 from: root.gradientSwatches[root.selectedGradientIndex].from,
                 to: root.gradientSwatches[root.selectedGradientIndex].to,
                 name: root.gradientSwatches[root.selectedGradientIndex].name,
                 subtitle: root.gradientSwatches[root.selectedGradientIndex].subtitle }
    }

    readonly property int selectedSwatch: {
        if (root.liveKind !== "" || root.selection === null)
            return -1
        if (root.selection.kind === "color")
            return root.colorSwatches.indexOf(root.selection.color)
        for (let i = 0; i < root.gradientSwatches.length; ++i) {
            if (root.gradientSwatches[i].from === root.selection.from && root.gradientSwatches[i].to === root.selection.to)
                return i
        }
        return -1
    }

    function fadedColor(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, root.opacityPct / 100).toString()
    }
    function withOpacity(sel) {
        if (sel.kind === "color")
            return sel.color === root.transparentValue ? sel : Object.assign({}, sel, { color: root.fadedColor(sel.color) })
        return Object.assign({}, sel, { from: root.fadedColor(sel.from), to: root.fadedColor(sel.to) })
    }

    // Fires when "Apply" is clicked, carrying the current selection (with the Opacity slider baked into its colour).
    // The consumer decides what to do with it.
    signal applied(var selection)
    signal cancelled()

    component GradientSwatch: Rectangle {
        id: gswatch

        property color from: "#000000"
        property color to: "#ffffff"
        property bool selected: false

        signal picked()

        width: 68
        height: 40
        radius: 8
        border.width: gswatch.selected ? 2 : 1
        border.color: gswatch.selected ? Theme.accent : Theme.borderSubtle
        gradient: Gradient {
            GradientStop { position: 0; color: gswatch.from }
            GradientStop { position: 1; color: gswatch.to }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: gswatch.picked()
        }
    }

    anchors.fill: parent
    visible: root.open

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
        width: Math.min(460, root.width - 80)
        readonly property int chrome: 62 + footerRow.height + 34
        height: Math.max(248, Math.min(root.height - 80, chrome + content.height))
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        clip: true

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        // ---- pinned header ----
        Item {
            id: headerItem
            x: 24
            y: 20
            width: parent.width - 48
            height: closeBtn.height

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 21
                font.weight: Font.DemiBold
            }

            ModalCloseButton {
                id: closeBtn
                onClicked: root.cancelled()
            }
        }

        // ---- scrollable palette (the middle) ----
        Flickable {
            id: flick
            x: 24
            y: headerItem.y + headerItem.height + 14
            width: parent.width - 48
            height: Math.max(120, card.height - y - footerRow.height - 34)
            contentWidth: width
            contentHeight: content.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content
                width: flick.width
                spacing: 18

                Row {
                    width: parent.width
                    spacing: 22

                    Repeater {
                        model: [
                            { label: qsTr("Normal"), icon: "image", tab: 0 },
                            { label: qsTr("Gradient"), icon: "blend", tab: 1 }
                        ]
                        delegate: Item {
                            id: tabItem
                            required property var modelData
                            readonly property bool active: root.paletteTab === tabItem.modelData.tab

                            width: tabRow.implicitWidth
                            height: 34

                            Row {
                                id: tabRow
                                y: 6
                                spacing: 7

                                IconGlyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: tabItem.modelData.icon
                                    color: tabItem.active ? Theme.textPrimary : Theme.textMuted
                                    width: 14; height: 14
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: tabItem.modelData.label
                                    color: tabItem.active ? Theme.textPrimary : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: tabItem.active ? Font.DemiBold : Font.Medium
                                }
                            }

                            // The accent underline marks the open tab — the same
                            // language the app's own tab bars use.
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 2
                                radius: 1
                                color: tabItem.active ? Theme.accent : "transparent"
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.paletteTab = tabItem.modelData.tab
                            }
                        }
                    }
                }

            // NORMAL — the palette itself.
            Column {
                width: parent.width
                spacing: 8
                visible: root.paletteTab === 0

                Grid {
                    columns: 5
                    columnSpacing: 10
                    rowSpacing: 10

                    Repeater {
                        model: root.colorSwatches
                        delegate: Rectangle {
                            id: colorSwatch
                            required property int index
                            required property string modelData
                            readonly property bool isTransparent: modelData === root.transparentValue
                            // opaque swatches are just their colour; Transparent and a
                            // colour with its own alpha (a typed hex, an old tint) sit on a checkerboard
                            readonly property bool isSolid: !isTransparent && Qt.color(modelData).a >= 1
                            readonly property bool selected: root.selectedSwatch === index

                            width: 40
                            height: 40
                            radius: 8
                            clip: true
                            color: colorSwatch.isSolid
                                   ? (colorSwatchArea.containsMouse ? Qt.lighter(modelData, 1.15) : modelData)
                                   : (colorSwatchArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.width: colorSwatch.selected ? 2 : 1
                            border.color: colorSwatch.selected ? Theme.accent : Theme.borderSubtle
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                            Behavior on border.width { NumberAnimation { duration: 100 } }

                            // Mini checkerboard so "Transparent" reads as a
                            // deliberate choice, not an empty/broken swatch.
                            Grid {
                                visible: !colorSwatch.isSolid
                                anchors.fill: parent
                                columns: 4
                                rows: 4
                                Repeater {
                                    model: 16
                                    delegate: Rectangle {
                                        required property int index
                                        width: 10
                                        height: 10
                                        color: (Math.floor(index / 4) + (index % 4)) % 2 === 0 ? Theme.borderSubtle : Theme.surface
                                    }
                                }
                            }

                            // the tint itself, over the checkerboard
                            Rectangle {
                                visible: !colorSwatch.isSolid && !colorSwatch.isTransparent
                                anchors.fill: parent
                                color: modelData
                            }

                            MouseArea {
                                id: colorSwatchArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.selectedColorIndex = colorSwatch.index
                                    root.liveKind = ""
                                    if (!colorSwatch.isTransparent) {
                                        root.customHex = colorSwatch.modelData
                                        hexInput.text = colorSwatch.modelData
                                        root.opacityPct = root.alphaPctOf(colorSwatch.modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                objectName: "chooseCustomColor"
                width: parent.width
                height: 44
                radius: 8
                visible: root.paletteTab === 0
                color: chooseCustomArea.containsMouse ? Theme.controlBg : Theme.inset
                border.color: Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Choose custom")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: chooseCustomArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.showCustomColor = !root.showCustomColor
                        if (root.showCustomColor)
                            hexInput.forceActiveFocus()
                    }
                }
            }

            LabeledSlider {
                objectName: "selfTestPaletteOpacity"
                width: parent.width
                label: qsTr("Opacity")
                minValue: 0
                maxValue: 100
                suffix: "%"
                value: root.opacityPct
                onMoved: (v) => root.opacityPct = v
                enabled: root.opacityApplies
                opacity: root.opacityApplies ? 1 : 0.45
            }

            // GRADIENT tab — the preset gradients, then the custom from/to
            // builder that used to sit at the very bottom of the dialog.
            Column {
                width: parent.width
                spacing: 8
                visible: root.paletteTab === 1

                // Flow (not Row) so it wraps onto further lines instead of
                // overflowing the card as custom gradients get added below.
                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: root.gradientSwatches
                        delegate: GradientSwatch {
                            required property int index
                            required property var modelData

                            from: modelData.from
                            to: modelData.to
                            selected: root.selectedSwatch === index
                            onPicked: {
                                root.selectedGradientIndex = index
                                root.selectedColorIndex = -1
                                root.liveKind = ""
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8
                visible: root.paletteTab === 0 && root.showCustomColor

                Rectangle {
                    width: parent.width
                    height: 40
                    radius: 8
                    color: Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            radius: 5
                            color: root.customHex
                            border.color: Theme.borderSubtle
                            border.width: 1
                        }

                        TextInput {
                            id: hexInput
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 30
                            text: root.customHex
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            selectByMouse: true
                            onTextChanged: {
                                const hex = root.normalizeHex(text)
                                if (root.isValidHex(hex)) {
                                    root.customHex = hex
                                    root.liveKind = "color"
                                }
                            }
                            onEditingFinished: root.commitCustomColor(text)
                        }
                    }
                }
            }

            Column {
                width: parent.width
                spacing: 8
                visible: root.paletteTab === 1

                Rectangle {
                    width: parent.width
                    height: 40
                    radius: 8
                    color: Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            radius: 5
                            border.color: Theme.borderSubtle
                            border.width: 1
                            gradient: Gradient {
                                GradientStop { position: 0; color: root.customGradFrom }
                                GradientStop { position: 1; color: root.customGradTo }
                            }
                        }

                        TextInput {
                            id: gradFromInput
                            anchors.verticalCenter: parent.verticalCenter
                            width: 140
                            text: root.customGradFrom
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            selectByMouse: true
                            onTextChanged: {
                                const from = root.normalizeHex(text)
                                const to = root.normalizeHex(gradToInput.text)
                                if (root.isValidHex(from) && root.isValidHex(to)) {
                                    root.customGradFrom = from
                                    root.customGradTo = to
                                    root.liveKind = "gradient"
                                }
                            }
                            onEditingFinished: root.commitCustomGradient(text, gradToInput.text)
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "→"
                            color: Theme.textMuted
                            font.pixelSize: 14
                        }

                        TextInput {
                            id: gradToInput
                            anchors.verticalCenter: parent.verticalCenter
                            width: 140
                            text: root.customGradTo
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            selectByMouse: true
                            onTextChanged: {
                                const from = root.normalizeHex(gradFromInput.text)
                                const to = root.normalizeHex(text)
                                if (root.isValidHex(from) && root.isValidHex(to)) {
                                    root.customGradFrom = from
                                    root.customGradTo = to
                                    root.liveKind = "gradient"
                                }
                            }
                            onEditingFinished: root.commitCustomGradient(gradFromInput.text, text)
                        }
                    }
                }
            }
            }
        }

        AppScrollBar {
            anchors.top: flick.top
            anchors.bottom: flick.bottom
            anchors.right: parent.right
            anchors.rightMargin: 6
            flickable: flick
        }

        // ---- pinned footer: Cancel/Apply can never scroll out of view ----
        Row {
            id: footerRow
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 20
            spacing: 10

            Rectangle {
                width: 78
                height: 34
                radius: 8
                color: cancelArea.containsMouse ? Theme.hoverBg : Theme.inset
                border.color: Theme.borderSubtle
                border.width: 1

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
                width: 78
                height: 34
                radius: 8
                color: applyArea.containsMouse ? Qt.darker(Theme.accent, 1.15) : Theme.accent
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Apply")
                    color: "#ffffff"
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: applyArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.accept()
                }
            }
        }
    }
}