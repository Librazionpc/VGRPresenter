import QtQuick

// The Edit screen's item-style cards, in FreeShow's collapsing-card language
// (see InspectorSection.qml — one shared card shape, ported from FreeShow's
// edit tools). Two sections, because they answer two different questions:
//
//   Size & Style — the box itself: Padding and Corner Radius. Corner Radius is
//                  deliberately NOT part of Border (it rounds the box's own
//                  fill/outline/selection chrome, so it works with the border
//                  off).
//   Border       — the stroke: on/off, colour, Width, Line/Dotted/Dashed.
//
// Item background fill is still not a control here: the top-of-panel
// "Background" row (EditScreen.qml) already picks a colour, and targets the
// selected item when something is selected, so this card does not duplicate
// that swatch+chip a second time.
//
// The card edits CanvasItemStyle value objects directly: point `targets` at the
// style object(s) of the currently selected canvas item(s) and every control
// binds to targets[0] and writes to all of them. No mirroring layer, no
// per-field signals — the card is reusable anywhere CanvasItemStyle is. An
// empty array disables the controls.
//
// There is no Opacity slider: item transparency lives on FiltersCard (it only
// ever made sense for some kinds, and folding a kind-gated control into a card
// everything uses unconditionally is what made the first removal necessary).
Column {
    id: root

    // The CanvasItemStyle object(s) being edited. [0] drives what the controls
    // display; any change is applied to every target.
    property list<CanvasItemStyle> targets: []

    readonly property CanvasItemStyle primary: targets.length > 0 ? targets[0] : null
    readonly property bool hasTargets: primary !== null

    // False for a kind whose own content (camera's live preview, the generic
    // media/audio/shape/timer/clock placeholder) fully covers its box with an
    // opaque visual — a background fill or border stroke drawn underneath it
    // would never be seen, so the Border section greys out.
    property bool fillSupported: true

    // Same reusable undo-notification pattern as TextItemPanel.qml's own
    // undoHook (see its header comment for the full reasoning).
    property var undoHook: null
    // Called BEFORE undoHook — hands keyboard focus back to the canvas first
    // (see EditScreen.qml's handleCanvasSelect and DraggableCanvasText's
    // onPressed) so Ctrl+Z/Ctrl+Y are not gated off by an in-progress text edit.
    property var focusHook: null
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    // Fired when the Border row's "Change" chip is clicked — this card owns no
    // colour-picker UI; the consumer reuses its BackgroundColorModal.
    signal changeBorderRequested()
    // Fired when the Style field (Line/Dotted/Dashed) is tapped — this card
    // owns no popup UI of its own (same reasoning as changeBorderRequested);
    // the consumer opens a DropdownPanel anchored to borderStyleFieldItem.
    signal changeBorderStyleRequested()
    // Exposed so the consumer's DropdownPanel can anchor to and width-match
    // this exact field (see TextItemPanel.qml's fontFamilyFieldItem for the
    // same pattern).
    readonly property alias borderStyleFieldItem: borderStyleField

    // Shorthands over the primary target so the bindings above stay short.
    readonly property bool borderEnabled: primary ? primary.borderEnabled : false
    readonly property string borderStyle: primary ? primary.borderStyle : "line"
    readonly property string borderStyleLabel: {
        switch (root.borderStyle) {
        case "dotted": return qsTr("Dotted")
        case "dashed": return qsTr("Dashed")
        default: return qsTr("Line")
        }
    }
    readonly property real padding: primary ? primary.padding : 0
    readonly property real cornerRadius: primary ? primary.cornerRadius : 0

    // FreeShow's sectionColors: box = #f079b4 (its "position" hue),
    // border = #d8d8d8 (its own neutral). The same hues are used on the TEXT
    // tab so a section keeps its colour across tabs.
    readonly property color boxHue: "#f079b4"
    readonly property color borderHue: "#d8d8d8"

    spacing: 8

    // ---- The box itself ----
    InspectorSection {
        width: parent.width
        title: qsTr("Size & Style")
        icon: "shape"
        iconColor: root.boxHue
        hasChanges: root.hasTargets && (root.padding !== 0 || root.cornerRadius !== 0)
        onResetRequested: {
            root.notifyUndo()
            root.targets.forEach((t) => {
                t.padding = 0
                t.cornerRadius = 0
            })
        }

        // The SAME editable value fields the Text card uses (NumberField.qml):
        // padding and radius are pixel values typed or nudged by one, and the
        // card already reads as a column of bordered fields.
        Row {
            width: parent.width
            spacing: 12

            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Padding")
                value: root.padding
                step: 1
                decimals: 0
                minValue: 0
                maxValue: 64
                enabled: root.hasTargets
                // Same hunt-by-eye case as Opacity (user call: "do the same
                // for padding and corner radius").
                showSlider: true
                onMoved: (v) => {
                    root.notifyUndo()
                    root.targets.forEach((t) => t.padding = v)
                }
            }

            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Corner Radius")
                value: root.cornerRadius
                step: 1
                decimals: 0
                minValue: 0
                maxValue: 48
                enabled: root.hasTargets
                showSlider: true
                onMoved: (v) => {
                    root.notifyUndo()
                    root.targets.forEach((t) => t.cornerRadius = v)
                }
            }
        }
    }

    // ---- The stroke ----
    InspectorSection {
        width: parent.width
        title: qsTr("Border")
        icon: "layoutTemplate"
        iconColor: root.borderHue
        // The card-level "this kind of item shows no fill at all" dimming lives
        // on the whole section, so it reads as one greyed group.
        opacity: root.fillSupported ? 1 : 0.4
        Behavior on opacity { NumberAnimation { duration: 100 } }
        hasChanges: root.borderEnabled
        onResetRequested: {
            root.notifyUndo()
            root.targets.forEach((t) => t.borderEnabled = false)
        }

        // On/off row: the checkbox and its label on the left, the colour swatch
        // and "Change" chip on the right — the same swatch/chip language as the
        // panel's Background row.
        Item {
            width: parent.width
            height: 26
            enabled: root.fillSupported

            Rectangle {
                id: borderCheckbox
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                height: 18
                width: 18
                radius: 4
                color: root.borderEnabled ? Theme.accent : Theme.inset
                border.color: root.borderEnabled ? Theme.accent : Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    visible: root.borderEnabled
                    text: "✓"
                    color: Theme.controlInk
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.notifyUndo()
                        root.targets.forEach((t) => t.borderEnabled = !root.borderEnabled)
                    }
                }
            }

            Text {
                anchors.left: borderCheckbox.right
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Show border")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 13

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.notifyUndo()
                        root.targets.forEach((t) => t.borderEnabled = !root.borderEnabled)
                    }
                }
            }

            Rectangle {
                anchors.right: changeChip.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                height: 22
                width: 22
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 5
                Behavior on opacity { NumberAnimation { duration: 100 } }
                color: root.primary ? root.primary.borderColor : Theme.controlInk
            }

            Rectangle {
                id: changeChip
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: Theme.borderSubtle
                border.width: 1
                color: borderChangeArea.containsMouse ? Theme.hoverBg : Theme.inset
                radius: 12
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                Behavior on opacity { NumberAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Change")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: borderChangeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.changeBorderRequested()
                }
            }
        }

        // Width beside Style now (user call: "Border Lines beside the width
        // then make dropdown[,] at all" — the Line/Dotted/Dashed segmented
        // buttons become a chevron dropdown field, sharing the row with
        // Width the same way Padding/Corner Radius do above), instead of
        // Width on its own row and three full-width buttons underneath.
        Row {
            width: parent.width
            spacing: 12

            NumberField {
                width: (parent.width - 12) / 2
                label: qsTr("Width")
                value: root.primary ? root.primary.borderWidth : 2
                step: 1
                decimals: 0
                minValue: 0
                maxValue: 12
                enabled: root.borderEnabled && root.fillSupported
                showSlider: true
                onMoved: (v) => {
                    root.notifyUndo()
                    root.targets.forEach((t) => t.borderWidth = v)
                }
            }

            FieldBox {
                id: borderStyleField
                width: (parent.width - 12) / 2
                enabled: root.borderEnabled && root.fillSupported
                opacity: (root.borderEnabled && root.fillSupported) ? 1 : 0.4
                Behavior on opacity { NumberAnimation { duration: 100 } }
                label: qsTr("Style")
                value: root.borderStyleLabel
                chevron: true
                onTapped: root.changeBorderStyleRequested()
            }
        }
    }
}
