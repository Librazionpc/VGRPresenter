import QtQuick
import VGRPresenterUI

Column {
    id: root

    // The CanvasItem being edited (must be kind "text"), or null.
    property var target: null

    property var undoHook: null
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
    readonly property string fontFamily: root.meta.fontFamily ?? FontService.defaultContentFamily
    readonly property string drawFamily: FontService.resolve(root.meta.fontFamily)
    readonly property string fontWeight: root.meta.fontWeight ?? "SemiBold"
    readonly property string autoSize: root.meta.autoSize ?? "none"
    // How the text is SHOWN whatever was typed: "none" | "upper" | "lower" | "capitalize". The typed text itself is never changed.
    readonly property string textCase: root.meta.textCase ?? "none"
    // The list style (an ENGINE list style key, see TextFormatService.listStyles): every line of the text becomes a list item.
    readonly property string listStyle: root.meta.list ?? "none"
    readonly property real fontSize: root.meta.fontSize ?? 16
    readonly property real lineHeight: MetadataFields.lineHeight(root.meta)
    // Blank air BETWEEN line boxes (the Lines card's second field) — additive px,
    // separate from the line height above, exactly FreeShow's split.
    readonly property real lineSpacing: MetadataFields.lineSpacing(root.meta)
    readonly property string lineBg: root.meta.lineBg ?? ""
    readonly property bool hasLineBg: root.lineBg !== "" && root.lineBg !== "transparent"
    // FreeShow's `lineRadius`: how round each line's band is (0 = square).
    readonly property real lineRadius: MetadataFields.lineRadius(root.meta)
    readonly property real letterSpacing: MetadataFields.letterSpacing(root.meta)
    readonly property real wordSpacing: MetadataFields.wordSpacing(root.meta)
    readonly property bool oneLine: MetadataFields.oneLine(root.meta)

    readonly property color sectionHue: "#e490c9"

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

    function setMeta(key, value) {
        if (!root.target)
            return
        const m = Object.assign({}, root.target.meta)
        m[key] = value
        root.target.meta = m
    }

    function setMetaDiscrete(key, value) {
        root.notifyUndo()
        root.setMeta(key, value)
    }

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
    InspectorSection {
        width: parent.width
        alwaysOpen: true
        title: qsTr("Text")

        Row {
            id: fontFieldRow
            width: parent.width
            spacing: 8

            FieldBox {
                id: fontFamilyField
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

        Row {
            width: parent.width
            spacing: 8

            NumberField {
                id: fontSizeField
                width: (parent.width - 8) * 0.58
                label: qsTr("Font size")
                value: root.fontSize
                step: 1
                decimals: 0
                minValue: 1
                maxValue: 400
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
        hasChanges: Math.abs(root.lineHeight - 1) > 0.001
                  || Math.abs(root.lineSpacing) > 0.001
                  || root.hasLineBg || root.lineRadius > 0
        onResetRequested: root.resetMeta({ lineHeight: 0, lineSpacing: 0, lineBg: "", lineRadius: 0 })

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
                }
            }
            PositionHoverArea {
                id: lineBgHit
                anchors.fill: parent
                checkAncestors: false
                onClicked: root.changeLineBackgroundRequested()
            }
        }

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