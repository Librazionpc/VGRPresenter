import QtQuick
import VGRPresenterUI

// The right panel's "TEXT" tab content — shown when a "text" kind canvas item
// is selected (see EditScreen.qml's rightPanelTab). Edits the item's typography
// (bold/italic/underline/strikethrough, alignment, size, line height, letter
// spacing, auto-size mode) plus its raw geometry, straight off the item's own
// `meta` bag (see CanvasItem.qml) and x/y/width/height — the same
// live-object-binding convention SizeStyleCard.qml uses for style, so a change
// here shows up on the canvas immediately, no mirroring layer.
//
// LAID OUT AS FREESHOW'S EDIT PANEL. The whole tab is the shared
// InspectorSection card (see its header): one headerless block carrying the
// controls an operator touches constantly (font family/weight/colour, the
// B/I/U/S formatting row, Size), then collapsible sections — Align, Text,
// Lines, List, Position & Size — each with the tab's one section hue, a chevron,
// and a Reset chip that appears only when that section's values differ from
// their defaults (and force-opens the section when they start to). That
// replaced this panel's previous mixture of three hand-rolled collapsible
// headers and four flat ALL-CAPS labels, which meant two different collapse
// implementations and no way to reset one group.
//
// `meta` fields this owns: bold, italic, underline, strikethrough,
// align ("left"|"center"|"right"|"justify"), verticalAlign ("top"|"center"|
// "bottom"), fontFamily, fontWeight (display string only, no picker UI yet),
// autoSize ("none"|"shrinkToFit"|"growToFit" — FreeShow's textFit vocabulary;
// legacy "shrink"/"grow" values are mapped on the canvas side), fontSize,
// lineHeight, lineSpacing (the px gap between line boxes), lineBg (the per-LINE
// background band; "" = none), letterSpacing, wordSpacing, oneLine, textCase, list. All
// optional — an item created
// before this panel existed just reads as the same defaults
// EditableCanvasLabel already rendered (16px, Medium weight, centered).
//
// Colors read Theme tokens, so a Light choice recolours this panel with the
// rest of the app.
Column {
    id: root

    // The CanvasItem being edited (must be kind "text"), or null.
    property var target: null

    // A function the consumer points at its own undo/history snapshot
    // function (e.g. EditScreen.qml's root.pushUndoSnapshot) — called once
    // per discrete edit (a toggle click, finishing a geometry field, the
    // start of a slider drag), never per intermediate value. Not wired
    // directly to a specific undo system so this panel (and the same
    // pattern on any other control) stays reusable outside EditScreen.qml
    // too; null just means "no undo support here yet."
    property var undoHook: null
    // Called BEFORE undoHook — hands keyboard focus back to the canvas
    // first, same as clicking to select an item already does (see
    // EditScreen.qml's handleCanvasSelect) and the same as a resize-handle
    // press already does (DraggableCanvasText's onPressed). Without this, a
    // slider drag that starts while THIS panel's own text item is still
    // mid-edit leaves focus on its TextEdit — Ctrl+Z/Ctrl+Y are gated on
    // mCanvas.activeFocus specifically (so they can't steal focus away from
    // an in-progress typing session), and that gate would then stay closed
    // for the rest of the session, not just this one drag. Same reusability
    // reasoning as undoHook above: a consumer wires this to
    // mCanvas.forceActiveFocus; null means nothing to hand off to.
    property var focusHook: null
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    signal changeColorRequested()
    signal changeFontRequested()        // opens the WEIGHT list (Regular/Medium/SemiBold/Bold)
    signal changeFontFamilyRequested()  // opens the FONT list - every family this machine has, not a short fixed set
    signal changeAutoSizeRequested()    // opens the None/Shrink/Grow list — same chevron-Field shape as the two above
    signal changeTextCaseRequested()    // opens the As-typed / UPPER / lower / Title list (Text section)
    signal changeLineBackgroundRequested()  // the Lines card's "Background color" row (per-LINE bands)

    // The "Font family", "Weight" and "Auto size" fields themselves (not
    // this whole panel) — so each field's own dropdown can be anchored to,
    // and width-matched against, the actual trigger field instead of a
    // hardcoded offset into the panel (user call: "let the font [list] be
    // [the] same width as it[s] drop[down trigger]... fix the weight also").
    readonly property alias fontFamilyFieldItem: fontFamilyField
    readonly property alias weightFieldItem: weightField
    readonly property alias autoSizeFieldItem: autoSizeField
    // The Text section's transform field is a dropdown too, so the screen's
    // menu can anchor to the field itself (same contract as the three above).
    readonly property alias textTransformFieldItem: textTransformField

    readonly property var meta: root.target ? root.target.meta : ({})
    readonly property bool bold: root.meta.bold === true
    readonly property bool italic: root.meta.italic === true
    readonly property bool underline: root.meta.underline === true
    readonly property bool strikethrough: root.meta.strikethrough === true
    readonly property string align: root.meta.align ?? "center"
    // Mirrors `align` but for the vertical axis ("top" | "center" | "bottom") - DesignPreview.qml and
    // EditScreen.qml's canvas both already read this key; only this panel's picker was missing.
    readonly property string verticalAlign: root.meta.verticalAlign ?? "center"
    // What the item ASKS FOR (shown in the picker, so the stored name is visible)
    // and what every surface actually DRAWS with — the engine's resolution, so
    // this panel's fit measurement matches the canvas and the frame.
    readonly property string fontFamily: root.meta.fontFamily ?? FontService.defaultContentFamily
    readonly property string drawFamily: FontService.resolve(root.meta.fontFamily)
    readonly property string fontWeight: root.meta.fontWeight ?? "SemiBold"
    readonly property string autoSize: root.meta.autoSize ?? "none"
    // How the text is SHOWN whatever was typed: "none" | "upper" | "lower" | "capitalize". The typed text itself is never changed.
    readonly property string textCase: root.meta.textCase ?? "none"
    // The list style (an ENGINE list style key, see TextFormatService.listStyles): every line of the text becomes a list item.
    readonly property string listStyle: root.meta.list ?? "none"
    readonly property real fontSize: root.meta.fontSize ?? 16
    // WHAT THIS ROW SHOWS IS WHAT EVERY SURFACE DRAWS. These two read through
    // MetadataFields — the one reading the Edit canvas, every DesignPreview
    // tile and the engine's SceneBuilder also use — instead of
    // this panel's own arbitrary `?? 0` / `?? 1.0`. That pair is what produced
    // the report this replaces: the Height row read 0 for an item whose meta
    // never named a line height, while the canvas, the previews and the outputs
    // all drew 1.2x the font's natural spacing. 0 now means "the font's own
    // line height" and is displayed as that (1.0), so the number in the panel is
    // the number on screen.
    readonly property real lineHeight: MetadataFields.lineHeight(root.meta)
    // Blank air BETWEEN line boxes (the Lines card's second field) — additive px,
    // separate from the line height above, exactly FreeShow's split.
    readonly property real lineSpacing: MetadataFields.lineSpacing(root.meta)
    // The per-LINE background colour ("/" or "transparent" = none). The Lines
    // card's swatch row. One reading of "no band" for the row, its reset state
    // and MetadataFields.lineBandText, so the picker's own transparent swatch
    // turns the band OFF rather than storing a colour nothing draws.
    readonly property string lineBg: root.meta.lineBg ?? ""
    readonly property bool hasLineBg: root.lineBg !== "" && root.lineBg !== "transparent"
    // FreeShow's `lineRadius`: how round each line's band is (0 = square).
    readonly property real lineRadius: MetadataFields.lineRadius(root.meta)
    readonly property real letterSpacing: MetadataFields.letterSpacing(root.meta)
    readonly property real wordSpacing: MetadataFields.wordSpacing(root.meta)
    readonly property bool oneLine: MetadataFields.oneLine(root.meta)

    // ONE hue for every section header in this tab, defined once.
    //
    // FreeShow gives each section its own colour (its edit/values/item.ts
    // sectionColors: align #f079b2, text #e490c9, lines #ffb6e7, list #a16adb,
    // position #f079b4). Carried over literally, that produced a row of
    // near-pinks plus one purple, so the Align / List / Position & Size headers
    // did not match the Text and Lines ones — the report this answers. The tab
    // now uses the Text section's hue for all five: a single definition, so the
    // header colours cannot drift section by section again.
    readonly property color sectionHue: "#e490c9"

    // FreeShow's control shape for this panel (its edit/values/boxes.ts and the
    // box markup in EditValues.svelte): every control is its own bordered FIELD
    // — a small section-hue label on top, the value underneath, a chevron when
    // it opens a list. A dense panel stays scannable because the box is the hit
    // target and the label says what the value means, instead of one loose row
    // of names and chips where only the operator's memory says which is which.
    // The labeled chevron box (Font family / Weight / Auto size below) is
    // now the shared FieldBox.qml component — see its own header for why it
    // moved out of here.

    // One cell of the B/I/U/S group — FreeShow's four style toggles, which are a
    // single segmented control there rather than four separate buttons.
    component StyleToggle: Rectangle {
        id: toggle
        property bool active: false
        property string label: ""
        // Hairline on the cell's right edge; off for the last cell.
        property bool divider: true
        signal toggled()

        height: 36
        radius: 9
        // Premium shape: each toggle is its own rounded cell with a gap, and
        // the ON state is a soft accent wash with the accent label — the thin
        // underline bar this used to draw read as a text decoration inside a
        // box, not as a switch that is on.
        color: toggle.active
               ? Qt.alpha(Theme.accent, toggleArea.hovered ? 0.22 : 0.16)
               : (toggleArea.hovered ? Theme.hoverBg : Theme.inset)
        border.width: 1
        border.color: toggle.active ? Qt.alpha(Theme.accent, 0.45) : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
        Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        Text {
            anchors.centerIn: parent
            text: toggle.label
            color: toggle.active ? Theme.accent : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.Medium
            font.bold: toggle.label === "B"
            font.italic: toggle.label === "I"
            font.underline: toggle.label === "U"
            font.strikeout: toggle.label === "S"
        }

        // Position truth, not containsMouse — same stuck-hover failure mode
        // as Field's own fieldHit above.
        PositionHoverArea {
            id: toggleArea
            anchors.fill: parent
            checkAncestors: false
            onClicked: toggle.toggled()
        }
    }

    // The largest font size at which this item's text still fits its box's
    // inner area (box minus padding). Measured from fitMeasure below: text
    // metrics scale linearly with font size, so the fit size is the base
    // render's size scaled by how much the text over/under-fills the box.
    // Drives the Size slider's dynamic max — the slider ends exactly where
    // the text would start leaving the bounding box, instead of letting the
    // thumb travel dead space that changes nothing (or overflows the box).
    //
    // Only meaningful in shrinkToFit/growToFit, where "fits the box" is the
    // actual goal — in "none" the user has fully manual control and this
    // must stay a FIXED ceiling. It depends on fitMeasure's content size,
    // which changes with every keystroke, so leaving it live in "none" mode
    // made the slider's max (and so the thumb's position for an unchanged
    // value) visibly drift while typing, with nothing having actually
    // changed the font size — reads exactly like "the size crept up on its
    // own".
    readonly property real fitMaxFontSize: {
        if (root.autoSize === "none")
            return 400
        if (!root.target)
            return 120
        const pad = root.target.style ? root.target.style.padding : 0
        const bw = root.target.width - pad * 2
        const bh = root.target.height - pad * 2
        const cw = fitMeasure.contentWidth
        const ch = fitMeasure.contentHeight
        if (bw <= 0 || bh <= 0 || cw <= 0 || ch <= 0)
            return 400
        return Math.max(8, Math.min(400, Math.floor(root.fontSize * Math.min(bw / cw, bh / ch))))
    }

    // Reassigns the whole object — `meta` is a plain `var` property on a
    // QtObject (CanvasItem), so mutating a field on the existing object in
    // place wouldn't fire its change notification; only a fresh assignment
    // does (same reasoning as CanvasItem.qml's own header comment about why
    // it's a real QtObject in the first place).
    function setMeta(key, value) {
        if (!root.target)
            return
        const m = Object.assign({}, root.target.meta)
        m[key] = value
        root.target.meta = m
    }

    // For discrete, single-shot edits (a toggle click, an alignment pick,
    // an autoSize pick) — notifies undo once and applies, as one step.
    // Slider-driven edits go through setMeta directly instead, since their
    // undo notification brackets the whole drag via dragStarted (see the
    // Size/Height/Spacing sliders below) rather than firing per value.
    function setMetaDiscrete(key, value) {
        root.notifyUndo()
        root.setMeta(key, value)
    }

    // Several keys back to their defaults as ONE undo step — what a section's
    // Reset chip does (FreeShow's resetSection, which sweeps every input in the
    // group rather than one at a time).
    function resetMeta(values) {
        if (!root.target)
            return
        root.notifyUndo()
        const m = Object.assign({}, root.target.meta)
        for (const key in values)
            m[key] = values[key]
        root.target.meta = m
    }

    spacing: 8

    // Invisible measurement twin for fitMaxFontSize above — renders the
    // item's text with the exact styling the canvas will use (family,
    // weight chain incl. the bold override, italic, spacing, line height)
    // so the measured fit matches what the canvas shows. Invisible items
    // are skipped by positioners, so this adds no visual gap in the Column.
    // Mirrors EditScreen.qml's shrinkMeasure — keep the two in sync.
    Text {
        id: fitMeasure
        visible: false
        text: root.target ? TextFormatService.applyList(root.target.text, root.listStyle) : ""
        font: MetadataFields.textFont(root.meta, Math.max(1, root.fontSize))
        lineHeight: root.lineHeight
        lineHeightMode: Text.ProportionalHeight
        // The twin has to wrap the way the canvas does, or a one-line item is
        // measured as the multi-line block it is not.
        wrapMode: root.oneLine ? Text.NoWrap : Text.WordWrap
    }

    // ---- The always-open block ------------------------------------------
    // FreeShow's first section has no title bar: the controls an operator uses
    // on every single text item stay one click away, and only the optional
    // groups below fold away. Same card surface, so it still reads as part of
    // the stack.
    InspectorSection {
        width: parent.width
        alwaysOpen: true
        title: qsTr("Text")

        // Family, weight AND colour, all ONE row — FreeShow's own grid
        // (InspirationOrResources/FreeShow-main: EditValues.svelte's
        // "default" section + MaterialFontDropdown.svelte, whose font-family
        // dropdown and a label-less "only arrow" style dropdown already sit
        // in ONE InputRow, with a flex:1 colour swatch joining them — "do the
        // same" per user call, "your design is wasting space"). The old
        // layout gave Text colour its OWN full-width row below this one; a
        // compact swatch here (hover for the hex, HoverDetail — the shared
        // component, not a bespoke label) removes that whole wasted block.
        Row {
            id: fontFieldRow
            width: parent.width
            spacing: 8

            FieldBox {
                id: fontFamilyField
                // Widths are (row width - 2 gaps - the swatch's 50) split
                // 2:1 — the old 0.5/0.3/0.2 shares plus the 16px of gaps came
                // to more than 100%, which squeezed the Weight box down to a
                // bare chevron with no label and no value.
                width: (parent.width - 16 - 50) * 0.62
                label: qsTr("Font family")
                value: root.fontFamily
                chevron: true
                onTapped: root.changeFontFamilyRequested()
            }
            FieldBox {
                id: weightField
                width: (parent.width - 16 - 50) * 0.38
                label: qsTr("Weight")
                value: root.fontWeight
                chevron: true
                onTapped: root.changeFontRequested()
            }
            Rectangle {
                id: colorSwatchField
                width: 50
                height: 50
                radius: 10
                color: colorFieldHit.hovered ? Theme.hoverBg : Theme.inset
                border.color: Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                HoverDetail {
                    anchors.fill: parent
                    title: qsTr("Text colour")
                    detail: root.target ? (root.target.meta.color ?? "#ffffff") : "#ffffff"
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    radius: 7
                    color: root.target ? root.target.meta.color ?? "#ffffff" : "#ffffff"
                    border.color: Qt.alpha(Theme.textPrimary, 0.18)
                    border.width: 1
                }

                PositionHoverArea {
                    id: colorFieldHit
                    anchors.fill: parent
                    checkAncestors: false
                    onClicked: root.changeColorRequested()
                }
            }
        }

        // B / I / U / S as ONE segmented group — FreeShow's shape for its style
        // row (four touching cells inside a single bordered control) rather than
        // four separate chips: they are one switch group, and each cell previews
        // what it does (bold, italic, underlined, struck through).
        Item {
            id: styleGroup
            width: parent.width
            height: 36

            Row {
                anchors.fill: parent
                spacing: 6

                StyleToggle {
                    width: (styleGroup.width - 18) / 4; height: parent.height
                    label: "B"; active: root.bold
                    onToggled: root.setMetaDiscrete("bold", !root.bold)
                }
                StyleToggle {
                    width: (styleGroup.width - 18) / 4; height: parent.height
                    label: "I"; active: root.italic
                    onToggled: root.setMetaDiscrete("italic", !root.italic)
                }
                StyleToggle {
                    width: (styleGroup.width - 18) / 4; height: parent.height
                    label: "U"; active: root.underline
                    onToggled: root.setMetaDiscrete("underline", !root.underline)
                }
                StyleToggle {
                    width: (styleGroup.width - 18) / 4; height: parent.height
                    divider: false
                    label: "S"; active: root.strikethrough
                    onToggled: root.setMetaDiscrete("strikethrough", !root.strikethrough)
                }
            }
        }

        // Font size AND Auto size, one row — same FreeShow grid as the
        // family/weight/colour row above (boxes.ts' "default" section pairs
        // font-size with textFit in exactly this shape). Auto size used to
        // be three buttons inside the collapsible "Text" section below —
        // moved here as a compact chevron-Field (its own dropdown, same
        // shape as Font family/Weight) since it is a control every text item
        // uses, not an occasional one, and burying it cost a whole extra row
        // most operators never opened.
        Row {
            width: parent.width
            spacing: 8

            // An EDITABLE VALUE, not a slider (user call: "remove the slider
            // for font size, just editable values in text box"): a size is
            // typed far more often than it is dragged, and a drag cannot hit
            // "47" — the number field carries the same type-a-number value with
            // the reference's up/down stepper beside it. The dynamic fit ceiling
            // the slider used to impose is gone with it; the box is the master
            // in every auto-size mode anyway (the solver squeezes on overflow),
            // so nothing here can push text out of its box.
            NumberField {
                id: fontSizeField
                width: (parent.width - 8) * 0.58
                label: qsTr("Font size")
                value: root.fontSize
                step: 1
                decimals: 0
                minValue: 1
                maxValue: 400
                // Inert in growToFit: there the BOX is the master and the
                // rendered size follows the box regardless of this value, so
                // the control dims instead of pretending.
                enabled: root.autoSize !== "growToFit"
                opacity: root.autoSize === "growToFit" ? 0.4 : 1
                Behavior on opacity { NumberAnimation { duration: 100 } }
                onMoved: (v) => root.setMetaDiscrete("fontSize", Math.round(v))
            }

            FieldBox {
                id: autoSizeField
                width: (parent.width - 8) * 0.42
                label: qsTr("Auto size")
                value: root.autoSize === "shrinkToFit" ? qsTr("Shrink") : root.autoSize === "growToFit" ? qsTr("Grow") : qsTr("None")
                chevron: true
                onTapped: root.changeAutoSizeRequested()
            }
        }
    }

    // ---- Align ----------------------------------------------------------
    // Holding the horizontal row (left/center/right/justify) above the vertical
    // one (top/center/bottom). Icons are FreeShow's own, not hand-drawn bars —
    // see IconGlyph.qml. Vertical is where the text SITS in its box
    // top-to-bottom rather than how each line is spread left-to-right; it was
    // fixed at "center" with no way to change it (a hardcoded engine default was
    // the only lever, and only per-template, not per-item) — a tall box holding
    // variable-length bound text needs this pickable per item.
    InspectorSection {
        id: alignSection
        width: parent.width
        title: qsTr("Align")
        icon: "alignCenter"
        iconColor: root.sectionHue
        hasChanges: root.align !== "center" || root.verticalAlign !== "center"
        onResetRequested: root.resetMeta({ align: "center", verticalAlign: "center" })

        component AlignToggle: Rectangle {
            id: alignToggle
            property bool active: false
            property string icon: ""
            // Set per-row (4 horizontal buttons vs 3 vertical ones share the
            // row's width differently) - defaults to the 4-across math so a
            // stray usage doesn't collapse to 0.
            property int siblingCount: 4
            signal picked()

            width: (parent.width - (alignToggle.siblingCount - 1) * 8) / alignToggle.siblingCount
            height: 32
            radius: 8
            color: alignToggle.active ? Theme.accent : (alignArea.containsMouse ? Theme.hoverBg : Theme.inset)
            border.color: alignToggle.active ? Theme.accent : Theme.border
            border.width: 1
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

            IconGlyph {
                anchors.centerIn: parent
                name: alignToggle.icon
                fit: true
                width: 15; height: 15
                color: alignToggle.active ? Theme.controlInk : Theme.textSecondary
            }

            MouseArea {
                id: alignArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: alignToggle.picked()
            }
        }

        Row {
            width: parent.width
            spacing: 8
            AlignToggle { icon: "alignLeft"; active: root.align === "left"; onPicked: root.setMetaDiscrete("align", "left") }
            AlignToggle { icon: "alignCenter"; active: root.align === "center"; onPicked: root.setMetaDiscrete("align", "center") }
            AlignToggle { icon: "alignRight"; active: root.align === "right"; onPicked: root.setMetaDiscrete("align", "right") }
            AlignToggle { icon: "alignJustify"; active: root.align === "justify"; onPicked: root.setMetaDiscrete("align", "justify") }
        }
        Row {
            width: parent.width
            spacing: 8
            AlignToggle { siblingCount: 3; icon: "alignTop"; active: root.verticalAlign === "top"; onPicked: root.setMetaDiscrete("verticalAlign", "top") }
            AlignToggle { siblingCount: 3; icon: "alignMiddle"; active: root.verticalAlign === "center"; onPicked: root.setMetaDiscrete("verticalAlign", "center") }
            AlignToggle { siblingCount: 3; icon: "alignBottom"; active: root.verticalAlign === "bottom"; onPicked: root.setMetaDiscrete("verticalAlign", "bottom") }
        }
    }

    // ---- Text: how the characters are presented, and how they fit --------
    // FreeShow's own "Text" card (its edit/values/boxes.ts `default` group): a
    // label-over-value FIELD per control instead of a label plus a row of
    // chips — Letter spacing, Word spacing, Text transform, Text on one line.
    // The chip rows this replaced (Case as four buttons) said the same thing in
    // three times the height and offered no place for a numeric value at all.
    //
    // WORD SPACING IS HONOURED BY EVERY SURFACE, which is why it is here at all:
    // QML's font value type carries it (font.wordSpacing), so the canvas label
    // and the tile previews set it directly, and the engine carries the same
    // number as TextStyle::wordSpacing — measured by TextLayout, drawn per
    // space glyph by DrawTextObject, and part of the layout cache key so a
    // measured layout can never be served to a style that spaces differently.
    InspectorSection {
        id: textSection
        width: parent.width
        title: qsTr("Text")
        icon: "typeCase"
        iconColor: root.sectionHue
        // autoSize moved to the always-open block above (its own chevron
        // Field, beside Font size) — this section's reset is spacing/case now.
        hasChanges: root.textCase !== "none" || root.oneLine
                  || Math.abs(root.letterSpacing) > 0.001 || Math.abs(root.wordSpacing) > 0.001
        onResetRequested: root.resetMeta({ textCase: "none", oneLine: false, letterSpacing: 0, wordSpacing: 0 })

        // The number fields here are the shared NumberField.qml (its header
        // says why a value is typed or nudged rather than dragged, and why
        // the stepper is a pair of pills rather than a divider).

        Row {
            width: parent.width
            // Wider than the default 8: two bordered fields this close read as
            // one field with a divider down the middle.
            spacing: 12

            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Letter spacing")
                value: root.letterSpacing
                step: 0.1
                decimals: 1
                minValue: -2
                maxValue: 10
                showSlider: true
                onMoved: (v) => root.setMetaDiscrete("letterSpacing", v)
            }
            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Word spacing")
                value: root.wordSpacing
                step: 0.5
                minValue: -10
                maxValue: 40
                decimals: 1
                showSlider: true
                onMoved: (v) => root.setMetaDiscrete("wordSpacing", v)
            }
        }

        FieldBox {
            id: textTransformField
            width: parent.width
            label: qsTr("Text transform")
            value: root.textCase === "upper" ? qsTr("UPPERCASE")
                 : root.textCase === "lower" ? qsTr("lowercase")
                 : root.textCase === "capitalize" ? qsTr("Title Case")
                 : qsTr("As typed")
            chevron: true
            onTapped: root.changeTextCaseRequested()
        }

        // "Text on one line" — a real switch, not a chip: the canvas label
        // stops wrapping, and the engine folds the content's newlines to
        // spaces before laying the block out (see SceneBuilder), so a single
        // line on the canvas is a single line on air.
        Item {
            id: oneLineRow
            width: parent.width
            height: 24

            Rectangle {
                id: oneLineBox
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 18; height: 18; radius: 5
                color: root.oneLine ? Theme.accent : (oneLineArea.hovered ? Theme.hoverBg : Theme.inset)
                border.width: 1
                border.color: root.oneLine ? Theme.accent : Theme.border
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                IconGlyph {
                    anchors.centerIn: parent
                    visible: root.oneLine
                    name: "check"
                    color: Theme.controlInk
                    width: 12; height: 12
                }
            }
            Text {
                anchors.left: oneLineBox.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Text on one line")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 14
            }
            PositionHoverArea {
                id: oneLineArea
                anchors.fill: parent
                checkAncestors: false
                onClicked: root.setMetaDiscrete("oneLine", !root.oneLine)
            }
        }
    }

    // ---- Lines ----------------------------------------------------------
    InspectorSection {
        id: linesSection
        width: parent.width
        title: qsTr("Lines")
        icon: "textLines"
        iconColor: root.sectionHue
        // Letter spacing moved into the Text card (it is a text-presentation
        // control, and FreeShow keeps it there next to the transform); this
        // section is line height alone now.
        hasChanges: Math.abs(root.lineHeight - 1) > 0.001
                  || Math.abs(root.lineSpacing) > 0.001
                  || root.hasLineBg || root.lineRadius > 0
        onResetRequested: root.resetMeta({ lineHeight: 0, lineSpacing: 0, lineBg: "", lineRadius: 0 })

        // The SAME editable value box the Text card's spacing rows use (user
        // call: "the same for lines") — a line height is typed or nudged by a
        // tenth, so a slider was the wrong control here too. The value is the
        // multiplier QML's Text.lineHeight and the engine's lineHeight both
        // mean (1.0 = the font's own spacing; the panel's neutral is 1).
        // Two numbers, FreeShow's own split (its boxes.ts `lines` group): the
        // HEIGHT sizes one line box (a multiplier of the font's own spacing),
        // the SPACING opens blank air between one box and the next (additive
        // px). One field could not say both, and a line height alone cannot
        // make a dense verse breathe.
        Row {
            width: parent.width
            spacing: 12

            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Line height")
                value: root.lineHeight
                step: 0.1
                decimals: 2
                minValue: 0.5
                maxValue: 5
                showSlider: true
                onMoved: (v) => root.setMetaDiscrete("lineHeight", v)
            }
            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Line spacing")
                value: root.lineSpacing
                step: 1
                decimals: 0
                minValue: -50
                maxValue: 500
                showSlider: true
                onMoved: (v) => root.setMetaDiscrete("lineSpacing", v)
            }
        }

        // THE PER-LINE BACKGROUND (FreeShow's lineBg). NOT the item's background
        // — that one fills the whole box and lives on the Background row above;
        // this one draws a band behind each LINE of the text, following the
        // wrapping, so a verse can be read a line at a time. Same field shape as
        // the numbers beside it, with the swatch standing in for the value; an
        // unset band shows the chequer and reads "None", not black.
        Rectangle {
            id: lineBgField
            width: parent.width
            height: 52
            radius: 10
            color: lineBgHit.hovered ? Theme.hoverBg : Theme.inset
            border.color: lineBgHit.hovered ? Theme.border : Theme.borderSubtle
            border.width: 1
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

            Text {
                x: 12
                y: 9
                text: qsTr("Background color")
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Medium
                font.letterSpacing: 0.2
            }
            Text {
                x: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 11
                width: parent.width - 64
                elide: Text.ElideRight
                text: root.hasLineBg ? root.lineBg.toUpperCase() : qsTr("None")
                color: root.hasLineBg ? Theme.textPrimary : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 16
            }
            Rectangle {
                id: lineBgSwatch
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 26
                height: 26
                radius: 7
                clip: true
                color: root.hasLineBg ? root.lineBg : "transparent"
                border.color: Qt.alpha(Theme.textPrimary, 0.18)
                border.width: 1

                // "None" is transparency, so it is SHOWN as transparency (the
                // app's one chequer) instead of a black-looking swatch.
                Checkerboard {
                    anchors.fill: parent
                    visible: !root.hasLineBg
                    // The shared board (13px cells), not this swatch's own 7px:
                    // a smaller cell on one surface made the same "no background"
                    // read as a finer pattern than the canvas behind it.
                }
            }
            PositionHoverArea {
                id: lineBgHit
                anchors.fill: parent
                checkAncestors: false
                onClicked: root.changeLineBackgroundRequested()
            }
        }

        // How round each line's band is (FreeShow's lineRadius). Half width, the
        // same grid as the pair above, and dimmed while no band is set — a radius
        // nothing draws would be a control that lies.
        NumberField {
            width: (parent.width - 12) / 2
            label: qsTr("Line radius")
            value: root.lineRadius
            step: 1
            decimals: 0
            minValue: 0
            maxValue: 100
            enabled: root.hasLineBg
            onMoved: (v) => root.setMetaDiscrete("lineRadius", v)
        }
    }

    // ---- List -----------------------------------------------------------
    // Like FreeShow's list option: every line becomes an item, marked with a
    // bullet, a dash, numbers, letters ... The typed text is never changed (the
    // marker is added when it is shown), and the styles come from the engine.
    InspectorSection {
        id: listSection
        width: parent.width
        title: qsTr("List")
        icon: "listBullets"
        iconColor: root.sectionHue
        hasChanges: root.listStyle !== "none"
        onResetRequested: root.setMetaDiscrete("list", "none")

        Flow {
            width: parent.width
            spacing: 8

            Repeater {
                model: TextFormatService.listStyles()
                delegate: Rectangle {
                    id: listBtn
                    required property var modelData
                    readonly property bool active: root.listStyle === listBtn.modelData.key

                    width: listBtn.modelData.key === "none" ? 64 : 48
                    height: 32
                    radius: 8
                    color: listBtn.active ? Theme.accent : (listArea.containsMouse ? Theme.hoverBg : Theme.inset)
                    border.color: listBtn.active ? Theme.accent : Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    Text {
                        anchors.centerIn: parent
                        text: listBtn.modelData.key === "none" ? qsTr("None") : listBtn.modelData.sample
                        color: listBtn.active ? Theme.controlInk : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: listBtn.active ? Font.DemiBold : Font.Medium
                    }

                    MouseArea {
                        id: listArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setMetaDiscrete("list", listBtn.modelData.key)
                    }
                }
            }
        }
    }

    // ---- Position & Size ------------------------------------------------
    InspectorSection {
        width: parent.width
        title: qsTr("Position & Size")
        icon: "gridView"
        iconColor: root.sectionHue

        Grid {
            width: parent.width
            columns: 4
            columnSpacing: 8
            rowSpacing: 8

            Repeater {
                model: [
                    { key: "x", label: qsTr("X") },
                    { key: "y", label: qsTr("Y") },
                    { key: "w", label: qsTr("W") },
                    { key: "h", label: qsTr("H") }
                ]
                delegate: Rectangle {
                    id: geomBox
                    required property var modelData

                    width: (parent.width - 24) / 4
                    height: 40
                    radius: 8
                    color: Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1

                    Column {
                        anchors.centerIn: parent
                        spacing: 1

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: geomBox.modelData.label
                            color: Theme.accent
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.Medium
                        }
                        TextInput {
                            id: geomInput
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            validator: IntValidator { bottom: -100000; top: 100000 }
                            selectByMouse: true
                            text: {
                                if (!root.target)
                                    return "0"
                                const k = geomBox.modelData.key
                                const v = k === "x" ? root.target.x : k === "y" ? root.target.y
                                    : k === "w" ? root.target.width : root.target.height
                                return String(Math.round(v))
                            }
                            onEditingFinished: {
                                if (!root.target)
                                    return
                                root.notifyUndo()
                                const v = parseInt(text || "0", 10)
                                const k = geomBox.modelData.key
                                if (k === "x") root.target.x = v
                                else if (k === "y") root.target.y = v
                                else if (k === "w") root.target.width = Math.max(1, v)
                                else root.target.height = Math.max(1, v)
                            }
                        }
                    }
                }
            }
        }
    }
}
