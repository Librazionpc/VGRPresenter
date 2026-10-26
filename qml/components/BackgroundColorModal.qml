import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// Reusable color/gradient palette + picker, laid out the way FreeShow's own
// colour popup is (its inputs/MaterialColorInput.svelte): a Normal / Gradient
// TAB pair above the swatches, the palette itself, the Opacity of the pick, and
// one "Choose custom" door to a colour outside the grid — one mode visible at a
// time, instead of both palettes and both custom builders stacked in a column.
// Selecting stays explicit here (Apply/Cancel), because this dialog edits
// persisted slide/item state that is undoable; FreeShow's inline popup commits
// on the swatch click because its values are already live. Matches
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Background_Color.qml's
// bg_modal_dim/bg_modal_card popup, which ships only as a flat PNG in the
// ground truth export (no element-level QML to copy) — rebuilt here as real
// QML, model-driven (colorSwatches/gradientSwatches), so it can be dropped
// in anywhere a color needs picking, not just the Edit screen's Background
// row.
//
// OPACITY: an Opacity slider (0-100%). The first version of it only faded the preview - the
// value was handed to consumers that ignored it, so it was removed. It is back and it WORKS: the alpha is
// written INTO the applied colour (#AARRGGBB), so consumers need no change and nothing else is carried in the
// payload. It applies to solid colours and to both stops of a gradient; Transparent stays transparent.
//
// THE SLIDER IS THE ITEM'S OWN OPACITY, not a multiplier over it. It SEEDS from the alpha of the colour being
// edited (a #89000000 tint opens at 54%) and WRITES that alpha back on Apply, so what the slider shows is
// always the value the item carries. It used to open at a hardcoded 100% and multiply while applying: the
// picker for a 54% tint read "100%", and nothing the user could do with the slider told them what the item
// actually held ("it doesn't remember the item opacity level, it always defaults back to 100").
//
// The palette is REAL COLOURS + Transparent, and nothing else: the six hardcoded TINTS (black / white at 25%,
// 50% and 75% as #AARRGGBB) that used to sit at the end were removed at the user's call - a translucent swatch
// can only be drawn as a checkerboard with a faint tint over it, which read as a broken/missing swatch ("the
// hardcoded error"). Transparency is the Opacity slider's job now (pick the real colour, then set how opaque it
// is), or the CUSTOM field for anyone who wants to type an alpha hex directly (#80ffffff).
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

    // ---- Esc = cancel, Enter = the Save button (qml/settings/Hotkeys.qml) --
    // The picker's own pair, pushed as a Hotkeys scope while it is up; the
    // footer's Save and this share one `accept()`, so the key and the button
    // cannot drift.
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

    // 0-100 — the alpha the item is being edited AT. Seeded from the colour it
    // is opened on (seedWith), written back into the applied colour
    // (withOpacity). No reset-on-open: that reset is what made the picker drop
    // a translucent tint back to 100% every time it was reopened.
    property real opacityPct: 100

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

    // Real colours only (+ Transparent, the deliberate "no background" pick): a
    // translucent preset can only render as checkerboard + faint tint, which reads
    // as a broken swatch. Opacity comes from the slider (or a typed alpha hex in
    // CUSTOM), never from a hardcoded #AARRGGBB entry here.
    // FreeShow's own default palette (its helpers/color.ts `defaultColors`),
    // minus nothing, plus Transparent first: that is the grid an operator
    // already knows from FreeShow, and five columns is exactly its row length.
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

    // Which palette tab is open — FreeShow's `normal` / `gradient` pair (see its
    // inputs/MaterialColorInput.svelte Tabs). Seeded from the value being
    // edited when the dialog opens, so a gradient reopens on its own swatches.
    property int paletteTab: 0
    // "Choose custom" reveals the hex entry. FreeShow hands off to its own
    // picker popup there; this dialog has no colour wheel, so the field IS the
    // custom door and stays hidden until it is asked for.
    property bool showCustomColor: false

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
    // The opacity percentage a colour value carries, for seeding the slider:
    // 100 for an opaque #RRGGBB (and for anything unparseable — "transparent"
    // is a swatch, not an alpha), and the leading byte's share of 255 for the
    // #AARRGGBB form this picker itself writes on Apply (see fadedColor).
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
        // A typed alpha hex IS an opacity choice: hand it to the slider, so the
        // two controls never show two different values for the one colour
        // (typing #80ffffff moves the slider to 50%, and Apply then writes back
        // what the slider says).
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

    // The one value Apply carries — swatch-driven or typed live, unified
    // into the same { kind, ... } shape either way.
    // Does the Opacity row have anything to act on?
    //
    // NO for the Transparent swatch, and only for it: withOpacity() below
    // deliberately hands Transparent back UNCHANGED (`sel.color ===
    // transparentValue ? sel : …` — "no background" is not a colour with an
    // alpha of 0%), and customSelection() applies the slider to a gradient's
    // stops, never to this swatch. So on Transparent the slider could be
    // dragged to any percentage and the applied value never moved — a control
    // that reads as live but does nothing, which is exactly the confusion the
    // row is disabled to prevent. A colour the user typed with its own alpha
    // (`#80000000`) is NOT this case: the slider legitimately re-writes that
    // alpha, so it stays live.
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

    // `color` with its alpha REPLACED by the Opacity slider, as "#AARRGGBB" /
    // "#RRGGBB". Replaced, not scaled: the slider holds the item's own opacity,
    // so applying it twice (open → Apply without touching anything) must land on
    // the same value rather than compounding 54% into 29%.
    function fadedColor(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, root.opacityPct / 100).toString()
    }
    // A selection with the Opacity slider applied. Transparent stays transparent
    // (it is a swatch, not a colour). At 100% an opaque colour comes back out
    // unchanged; a translucent one comes back fully opaque, which is exactly
    // what the slider then reads.
    function withOpacity(sel) {
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

    // Dim scrim behind the card. Outside clicks are trapped without cancelling;
    // the shared scrim also prevents wheel events reaching the canvas behind.
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

    // The card: pinned header (title + close X), scrollable palette middle,
    // pinned Cancel/Apply footer — the same contract ModalCard gives every
    // other dialog. The palette body is taller than a settings window; the
    // old content-sized card centered itself with NO cap and clipped BOTH
    // ends (close X off the top, Apply/Cancel off the bottom, nothing
    // scrollable). Height now caps against the parent; the middle scrolls.
    Rectangle {
        id: card
        // The app's one scale, on the card (see AppScale.qml) — never on the root,
        // whose scrim IS the dialog's black box and has to stay window-sized.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
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
                font.family: Theme.fontFamily
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
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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

                // FreeShow's palette tabs (its MaterialColorInput: a `normal` /
                // `gradient` Tabs pair directly above the swatches). Same two
                // modes, one panel each — instead of stacking BOTH palettes plus
                // both custom builders in one long column, which is what made
                // this dialog read as three screens' worth of colour controls.
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
                            // Hover feedback on the fill itself (same
                            // lighten-on-hover language as every other
                            // chip/button in this file family), in addition
                            // to the selection ring below.
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
                                    // Sync the hex field + Opacity slider to THIS
                                    // swatch (user call: "i selected black why do
                                    // now have hex color" — clicking a preset left
                                    // the hex field and the slider showing
                                    // whatever the PREVIOUS pick/item left behind,
                                    // not black's own values). Same reseed a typed
                                    // hex already gets via commitCustomColor —
                                    // every way of picking a colour must agree on
                                    // what the two readouts show for it.
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

            // CHOOSE CUSTOM — FreeShow's one button under its palette, opening
            // the custom picker. Ours reveals the hex field below (there is no
            // native colour wheel to hand off to), already focused so typing a
            // hex works on the first keystroke.
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

            // OPACITY - the alpha of whatever is picked (colour or gradient); 100% = as chosen.
            // GREYED OUT on the Transparent swatch: see opacityApplies. The row
            // keeps its place in the layout (nothing moves when the pick
            // changes) and dims, so it reads as "not available for this pick"
            // rather than as a control that ignores the pointer.
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

            // CUSTOM hex entry — revealed by "Choose custom" above; committing
            // (Enter/losing focus) both selects this colour and, if it's new,
            // adds it to the palette.
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
            // either one both selects this gradient and, if it's new, adds it
            // to the presets above. Same create-on-commit idea as the custom
            // colour field.
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
