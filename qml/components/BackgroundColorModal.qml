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
// OPACITY: an Opacity slider (0-100%, default 100). The first version of it only faded the preview - the
// value was handed to consumers that ignored it, so it was removed. It is back and it WORKS: the alpha is
// written INTO the applied colour (#AARRGGBB), so consumers need no change and nothing else is carried in the
// payload. It applies to solid colours and to both stops of a gradient; Transparent stays transparent.
// Besides that the palette offers Transparent (nothing is drawn - the checkerboard shows it, and it renders out
// transparent) and six TINTS: black / white at 25%, 50% and 75% (#AARRGGBB), also typeable in the custom field.
//
// The whole picker state is a single `selection` value —
//   { kind: "color", color } | { kind: "gradient", from, to, name, subtitle }
// — whether it came from a swatch or was typed live into the CUSTOM fields.
// Consumers just read `selection` on applied() and decide what to do with it
// (this component owns no application state of its own beyond the picker UI).
//
// Colors read Theme tokens, so a Light choice recolours the modal with the
// rest of the app. (The palette swatches themselves — the preset colours and
// gradients — are CONTENT, not chrome, and keep their literal values.)
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    // 0-100. Written into the applied colour's alpha (see withOpacity); reset to fully opaque whenever the picker opens.
    property real opacityPct: 100
    onOpenChanged: if (open) root.opacityPct = 100

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
        "#2ecc71", "#14b8a6", "#3b82f6", "#8b5cf6", "#6b7280",
        // tints: translucent black and white (#AARRGGBB) for dimming bars and lower thirds
        "#40000000", "#80000000", "#bf000000", "#40ffffff", "#80ffffff", "#bfffffff"
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

    // Seed the palette from the CURRENT value — the picker shows the color
    // being edited, not this component's hardcoded defaults (the old open
    // path always displayed #1B2440 etc. until the user picked). A hex
    // selects/highlights its swatch (adding it first when custom, same
    // create-on-commit rule as typing); "transparent" (or anything that
    // isn't a color) selects the Transparent swatch. Pure: does NOT touch
    // `open`, so a caller that binds `open` to its own state (EditScreen's
    // shared picker) can call this when opening without breaking the binding.
    function seedWith(color) {
        root.selectedGradientIndex = 0
        root.selectedColorIndex = -1
        root.liveKind = ""
        const hex = String(color ?? "")
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

    // For callers that hand the picker its visibility (no `open` binding):
    // seed from the current value and show. Bound callers: seedWith() + set
    // their own open flag.
    function openWith(color) {
        root.seedWith(color)
        root.open = true
    }

    function isValidHex(h) {
        return /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(h)   // 8 digits = #AARRGGBB
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

    // `color` (a colour string or value) with its alpha scaled by the Opacity slider, as "#AARRGGBB" / "#RRGGBB".
    function fadedColor(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * root.opacityPct / 100).toString()
    }
    // A selection with the Opacity slider applied. Transparent stays transparent; at 100% nothing changes.
    function withOpacity(sel) {
        if (root.opacityPct >= 100)
            return sel
        if (sel.kind === "color")
            return sel.color === root.transparentValue ? sel : Object.assign({}, sel, { color: root.fadedColor(sel.color) })
        return Object.assign({}, sel, { from: root.fadedColor(sel.from), to: root.fadedColor(sel.to) })
    }

    // Fires when "Apply" is clicked, carrying the current selection (with the Opacity slider baked into its colour).
    // The consumer decides what to do with it.
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

    // Dim scrim behind the card; clicking it cancels, same as an
    // outside-click on any other overlay in this app. Shared ModalScrim —
    // also consumes wheel so the canvas behind can't scroll through it.
    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    // The card: pinned header (title + close X), scrollable palette middle,
    // pinned Cancel/Apply footer — the same contract ModalCard gives every
    // other dialog. The palette body is taller than a settings window; the
    // old content-sized card centered itself with NO cap and clipped BOTH
    // ends (close X off the top, Apply/Cancel off the bottom, nothing
    // scrollable). Height now caps against the parent; the middle scrolls.
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(460, root.width - 80)
        // Height is decided HERE, in one place: the content plus the chrome,
        // capped to the window. 62 = header (20 top margin + 28 close row + 14
        // gap); footerRow.height + 34 = footer (34 buttons + 20 bottom margin
        // + 14 of air between the scroll area and the buttons). The flick's
        // height below derives from THIS, not the other way round — the old
        // paired formulas met at exactly the footer's top edge, so scrolled to
        // the bottom the last row's border sat under the Cancel/Apply buttons.
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
                font.family: "Segoe UI"
                font.pixelSize: 21
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

        // ---- scrollable palette (the middle) ----
        Flickable {
            id: flick
            x: 24
            y: headerItem.y + headerItem.height + 14
            width: parent.width - 48
            // From the card's height (already capped): everything below the
            // flick minus the 20px footer margin AND a 14px cushion, so at
            // full scroll-down the last row never touches the buttons. The
            // 120 floor keeps a degenerate-short window from collapsing the
            // palette to nothing (the card may then clip — nothing to fix
            // there, there is simply no room).
            height: Math.max(120, card.height - y - footerRow.height - 34)
            contentWidth: width
            contentHeight: content.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content
                width: flick.width
                spacing: 18

                Text {
                    width: parent.width
                    text: qsTr("Pick a color or gradient for the slide background")
                    color: Theme.textSecondary
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

            // Current-selection preview.
            Rectangle {
                width: parent.width
                height: 64
                radius: 10
                color: Theme.inset
                border.color: Theme.borderSubtle
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
                            color: Theme.inset

                            Grid {
                                columns: 6
                                rows: 6
                                Repeater {
                                    model: 36
                                    delegate: Rectangle {
                                        required property int index
                                        width: 6
                                        height: 6
                                        color: (Math.floor(index / 6) + (index % 6)) % 2 === 0 ? Theme.borderSubtle : Theme.surface
                                    }
                                }
                            }
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: 8
                            border.color: Theme.accent
                            border.width: 1.5
                            color: root.selection.kind === "color" && root.selection.color !== root.transparentValue
                                   ? root.fadedColor(root.selection.color) : "transparent"
                            gradient: root.selection.kind === "gradient" ? gradPreview : null

                            Gradient {
                                id: gradPreview
                                GradientStop { position: 0; color: root.fadedColor(root.selection.kind === "gradient" ? root.selection.from : root.selection.color) }
                                GradientStop { position: 1; color: root.fadedColor(root.selection.kind === "gradient" ? root.selection.to : root.selection.color) }
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
                                if (root.selection.color === root.transparentValue)
                                    return qsTr("Transparent")
                                return Qt.color(root.selection.color).a < 1 ? qsTr("Tint") : qsTr("Custom color")
                            }
                            color: Theme.textPrimary
                            font.family: "Segoe UI"
                            font.pixelSize: 15
                            font.weight: Font.Medium
                        }
                        Text {
                            text: root.selection.kind === "gradient" ? root.selection.subtitle : root.selection.color
                            color: Theme.textMuted
                            font.family: "Segoe UI"
                            font.pixelSize: 12
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
                    color: Theme.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
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
                            // opaque swatches are just their colour; Transparent and the tints sit on a checkerboard
                            readonly property bool isSolid: !isTransparent && Qt.color(modelData).a >= 1
                            readonly property bool selected: root.selectedSwatch === index

                            width: 40
                            height: 40
                            radius: 8
                            clip: true
                            // Hover feedback on the fill itself (same
                            // lighten-on-hover language as every other
                            // chip/button in this file family), in addition
                            // to the selection ring below.
                            color: colorSwatch.isSolid
                                   ? (colorSwatchArea.containsMouse ? Qt.lighter(modelData, 1.15) : modelData)
                                   : (colorSwatchArea.containsMouse ? Theme.hoverBg : Theme.inset)
                            border.width: colorSwatch.selected ? 2 : 1
                            border.color: colorSwatch.selected ? Theme.accent : Theme.borderSubtle
                            Behavior on color { ColorAnimation { duration: 100 } }
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
                                }
                            }
                        }
                    }
                }
            }

            // OPACITY - the alpha of whatever is picked (colour or gradient); 100% = as chosen.
            LabeledSlider {
                objectName: "selfTestPaletteOpacity"
                width: parent.width
                label: qsTr("Opacity")
                minValue: 0
                maxValue: 100
                suffix: "%"
                value: root.opacityPct
                onMoved: (v) => root.opacityPct = v
            }

            // GRADIENTS
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("GRADIENTS")
                    color: Theme.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
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
                    color: Theme.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    font.weight: Font.Bold
                }

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
                            font.family: "Segoe UI"
                            font.pixelSize: 14
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
                    color: Theme.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    font.weight: Font.Bold
                }

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
                            font.family: "Segoe UI"
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
                            font.family: "Segoe UI"
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
                    font.family: "Segoe UI"
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
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Apply")
                    color: "#ffffff"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: applyArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.applied(root.withOpacity(root.selection))
                }
            }
        }
    }
}
