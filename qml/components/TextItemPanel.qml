import QtQuick

// The right panel's "TEXT" tab content — shown when a "text" kind canvas
// item is selected (see EditScreen.qml's rightPanelTab). Edits the item's
// typography (bold/italic/underline/strikethrough, alignment, size, line
// height, letter spacing, auto-size mode) plus its raw geometry, straight
// off the item's own `meta` bag (see CanvasItem.qml) and x/y/width/height —
// the same live-object-binding convention SizeStyleCard.qml uses for style,
// so a change here shows up on the canvas immediately, no mirroring layer.
//
// `meta` fields this owns: bold, italic, underline, strikethrough,
// align ("left"|"center"|"right"|"justify"), fontFamily, fontWeight
// (display string only, no picker UI yet), autoSize ("none"|"shrinkToFit"|
// "growToFit" — FreeShow's textFit vocabulary; legacy "shrink"/"grow"
// values are mapped on the canvas side), fontSize, lineHeight,
// letterSpacing. All optional — an item created before this panel existed
// just reads as the same defaults EditableCanvasLabel already rendered
// (16px, Medium weight, centered).
//
// Literal colors, not Theme.* — same AOT-compiler limitation as
// SizeStyleCard.qml at this nesting depth.
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

    readonly property var meta: root.target ? root.target.meta : ({})
    readonly property bool bold: root.meta.bold === true
    readonly property bool italic: root.meta.italic === true
    readonly property bool underline: root.meta.underline === true
    readonly property bool strikethrough: root.meta.strikethrough === true
    readonly property string align: root.meta.align ?? "center"
    // Mirrors `align` but for the vertical axis ("top" | "center" | "bottom") - DesignPreview.qml and
    // EditScreen.qml's canvas both already read this key; only this panel's picker was missing.
    readonly property string verticalAlign: root.meta.verticalAlign ?? "center"
    readonly property string fontFamily: root.meta.fontFamily ?? "Segoe UI"
    readonly property string fontWeight: root.meta.fontWeight ?? "SemiBold"
    readonly property string autoSize: root.meta.autoSize ?? "none"
    // How the text is SHOWN whatever was typed: "none" | "upper" | "lower" | "capitalize". The typed text itself is never changed.
    readonly property string textCase: root.meta.textCase ?? "none"
    // The list style (an ENGINE list style key, see TextFormatService.listStyles): every line of the text becomes a list item.
    readonly property string listStyle: root.meta.list ?? "none"
    readonly property real fontSize: root.meta.fontSize ?? 16
    readonly property real lineHeight: root.meta.lineHeight ?? 1.2
    readonly property real letterSpacing: root.meta.letterSpacing ?? 0

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

    spacing: 16

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
        font.family: root.fontFamily
        font.pixelSize: Math.max(1, root.fontSize)
        font.weight: root.bold ? Font.Bold
            : root.fontWeight === "Regular" ? Font.Normal
            : root.fontWeight === "SemiBold" ? Font.DemiBold
            : root.fontWeight === "Bold" ? Font.Bold
            : Font.Medium
        font.italic: root.italic
        font.letterSpacing: root.letterSpacing
        lineHeight: root.lineHeight
        lineHeightMode: Text.ProportionalHeight
    }

    Rectangle {
        width: 84
        height: 22
        radius: 5
        color: "#6c5ce7"

        Text {
            anchors.centerIn: parent
            text: qsTr("TEXT ITEM")
            color: "#ffffff"
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.Bold
        }
    }

    // Style toggles (B/I/U/S) — same segmented-chip language as SizeStyleCard's Line/Dotted/Dashed selector.
    Row {
        width: parent.width
        spacing: 6

        component StyleToggle: Rectangle {
            id: toggle
            property bool active: false
            property string label: ""
            signal toggled()

            width: 40
            height: 32
            radius: 8
            color: toggle.active ? "#6c5ce7" : (toggleArea.containsMouse ? "#20222c" : "#1a1c26")
            border.color: toggle.active ? "#6c5ce7" : "#2a2f3a"
            border.width: 1
            Behavior on color { ColorAnimation { duration: 100 } }

            Text {
                anchors.centerIn: parent
                text: toggle.label
                color: toggle.active ? "#ffffff" : "#c8cdd9"
                font.family: "Segoe UI"
                font.pixelSize: 15
                font.bold: toggle.label === "B"
                font.italic: toggle.label === "I"
                font.underline: toggle.label === "U"
                font.strikeout: toggle.label === "S"
            }

            MouseArea {
                id: toggleArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: toggle.toggled()
            }
        }

        StyleToggle { label: "B"; active: root.bold; onToggled: root.setMetaDiscrete("bold", !root.bold) }
        StyleToggle { label: "I"; active: root.italic; onToggled: root.setMetaDiscrete("italic", !root.italic) }
        StyleToggle { label: "U"; active: root.underline; onToggled: root.setMetaDiscrete("underline", !root.underline) }
        StyleToggle { label: "S"; active: root.strikethrough; onToggled: root.setMetaDiscrete("strikethrough", !root.strikethrough) }
    }

    // ---- Align — its own collapsible section (SizeStyleCard's header language: label + chevron, click to
    // fold), holding the horizontal row (left/center/right/justify) above the vertical one (top/center/
    // bottom). Icons are FreeShow's own (src/frontend/values/icons.ts alignLeft/Center/Right/Justify/Top/
    // Middle/Bottom), not hand-drawn bars - see IconGlyph.qml. Vertical is where the text SITS in its box
    // top-to-bottom rather than how each line is spread left-to-right; it was fixed at "center" with no way
    // to change it (a hardcoded engine default was the only lever, and only per-template, not per-item) - a
    // tall box holding variable-length bound text (a sermon paragraph, say) needs this pickable per item,
    // the same as FreeShow's own alignY (edit/values/boxes.ts: align-items flex-start/center/flex-end).
    Column {
        id: alignSection
        width: parent.width
        spacing: 10

        property bool expanded: true

        Rectangle {
            width: parent.width
            height: 44
            radius: 8
            color: "#161823"

            Row {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                IconGlyph { anchors.verticalCenter: parent.verticalCenter; name: "alignCenter"; color: "#e0399f"; width: 14; height: 14 }
                Text {
                    text: qsTr("Align")
                    color: "#eef1f8"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }
            }

            IconGlyph {
                x: parent.width - 28
                anchors.verticalCenter: parent.verticalCenter
                name: alignSection.expanded ? "chevronUp" : "chevronDown"
                color: "#6b7280"
                width: 12; height: 12
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: alignSection.expanded = !alignSection.expanded
            }
        }

        Column {
            width: parent.width
            spacing: 6
            visible: alignSection.expanded
            height: visible ? implicitHeight : 0

            component AlignToggle: Rectangle {
                id: alignToggle
                property bool active: false
                property string icon: ""
                // Set per-row (4 horizontal buttons vs 3 vertical ones share the row's width differently) -
                // defaults to the 4-across math so a stray usage doesn't collapse to 0.
                property int siblingCount: 4
                signal picked()

                width: (parent.width - (alignToggle.siblingCount - 1) * 6) / alignToggle.siblingCount
                height: 32
                radius: 8
                color: alignToggle.active ? "#6c5ce7" : (alignArea.containsMouse ? "#20222c" : "#1a1c26")
                border.color: alignToggle.active ? "#6c5ce7" : "#2a2f3a"
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                IconGlyph {
                    anchors.centerIn: parent
                    name: alignToggle.icon
                    fit: true
                    width: 15; height: 15
                    color: alignToggle.active ? "#ffffff" : "#c8cdd9"
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
                spacing: 6
                AlignToggle { icon: "alignLeft"; active: root.align === "left"; onPicked: root.setMetaDiscrete("align", "left") }
                AlignToggle { icon: "alignCenter"; active: root.align === "center"; onPicked: root.setMetaDiscrete("align", "center") }
                AlignToggle { icon: "alignRight"; active: root.align === "right"; onPicked: root.setMetaDiscrete("align", "right") }
                AlignToggle { icon: "alignJustify"; active: root.align === "justify"; onPicked: root.setMetaDiscrete("align", "justify") }
            }
            Row {
                width: parent.width
                spacing: 6
                AlignToggle { siblingCount: 3; icon: "alignTop"; active: root.verticalAlign === "top"; onPicked: root.setMetaDiscrete("verticalAlign", "top") }
                AlignToggle { siblingCount: 3; icon: "alignMiddle"; active: root.verticalAlign === "center"; onPicked: root.setMetaDiscrete("verticalAlign", "center") }
                AlignToggle { siblingCount: 3; icon: "alignBottom"; active: root.verticalAlign === "bottom"; onPicked: root.setMetaDiscrete("verticalAlign", "bottom") }
            }
        }
    }

    // Font family + weight, each its own click target: the family name opens the FULL list (every font this machine has, via
    // Qt.fontFamilies() - EditScreen.qml builds the dropdown), the weight pill opens the short Regular/Medium/SemiBold/Bold list,
    // unchanged. The color circle opens the shared color picker (see EditScreen.qml's bgColorModal, reused for border/background
    // colors too) targeting this item's text color.
    Rectangle {
        width: parent.width
        height: 46
        radius: 8
        color: "#161823"

        Row {
            id: fontRow
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Aa"
                color: "#eef1f8"
                font.family: "Segoe UI"
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Text {
                id: fontFamilyLabel
                anchors.verticalCenter: parent.verticalCenter
                text: root.fontFamily
                color: "#c8cdd9"
                font.family: "Segoe UI"
                font.pixelSize: 14
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 120)
            }
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                name: "chevronDown"
                color: "#6b7280"
                width: 10; height: 10
            }
        }

        MouseArea {
            id: familyArea
            anchors.left: parent.left
            anchors.right: weightPill.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            cursorShape: Qt.PointingHandCursor
            onClicked: root.changeFontFamilyRequested()
        }

        // A plain Item, not a Row, wraps the pill: a Row forbids anchoring (even `fill`) on its own children, and the click
        // target here needs to extend a few px past the visible pill for an easy hit.
        Item {
            id: weightPill
            anchors.right: colorChip.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: weightRow.width
            height: weightRow.height

            Row {
                id: weightRow
                spacing: 4

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.fontWeight
                    color: "#8a94a6"
                    font.family: "Segoe UI"
                    font.pixelSize: 13
                }
                IconGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevronDown"
                    color: "#6b7280"
                    width: 8; height: 8
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                onClicked: root.changeFontRequested()
            }
        }

        // Color swatch + "Change" chip — same layout language as the right
        // panel's Background row (swatch, pill, chevron).
        Row {
            id: colorChip
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                radius: 11
                color: root.target ? root.target.meta.color ?? "#ffffff" : "#ffffff"
                border.color: "#3a4a7a"
                border.width: 1
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                radius: 12
                color: colorChangeArea.containsMouse ? "#20242f" : "#1a1c26"
                border.color: "#2a3140"
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Change")
                    color: "#aeb6c8"
                    font.family: "Segoe UI"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: colorChangeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.changeColorRequested()
                }
            }
        }
    }

    // Letter case - same segmented-chip language as AUTO SIZE below.
    Column {
        width: parent.width
        spacing: 8

        Text {
            text: qsTr("CASE")
            color: "#5c6475"
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.Bold
        }

        Row {
            width: parent.width
            spacing: 8

            Repeater {
                model: [
                    { key: "none", label: qsTr("As typed") },
                    { key: "upper", label: qsTr("UPPER") },
                    { key: "lower", label: qsTr("lower") },
                    { key: "capitalize", label: qsTr("Title") }
                ]
                delegate: Rectangle {
                    id: caseBtn
                    required property var modelData
                    readonly property bool active: root.textCase === caseBtn.modelData.key

                    width: (parent.width - 24) / 4
                    height: 32
                    radius: 8
                    color: caseBtn.active ? "#6c5ce7" : (caseArea.containsMouse ? "#20222c" : "#1a1c26")
                    border.color: caseBtn.active ? "#6c5ce7" : "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: caseBtn.modelData.label
                        color: caseBtn.active ? "#ffffff" : "#c8cdd9"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                        font.weight: caseBtn.active ? Font.DemiBold : Font.Medium
                    }

                    MouseArea {
                        id: caseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setMetaDiscrete("textCase", caseBtn.modelData.key)
                    }
                }
            }
        }
    }

    Column {
        width: parent.width
        spacing: 8

        Text {
            text: qsTr("AUTO SIZE")
            color: "#5c6475"
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.Bold
        }

        Row {
            width: parent.width
            spacing: 8

            Repeater {
                // FreeShow's exact textFit vocabulary (see its show.ts Item
                // type and autosize.ts): shrinkToFit = set size, shrinking
                // only on overflow; growToFit = fill the box; none = fixed.
                // The canvas side maps legacy "shrink"/"grow" keys across.
                model: [
                    { key: "none", label: qsTr("None") },
                    { key: "shrinkToFit", label: qsTr("Shrink") },
                    { key: "growToFit", label: qsTr("Grow") }
                ]
                delegate: Rectangle {
                    id: sizeBtn
                    required property var modelData
                    readonly property bool active: root.autoSize === sizeBtn.modelData.key

                    width: (parent.width - 16) / 3
                    height: 32
                    radius: 8
                    color: sizeBtn.active ? "#6c5ce7" : (sizeArea.containsMouse ? "#20222c" : "#1a1c26")
                    border.color: sizeBtn.active ? "#6c5ce7" : "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: sizeBtn.modelData.label
                        color: sizeBtn.active ? "#ffffff" : "#c8cdd9"
                        font.family: "Segoe UI"
                        font.pixelSize: 14
                        font.weight: sizeBtn.active ? Font.DemiBold : Font.Medium
                    }

                    MouseArea {
                        id: sizeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setMetaDiscrete("autoSize", sizeBtn.modelData.key)
                    }
                }
            }
        }
    }

    Column {
        width: parent.width
        spacing: 14

        LabeledSlider {
            width: parent.width
            label: qsTr("Size")
            value: root.fontSize
            minValue: 8
            // Dynamic max = the fit limit (see fitMaxFontSize) — no dead
            // travel, no overflow. Inert in growToFit: there the BOX is the
            // master and the rendered size follows the box regardless of
            // this value, so the control dims instead of pretending.
            maxValue: root.fitMaxFontSize
            enabled: root.autoSize !== "growToFit"
            opacity: root.autoSize === "growToFit" ? 0.4 : 1
            Behavior on opacity { NumberAnimation { duration: 100 } }
            onDragStarted: root.notifyUndo()
            onMoved: (v) => root.setMeta("fontSize", Math.round(v))
        }
        LabeledSlider {
            width: parent.width
            label: qsTr("Height")
            value: root.lineHeight
            minValue: 0.8
            maxValue: 3
            decimals: 1
            onDragStarted: root.notifyUndo()
            onMoved: (v) => root.setMeta("lineHeight", Math.round(v * 10) / 10)
        }
        LabeledSlider {
            width: parent.width
            label: qsTr("Spacing")
            value: root.letterSpacing
            minValue: -2
            maxValue: 10
            decimals: 1
            onDragStarted: root.notifyUndo()
            onMoved: (v) => root.setMeta("letterSpacing", Math.round(v * 10) / 10)
        }
    }

    // List - like FreeShow's list option: every line becomes an item, marked with a bullet, a dash, numbers, letters ...
    // The typed text is never changed (the marker is added when it is shown), and the styles come from the engine.
    // Same collapsible-section language as Align above (FreeShow's own "list" glyph as the header icon). Moved
    // below Auto Size/the sliders (user call) - was sitting right after Case, ahead of the sizing controls.
    Column {
        id: listSection
        width: parent.width
        spacing: 10

        property bool expanded: true

        Rectangle {
            width: parent.width
            height: 44
            radius: 8
            color: "#161823"

            Row {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                IconGlyph { anchors.verticalCenter: parent.verticalCenter; name: "listBullets"; color: "#e0399f"; width: 14; height: 14 }
                Text {
                    text: qsTr("List")
                    color: "#eef1f8"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }
            }

            IconGlyph {
                x: parent.width - 28
                anchors.verticalCenter: parent.verticalCenter
                name: listSection.expanded ? "chevronUp" : "chevronDown"
                color: "#6b7280"
                width: 12; height: 12
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: listSection.expanded = !listSection.expanded
            }
        }

        Flow {
            width: parent.width
            spacing: 8
            visible: listSection.expanded
            height: visible ? implicitHeight : 0

            Repeater {
                model: TextFormatService.listStyles()
                delegate: Rectangle {
                    id: listBtn
                    required property var modelData
                    readonly property bool active: root.listStyle === listBtn.modelData.key

                    width: listBtn.modelData.key === "none" ? 64 : 48
                    height: 32
                    radius: 8
                    color: listBtn.active ? "#6c5ce7" : (listArea.containsMouse ? "#20222c" : "#1a1c26")
                    border.color: listBtn.active ? "#6c5ce7" : "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: listBtn.modelData.key === "none" ? qsTr("None") : listBtn.modelData.sample
                        color: listBtn.active ? "#ffffff" : "#c8cdd9"
                        font.family: "Segoe UI"
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

    Column {
        width: parent.width
        spacing: 8

        Text {
            text: qsTr("POSITION & SIZE")
            color: "#5c6475"
            font.family: "Segoe UI"
            font.pixelSize: 10
            font.weight: Font.Bold
        }

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
                    color: "#161823"

                    Column {
                        anchors.centerIn: parent
                        spacing: 1

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: geomBox.modelData.label
                            color: "#5c6475"
                            font.family: "Segoe UI"
                            font.pixelSize: 9
                        }
                        TextInput {
                            id: geomInput
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#eef1f8"
                            font.family: "Segoe UI"
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
