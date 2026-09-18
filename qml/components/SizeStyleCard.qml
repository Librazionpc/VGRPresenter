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
// Literal colors, not Theme.* — same house convention as DropdownPanel.qml/
// LabeledSlider.qml at this nesting depth.
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
    function notifyUndo() {
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
        color: "#161823"

        Row {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Text {
                text: qsTr("Size & Style")
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
                font.weight: Font.Medium
            }
        }

        Text {
            x: parent.width - 28
            anchors.verticalCenter: parent.verticalCenter
            text: root.expanded ? "⌃" : "⌄"
            color: "#6b7280"
            font.pixelSize: 12
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

        // Border color row — same card language as EditScreen.qml's
        // Background row (swatch + "Change" chip at the same x offsets,
        // assuming the same 384px row width).
        Rectangle {
            width: parent.width
            height: 46
            radius: 8
            color: "#161823"
            enabled: root.fillSupported
            opacity: root.fillSupported ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Border")
                color: "#eef1f8"
                font.family: "Inter"
                font.pixelSize: 12
            }

            // Enable checkbox — the swatch/Change chip below stay visible
            // (so you can still see/prepare a color) but dimmed and inert
            // until this is checked.
            Rectangle {
                id: borderCheckbox
                x: 216
                anchors.verticalCenter: parent.verticalCenter
                height: 18
                width: 18
                radius: 4
                color: root.borderEnabled ? "#6c5ce7" : "#1a1c26"
                border.color: root.borderEnabled ? "#6c5ce7" : "#3a4050"
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    visible: root.borderEnabled
                    text: "✓"
                    color: "#ffffff"
                    font.pixelSize: 11
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
                x: 256
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: "#3a4a7a"
                border.width: 1
                radius: 5
                Behavior on opacity { NumberAnimation { duration: 100 } }
                color: root.primary ? root.primary.borderColor : "#ffffff"
            }

            Rectangle {
                x: 298
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                opacity: root.borderEnabled ? 1 : 0.4
                border.color: "#2a3140"
                border.width: 1
                color: borderChangeArea.containsMouse ? "#20242f" : "#1a1c26"
                radius: 12
                Behavior on color { ColorAnimation { duration: 100 } }
                Behavior on opacity { NumberAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Change")
                    color: "#aeb6c8"
                    font.family: "Inter"
                    font.pixelSize: 10
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
                x: 368
                anchors.verticalCenter: parent.verticalCenter
                color: "#6b7280"
                font.family: "Inter"
                font.pixelSize: 14
                text: "›"
            }
        }

        LabeledSlider {
            width: parent.width
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
            width: parent.width
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
                    color: styleBtn.active ? "#6c5ce7" : (styleArea.containsMouse ? "#20222c" : "#1a1c26")
                    border.color: styleBtn.active ? "#6c5ce7" : "#2a3140"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: styleBtn.modelData.label
                        color: styleBtn.active ? "#ffffff" : "#aeb6c8"
                        font.family: "Inter"
                        font.pixelSize: 11
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

    // Shorthands over the primary target so the bindings above stay short.
    // They read through `primary` reactively (primary is a readonly binding,
    // so the whole chain re-evaluates when targets changes).
    readonly property bool borderEnabled: primary ? primary.borderEnabled : false
    readonly property string borderStyle: primary ? primary.borderStyle : "line"
}
