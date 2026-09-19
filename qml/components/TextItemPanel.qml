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
    signal changeFontRequested()

    readonly property var meta: root.target ? root.target.meta : ({})
    readonly property bool bold: root.meta.bold === true
    readonly property bool italic: root.meta.italic === true
    readonly property bool underline: root.meta.underline === true
    readonly property bool strikethrough: root.meta.strikethrough === true
    readonly property string align: root.meta.align ?? "center"
    readonly property string fontFamily: root.meta.fontFamily ?? "Inter"
    readonly property string fontWeight: root.meta.fontWeight ?? "SemiBold"
    readonly property string autoSize: root.meta.autoSize ?? "none"
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
    readonly property real fitMaxFontSize: {
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
        text: root.target ? root.target.text : ""
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
            font.family: "Inter"
            font.pixelSize: 9
            font.weight: Font.Bold
        }
    }

    // Style toggles (B/I/U/S) + alignment, one row — same segmented-chip
    // language as SizeStyleCard's Line/Dotted/Dashed selector.
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
                font.family: "Inter"
                font.pixelSize: 13
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

        // Alignment — four mutually-exclusive buttons, each a tiny
        // hand-drawn bar icon (no source SVG path data for these, same
        // simplified-icon status as ShapeSourceModal's glyphs) showing
        // left/center/right/justify as differently-aligned bar stacks.
        component AlignToggle: Rectangle {
            id: alignToggle
            property bool active: false
            property string mode: "left"
            signal picked()

            width: 40
            height: 32
            radius: 8
            color: alignToggle.active ? "#6c5ce7" : (alignArea.containsMouse ? "#20222c" : "#1a1c26")
            border.color: alignToggle.active ? "#6c5ce7" : "#2a2f3a"
            border.width: 1
            Behavior on color { ColorAnimation { duration: 100 } }

            Column {
                anchors.centerIn: parent
                spacing: 2

                Repeater {
                    model: 3
                    delegate: Rectangle {
                        required property int index
                        readonly property real barWidth: alignToggle.mode === "justify" ? 16
                            : index === 1 ? 11 : 16
                        height: 1.5
                        width: barWidth
                        radius: 0.75
                        color: alignToggle.active ? "#ffffff" : "#c8cdd9"
                        anchors.left: alignToggle.mode === "left" ? parent.left : undefined
                        anchors.right: alignToggle.mode === "right" ? parent.right : undefined
                        anchors.horizontalCenter: alignToggle.mode === "center" || alignToggle.mode === "justify" ? parent.horizontalCenter : undefined
                    }
                }
            }

            MouseArea {
                id: alignArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: alignToggle.picked()
            }
        }

        AlignToggle { mode: "left"; active: root.align === "left"; onPicked: root.setMetaDiscrete("align", "left") }
        AlignToggle { mode: "center"; active: root.align === "center"; onPicked: root.setMetaDiscrete("align", "center") }
        AlignToggle { mode: "right"; active: root.align === "right"; onPicked: root.setMetaDiscrete("align", "right") }
        AlignToggle { mode: "justify"; active: root.align === "justify"; onPicked: root.setMetaDiscrete("align", "justify") }
    }

    // Font family/weight — informational for now (no font list/weight
    // picker built yet); the color circle opens the shared color picker
    // (see EditScreen.qml's bgColorModal, reused for border/background
    // colors too) targeting this item's text color.
    Rectangle {
        width: parent.width
        height: 46
        radius: 8
        color: "#161823"

        // Family/weight — click anywhere in this row to open the weight
        // dropdown (see changeFontRequested below; EditScreen.qml supplies
        // the actual DropdownPanel, same shared-menu convention as the
        // slide/canvas context menus). No font-family list yet, just the
        // weight presets — a family picker is future work.
        Row {
            id: fontRow
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Aa"
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.fontFamily + " " + root.fontWeight
                color: "#c8cdd9"
                font.family: "Inter"
                font.pixelSize: 12
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "▾"
                color: "#6b7280"
                font.pixelSize: 10
            }
        }

        MouseArea {
            anchors.left: parent.left
            anchors.right: colorChip.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            cursorShape: Qt.PointingHandCursor
            onClicked: root.changeFontRequested()
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
                    font.family: "Inter"
                    font.pixelSize: 10
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

    Column {
        width: parent.width
        spacing: 8

        Text {
            text: qsTr("AUTO SIZE")
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
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
                        font.family: "Inter"
                        font.pixelSize: 12
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

    Column {
        width: parent.width
        spacing: 8

        Text {
            text: qsTr("POSITION & SIZE")
            color: "#5c6475"
            font.family: "Inter"
            font.pixelSize: 9
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
                            font.family: "Inter"
                            font.pixelSize: 8
                        }
                        TextInput {
                            id: geomInput
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: "#eef1f8"
                            font.family: "Inter"
                            font.pixelSize: 12
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
