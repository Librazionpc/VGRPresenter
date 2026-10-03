import QtQuick

// The Edit screen's "Size & Style" collapsible panel: a Padding slider and
// a Corner Radius slider (both always active — radius is an INDEPENDENT
// control here, not a border setting: it rounds the box itself, so you can
// round a box with the border off), then a Border section (color row, Width,
// and a Line/Dotted/Dashed style selector). Item background fill is not a
// control here — the
// existing top-of-panel "Background" row (EditScreen.qml) already picks a
// color; it targets the selected item's background when something's
// selected and the slide's background otherwise, rather than this card
// duplicating that same swatch+chip UI a second time.
// 1BBTIwaya/VGRPresenter_Settings_Outputs_Edit.qml ships
// this only as a flat PNG (style_dialog.png/modal_9.png), so it's rebuilt
// here as real QML from the reference screenshot, model-driven where it
// matters (borderStyle options) so it stays a self-contained, reusable card
// rather than living inline in EditScreen.qml.
//
// The card edits CanvasItemStyle value objects directly: point `targets` at
// the style object(s) of the currently selected canvas item(s) and every
// control binds to targets[0] and writes to all of them. No mirroring layer,
// no per-field signals — the card is reusable anywhere CanvasItemStyle is.
// An empty array disables the controls.
//
// There is no Opacity slider: item transparency was removed (it only ever
// made sense alongside slide backgrounds; on a text item it either did
// nothing or just faded the text in a confusing way), and nothing here
// needs it. Re-adding a style field later means one property on
// CanvasItemStyle + one row here — nothing else.
//
// Every color reads a Theme token, so a Light choice recolours this card
// with the rest of the app.
Column {
    id: root

    property bool expanded: true

    // The CanvasItemStyle object(s) being edited. [0] drives what the
    // controls display; any change is applied to every target (so a
    // multi-selection moves all of them together, matching how the canvas
    // selection works).
    property list<CanvasItemStyle> targets: []

    readonly property CanvasItemStyle primary: targets.length > 0 ? targets[0] : null
    readonly property bool hasTargets: primary !== null

    // False for a kind whose own content (camera's live preview, the
    // generic media/audio/shape/timer/clock placeholder) fully covers its
    // box with an opaque visual — a background fill or border stroke drawn
    // underneath it would never actually be seen, so the Border section
    // greys out instead of looking active but doing nothing. Corner Radius
    // is unaffected: it also rounds the selection outline/hover chrome,
    // which draws on top of the content and so stays visible regardless.
    property bool fillSupported: true

    // Same reusable undo-notification pattern as TextItemPanel.qml's own
    // undoHook (see its header comment for the full reasoning) — a
    // consumer wires this to its own history/snapshot function; null means
    // no undo support wired up.
    property var undoHook: null
    // Called BEFORE undoHook — hands keyboard focus back to the canvas
    // first, same as clicking to select an item already does (see
    // EditScreen.qml's handleCanvasSelect) and the same as a resize-handle
    // press already does (DraggableCanvasText's onPressed). Without this, a
    // slider drag that starts while a text item is still mid-edit leaves
    // focus on that item's TextEdit — Ctrl+Z/Ctrl+Y are gated on
    // mCanvas.activeFocus specifically (so they can't steal focus away from
    // an in-progress typing session), and that gate would then stay closed
    // for the rest of the session, not just this one drag. A consumer wires
    // this to mCanvas.forceActiveFocus; null means nothing to hand off to.
    property var focusHook: null
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    // Fired when the Border row's "Change" chip is clicked — this card owns
    // no color-picker UI itself; the consumer (EditScreen.qml) reuses its
    // existing BackgroundColorModal instance for both Background and Border.
    signal changeBorderRequested()

    spacing: 14

    Rectangle {
        width: parent.width
        height: 44
        radius: 8
        color: Theme.card

        Row {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Text {
                text: qsTr("Size & Style")
                color: Theme.textPrimary
                font.family: "Segoe UI"
                font.pixelSize: 14
                font.weight: Font.Medium
            }
        }

        IconGlyph {
            x: parent.width - 28
            anchors.verticalCenter: parent.verticalCenter
            name: root.expanded ? "chevronUp" : "chevronDown"
            color: Theme.textMuted
            width: 12; height: 12
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }
    }

    Column {
        width: parent.width
        spacing: 18
        visible: root.expanded
        height: visible ? implicitHeight : 0
        opacity: root.hasTargets ? 1 : 0.4

        LabeledSlider {
            width: parent.width
            label: qsTr("Padding")
            value: root.primary ? root.primary.padding : 0
            minValue: 0
            maxValue: 64
            onDragStarted: root.notifyUndo()
            onMoved: (v) => root.targets.forEach((t) => t.padding = v)
        }

        // Corner Radius — deliberately OUTSIDE the border section below: it
        // rounds the box itself (fill, outline, selection chrome all follow
        // via CanvasItemStyle.cornerRadius), so it works with the border off.
        // Always full-opacity; dimming it with the border would wrongly imply
        // it belongs to the border.
        LabeledSlider {
            width: parent.width
            label: qsTr("Corner Radius")
            value: root.primary ? root.primary.cornerRadius : 0
            minValue: 0
            maxValue: 48
            suffix: "px"
            onDragStarted: root.notifyUndo()
            onMoved: (v) => root.targets.forEach((t) => t.cornerRadius = v)
        }

        // ---- The Border section: ONE box for the whole group -------------
        // The Border toggle row, its Width slider and the Line/Dotted/Dashed
        // selector are one nested section — the border's OWN settings, as
        // opposed to Padding/Corner Radius above, which describe the box
        // itself. They used to be three loose controls with only the toggle
        // row wearing a panel, so nothing said "these three belong together";
        // the box is what says it, and it also makes the border-off dimming
        // read as one section that is currently off rather than three
        // unrelated controls that happen to be grey.
        //
        // The old all-in-one Border row is now the box's transparent hit area
        // (a plain Item) so the row's label/swatch/Change/chevron keep the
        // exact x offsets they share with EditScreen.qml's Background row —
        // the columns line up straight down the card — while the box itself
        // carries the fill.
        Rectangle {
            id: borderSection

            width: parent.width
            // 8 top + the toggle row + 14 + the Width slider + 16 + the
            // selector + 12 bottom. The gaps are the body Column's own 18
            // trimmed slightly: inside one box the group reads as a unit, so
            // it needs less air than three separate rows did.
            height: borderRow.y + borderRow.height + 14 + widthSlider.height + 16 + styleRow.height + 12
            radius: 8
            color: Theme.card

            // The card-level "this kind of item shows no fill at all" dimming
            // lives HERE (not on the row) so the box dims as a whole, exactly
            // the way the old all-in-one row did. The border's own on/off
            // dimming is separate, per control, below.
            opacity: root.fillSupported ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            // The Border row — the box's header: label, the enable checkbox,
            // the color swatch, the "Change" chip.
            Item {
                id: borderRow

                x: 0
                y: 8
                width: parent.width
                height: 46
                enabled: root.fillSupported

                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Border")
                    color: Theme.textPrimary
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                }

                // Enable checkbox — the swatch/Change chip stay visible
                // (so you can still see/prepare a color) but dimmed and inert
                // until this is checked. Like the cluster beside it, it hangs
                // off the row's RIGHT edge rather than an absolute x — the
                // card is width-agnostic (its consumer sizes it), so absolute
                // offsets stop lining up the moment the row gets narrower.
                Rectangle {
                    id: borderCheckbox
                    anchors.right: parent.right
                    anchors.rightMargin: 150
                    anchors.verticalCenter: parent.verticalCenter
                    height: 18
                    width: 18
                    radius: 4
                    color: root.borderEnabled ? Theme.accent : Theme.inset
                    border.color: root.borderEnabled ? Theme.accent : Theme.border
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        visible: root.borderEnabled
                        text: "✓"
                        color: "#ffffff"
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

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 104
                    anchors.verticalCenter: parent.verticalCenter
                    height: 24
                    width: 24
                    opacity: root.borderEnabled ? 1 : 0.4
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 5
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    color: root.primary ? root.primary.borderColor : "#ffffff"
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 26
                    anchors.verticalCenter: parent.verticalCenter
                    height: 24
                    width: 60
                    opacity: root.borderEnabled ? 1 : 0.4
                    border.color: Theme.borderSubtle
                    border.width: 1
                    color: borderChangeArea.containsMouse ? Theme.hoverBg : Theme.inset
                    radius: 12
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Change")
                        color: Theme.textSecondary
                        font.family: "Segoe UI"
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

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.textMuted
                    font.family: "Segoe UI"
                    font.pixelSize: 16
                    text: "›"
                }
            }

            // The border's own settings, inset 14 so they line up with the
            // row's label above and keep clear of the box's edges.
            LabeledSlider {
                id: widthSlider
                x: 14
                y: borderRow.y + borderRow.height + 14
                width: parent.width - 28
                enabled: root.fillSupported
                opacity: (root.borderEnabled && root.fillSupported) ? 1 : 0.4
                label: qsTr("Width")
                value: root.primary ? root.primary.borderWidth : 2
                minValue: 0
                maxValue: 12
                suffix: "px"
                onDragStarted: root.notifyUndo()
                onMoved: (v) => root.targets.forEach((t) => t.borderWidth = v)
                Behavior on opacity { NumberAnimation { duration: 100 } }
            }

            // Line / Dotted / Dashed segmented selector.
            Row {
                id: styleRow
                x: 14
                y: widthSlider.y + widthSlider.height + 16
                width: parent.width - 28
                spacing: 8
                enabled: root.fillSupported
                opacity: (root.borderEnabled && root.fillSupported) ? 1 : 0.4
                Behavior on opacity { NumberAnimation { duration: 100 } }

                Repeater {
                    model: [
                        { key: "line", label: qsTr("Line") },
                        { key: "dotted", label: qsTr("Dotted") },
                        { key: "dashed", label: qsTr("Dashed") }
                    ]
                    delegate: Rectangle {
                        id: styleBtn
                        required property var modelData
                        readonly property bool active: root.borderStyle === styleBtn.modelData.key

                        width: (parent.width - 16) / 3
                        height: 32
                        radius: 8
                        color: styleBtn.active ? Theme.accent : (styleArea.containsMouse ? Theme.hoverBg : Theme.inset)
                        border.color: styleBtn.active ? Theme.accent : Theme.borderSubtle
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: styleBtn.modelData.label
                            color: styleBtn.active ? "#ffffff" : Theme.textSecondary
                            font.family: "Segoe UI"
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }

                        MouseArea {
                            id: styleArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.notifyUndo()
                                root.targets.forEach((t) => t.borderStyle = styleBtn.modelData.key)
                            }
                        }
                    }
                }
            }
        }
    }

    // Shorthands over the primary target so the bindings above stay short.
    // They read through `primary` reactively (primary is a readonly binding,
    // so the whole chain re-evaluates when targets changes).
    readonly property bool borderEnabled: primary ? primary.borderEnabled : false
    readonly property string borderStyle: primary ? primary.borderStyle : "line"
}
