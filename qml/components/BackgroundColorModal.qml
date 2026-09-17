import QtQuick

// Reusable color/gradient palette + picker: solid swatches, gradient
// swatches, a custom hex field, a custom from/to gradient builder, a live
// preview, and Apply/Cancel. Matches
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Background_Color.qml's
// bg_modal_dim/bg_modal_card popup, which ships only as a flat PNG in the
// ground truth export (no element-level QML to copy) — rebuilt here as real
// QML, model-driven (colorSwatches/gradientSwatches), so it can be dropped
// in anywhere a color needs picking, not just the Edit screen's Background
// row.
//
// There is deliberately no Opacity slider: opacity was removed from the app
// (it either did nothing or just faded things confusingly), and a fill's
// "less visible" state is expressed by picking a dimmer color instead. If it
// ever comes back it belongs on the consumer's style object, not in this
// picker's payload.
//
// The whole picker state is a single `selection` value —
//   { kind: "color", color } | { kind: "gradient", from, to, name, subtitle }
// — whether it came from a swatch or was typed live into the CUSTOM fields.
// Consumers just read `selection` on applied() and decide what to do with it
// (this component owns no application state of its own beyond the picker UI).
//
// Literal colors throughout, not Theme.* — same AOT-compiler limitation as
// DropdownPanel.qml at this nesting depth (instantiated from EditScreen.qml,
// itself nested under Main.qml).
Item {
    id: root

    property bool open: false

    // Header text — rename when reusing the picker for something other than
    // the slide background (e.g. an item's border color).
    property string title: qsTr("Background Color")

    // The one place "no background" is spelled out — a real, deliberately-
    // chosen swatch (rendered as a checkerboard below), not just whatever a
    // freshly-created background happens to default to, so "no background"
    // is always one click away again after picking an actual color.
    // Consumers of this component (e.g. EditScreen.qml's slideBackground
    // default) read this property instead of hardcoding "transparent"
    // themselves, so there's exactly one definition to change.
    readonly property string transparentValue: "transparent"

    property var colorSwatches: [
        root.transparentValue, "#ffffff", "#000000", "#e74c3c", "#f39c12", "#f1c40f",
        "#2ecc71", "#14b8a6", "#3b82f6", "#8b5cf6", "#6b7280"
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

    // "" | "color" | "gradient" — while non-empty, the preview/Apply follow
    // whatever is currently typed in the CUSTOM fields directly instead of
    // a swatch index, so the preview updates as you type instead of only
    // after pressing Enter/clicking away. Clicking any swatch clears this
    // back to swatch-driven selection.
    property string liveKind: ""

    function isValidHex(h) {
        return /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(h)
    }
    function normalizeHex(h) {
        let hex = h.trim()
        if (hex.length > 0 && hex[0] !== "#")
            hex = "#" + hex
        return hex
    }
    // Typing a custom hex and leaving the field both selects it (so it
    // drives the preview/Apply) and, if it's new, adds it as a swatch so
    // it's there to reuse next time — same "create on commit" idea as the
    // custom gradient builder below.
    function commitCustomColor(rawHex) {
        const hex = root.normalizeHex(rawHex)
        if (!root.isValidHex(hex)) {
            hexInput.text = root.customHex
            root.liveKind = ""
            return
        }
        root.customHex = hex
        hexInput.text = hex
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

    // The one value Apply carries — swatch-driven or typed live, unified
    // into the same { kind, ... } shape either way.
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

    // Index into colorSwatches/gradientSwatches of whatever `selection`
    // currently is, or -1 — so swatch highlighting stays in lockstep with
    // the preview (including a custom hex that matches a swatch) instead of
    // drifting from it. Live typing selects nothing until committed.
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

    // Fires when "Apply" is clicked, carrying the current selection. The
    // consumer decides what to do with it.
    signal applied(var selection)
    signal cancelled()

    // One reusable gradient swatch: a rounded rect filled with a two-stop
    // vertical gradient, a selection ring, and hover feedback. Used for
    // both the preset gradients and the custom ones added by the builder —
    // one definition instead of a hand-rolled copy per site.
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
        border.color: gswatch.selected ? "#6c5ce7" : "#2a3140"
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

    // Dim scrim behind the card; clicking it cancels, same as an
    // outside-click on any other overlay in this app.
    Rectangle {
        anchors.fill: parent
        color: "#99000000"

        MouseArea {
            anchors.fill: parent
            onClicked: root.cancelled()
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 460
        height: content.height + 40
        radius: 14
        color: "#15161d"
        border.color: "#232530"
        border.width: 1

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 18

            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.title
                        color: "#eef0f6"
                        font.family: "Inter"
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
                        color: closeArea.containsMouse ? "#20222c" : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: "#8a94a6"
                            font.pixelSize: 12
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
                    text: qsTr("Pick a color or gradient for the slide background")
                    color: "#8a94a6"
                    font.family: "Inter"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }

            // Current-selection preview.
            Rectangle {
                width: parent.width
                height: 64
                radius: 10
                color: "#1a1c26"
                border.color: "#2a3140"
                border.width: 1

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    // Checkerboard backing + the swatch on top. The
                    // checkerboard stays useful for light colors with
                    // transparency of their own (#80ffffff and friends).
                    Item {
                        width: 36
                        height: 36
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            id: previewChecker
                            anchors.fill: parent
                            radius: 8
                            clip: true
                            color: "#1a1c26"

                            Grid {
                                columns: 6
                                rows: 6
                                Repeater {
                                    model: 36
                                    delegate: Rectangle {
                                        required property int index
                                        width: 6
                                        height: 6
                                        color: (Math.floor(index / 6) + (index % 6)) % 2 === 0 ? "#2a2c38" : "#15161d"
                                    }
                                }
                            }
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: 8
                            border.color: "#6c5ce7"
                            border.width: 1.5
                            color: root.selection.kind === "color" ? root.selection.color : "transparent"
                            gradient: root.selection.kind === "gradient" ? gradPreview : null

                            Gradient {
                                id: gradPreview
                                GradientStop { position: 0; color: root.selection.kind === "gradient" ? root.selection.from : root.selection.color }
                                GradientStop { position: 1; color: root.selection.kind === "gradient" ? root.selection.to : root.selection.color }
                            }
                        }
                    }

                    Column {
                        spacing: 2
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            text: {
                                if (root.selection.kind === "gradient")
                                    return root.selection.name
                                return root.selection.color === root.transparentValue ? qsTr("Transparent") : qsTr("Custom color")
                            }
                            color: "#eef0f6"
                            font.family: "Inter"
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Text {
                            text: root.selection.kind === "gradient" ? root.selection.subtitle : root.selection.color
                            color: "#5c6475"
                            font.family: "Inter"
                            font.pixelSize: 10
                        }
                    }
                }
            }

            // COLORS
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("COLORS")
                    color: "#5c6475"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }

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
                            readonly property bool selected: root.selectedSwatch === index

                            width: 40
                            height: 40
                            radius: 8
                            clip: true
                            // Hover feedback on the fill itself (same
                            // lighten-on-hover language as every other
                            // chip/button in this file family), in addition
                            // to the selection ring below.
                            color: colorSwatch.isTransparent
                                   ? (colorSwatchArea.containsMouse ? "#242633" : "#1a1c26")
                                   : (colorSwatchArea.containsMouse ? Qt.lighter(modelData, 1.15) : modelData)
                            border.width: colorSwatch.selected ? 2 : 1
                            border.color: colorSwatch.selected ? "#6c5ce7" : "#2a3140"
                            Behavior on color { ColorAnimation { duration: 100 } }
                            Behavior on border.width { NumberAnimation { duration: 100 } }

                            // Mini checkerboard so "Transparent" reads as a
                            // deliberate choice, not an empty/broken swatch.
                            Grid {
                                visible: colorSwatch.isTransparent
                                anchors.fill: parent
                                columns: 4
                                rows: 4
                                Repeater {
                                    model: 16
                                    delegate: Rectangle {
                                        required property int index
                                        width: 10
                                        height: 10
                                        color: (Math.floor(index / 4) + (index % 4)) % 2 === 0 ? "#2a2c38" : "#15161d"
                                    }
                                }
                            }

                            MouseArea {
                                id: colorSwatchArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.selectedColorIndex = colorSwatch.index
                                    root.liveKind = ""
                                }
                            }
                        }
                    }
                }
            }

            // GRADIENTS
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("GRADIENTS")
                    color: "#5c6475"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }

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

            // CUSTOM hex entry — committing (Enter/losing focus) both
            // selects this color and, if it's new, adds it to COLORS above.
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("CUSTOM COLOR")
                    color: "#5c6475"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }

                Rectangle {
                    width: parent.width
                    height: 40
                    radius: 8
                    color: "#1a1c26"
                    border.color: "#2a3140"
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
                            border.color: "#2a3140"
                            border.width: 1
                        }

                        TextInput {
                            id: hexInput
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 30
                            text: root.customHex
                            color: "#c9cedd"
                            font.family: "Inter"
                            font.pixelSize: 12
                            selectByMouse: true
                            // Live preview on every keystroke, not just once
                            // the field loses focus — the swatch/name above
                            // update as you type instead of only after
                            // pressing Enter or clicking away.
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

            // CUSTOM GRADIENT builder — two hex fields (from/to); committing
            // either one both selects this gradient and, if it's new, adds
            // it to GRADIENTS above. Same create-on-commit idea as the
            // custom color field.
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("CUSTOM GRADIENT")
                    color: "#5c6475"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }

                Rectangle {
                    width: parent.width
                    height: 40
                    radius: 8
                    color: "#1a1c26"
                    border.color: "#2a3140"
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
                            border.color: "#2a3140"
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
                            color: "#c9cedd"
                            font.family: "Inter"
                            font.pixelSize: 12
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
                            color: "#5c6475"
                            font.pixelSize: 12
                        }

                        TextInput {
                            id: gradToInput
                            anchors.verticalCenter: parent.verticalCenter
                            width: 140
                            text: root.customGradTo
                            color: "#c9cedd"
                            font.family: "Inter"
                            font.pixelSize: 12
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

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 78
                    height: 34
                    radius: 8
                    color: cancelArea.containsMouse ? "#20222c" : "#1a1c26"
                    border.color: "#2a3140"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#c9cedd"
                        font.family: "Inter"
                        font.pixelSize: 12
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
                    color: applyArea.containsMouse ? "#5a4cd6" : "#6c5ce7"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Apply")
                        color: "#ffffff"
                        font.family: "Inter"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: applyArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied(root.selection)
                    }
                }
            }
        }
    }
}
