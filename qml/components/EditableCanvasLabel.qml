import QtQuick
import VGRPresenterUI

Item {
    id: root

    // The TYPED text - what is edited and what the model keeps. What is SHOWN is this with the list style applied (below).
    property string text: ""
    // The engine's list style for the text ("" / "none" = a plain paragraph, "disc", "decimal" ... = every line is a list
    // item). It only changes what is displayed; the typed text is never touched.
    property string listStyle: ""
    // The text as displayed (list markers included) - for a consumer that has to measure what is on screen.
    readonly property alias shownText: displayText.text
    property alias color: displayText.color
    property alias font: displayText.font
    property alias horizontalAlignment: displayText.horizontalAlignment
    property alias verticalAlignment: displayText.verticalAlignment
    property alias wrapMode: displayText.wrapMode
    property alias lineHeight: displayText.lineHeight
    property alias fontSizeMode: displayText.fontSizeMode
    property alias minimumPixelSize: displayText.minimumPixelSize
    readonly property alias contentWidth: displayText.contentWidth
    readonly property alias contentHeight: displayText.contentHeight
    property string preEditText: ""
    property bool editing: false
    property bool selected: false
    readonly property alias hovered: dragArea.containsMouse

    signal committed(string value)
    signal edited(string value)
    signal selectRequested(var modifiers, real x, real y)
    signal moveRequested(real dx, real dy, bool snapDisabled)
    // Fired on release of a body-drag (whether or not it actually moved),
    // so a consumer can clear its snap-guide/group-drag state.
    signal dragEnded()
    signal undoRedoRequested(bool undo)

    anchors.fill: parent

    // ---- the per-LINE background band (the Lines card's "Background color") --
    property string bandColor: ""
    property string bandCase: "none"
    property real bandRadius: 0

    LineBandLayer {
        anchors.fill: parent
        layout: displayText
        color: root.bandColor
        radius: root.bandRadius
        textCase: root.bandCase
    }

    Text {
        id: displayText
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        visible: !root.editing
        text: TextFormatService.applyList(root.text, root.listStyle)
    }

    TextEdit {
        id: editInput
        anchors.fill: parent
        verticalAlignment: displayText.verticalAlignment
        visible: root.editing
        text: root.text
        color: displayText.color
        font: displayText.font
        horizontalAlignment: displayText.horizontalAlignment
        wrapMode: displayText.wrapMode
        selectByMouse: true
        selectionColor: Theme.accent
        selectedTextColor: Theme.controlInk

            cursorDelegate: Rectangle {
                id: caret
                width: Math.max(1, Math.round(editInput.font.pixelSize / 16))
                color: "#ffffff"
                opacity: editInput.activeFocus && editInput.cursorVisible ? 1 : 0
                SequentialAnimation on opacity {
                    running: editInput.activeFocus
                    loops: Animation.Infinite
                    PropertyAnimation { to: 0; duration: 500 }
                    PropertyAnimation { to: 1; duration: 500 }
                }
            }

        function syncEditCursor() {
            if (visible && AppCursor.hovered(editInput))
                AppCursor.push(Qt.IBeamCursor, editInput)
            else
                AppCursor.pop(editInput)
        }
        onVisibleChanged: {
            if (visible) {
                preEditText = root.text
                forceActiveFocus()
                selectAll()
            }
            syncEditCursor()
        }
        Component.onDestruction: AppCursor.pop(editInput)
        Keys.onEscapePressed: {
            text = preEditText
            root.editing = false
        }
        Keys.onPressed: (e) => {
            const ctrl = (e.modifiers & Qt.ControlModifier) !== 0
            if (!ctrl)
                return
            if (e.key === Qt.Key_Z && (e.modifiers & Qt.ShiftModifier)) {
                e.accepted = true
                root.undoRedoRequested(false)
            } else if (e.key === Qt.Key_Z) {
                e.accepted = true
                root.undoRedoRequested(true)
            } else if (e.key === Qt.Key_Y) {
                e.accepted = true
                root.undoRedoRequested(false)
            }
        }
        // textEdited (not textChanged) — user keystrokes only, so a
        // programmatic write back into the model can't echo back here.
        onTextEdited: root.edited(text)
        onEditingFinished: {
            root.editing = false
            root.committed(text)
        }
    }

    MouseArea {
        id: editHoverArea
        anchors.fill: parent
        visible: root.editing
        hoverEnabled: false
        acceptedButtons: Qt.NoButton
        onVisibleChanged: editInput.syncEditCursor()
        Connections {
            target: AppCursor
            function onPointerMoved() { editInput.syncEditCursor() }
        }
    }

    MouseArea {
        id: dragArea

        property real pressMouseX: 0
        property real pressMouseY: 0
        property bool dragMoved: false
        property bool dragging: false

        anchors.fill: parent
        visible: !root.editing
        hoverEnabled: true
        cursorShape: dragArea.dragging ? Qt.ClosedHandCursor : Qt.IBeamCursor

        function syncCursor() {
            if (visible && (pressed || AppCursor.hovered(root)))
                AppCursor.push((pressed && dragging) ? Qt.ClosedHandCursor : Qt.IBeamCursor,
                               dragArea, pressed ? AppCursor.priorityPressed : AppCursor.priorityHover)
            else
                AppCursor.pop(dragArea)
        }
        // (Position-stream trigger via the Connections below; the drag
        // onPositionChanged handler covers moves while pressed.)
        onEnabledChanged: syncCursor()
        Connections {
            target: AppCursor
            function onPointerMoved() { dragArea.syncCursor() }
        }
        onVisibleChanged: {
            // Reappear (edit ended / zoom flip): re-check with position
            // truth so a stale hover state can't resurrect the override.
            if (visible)
                syncCursor()
            else
                AppCursor.pop(dragArea)
        }
        Component.onDestruction: AppCursor.pop(dragArea)

        onPressed: (mouse) => {
            dragging = true
            dragMoved = false
            syncCursor()
            const g = mapToItem(null, mouse.x, mouse.y)
            pressMouseX = g.x
            pressMouseY = g.y
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            const g = mapToItem(null, mouse.x, mouse.y)
            const dx = g.x - pressMouseX
            const dy = g.y - pressMouseY
            if (dragMoved || Math.abs(dx) > 4 || Math.abs(dy) > 4) {
                dragMoved = true
                syncCursor() // hand cursor once the drag is live
                root.moveRequested(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
                pressMouseX = g.x
                pressMouseY = g.y
            }
        }
        onReleased: {
            dragging = false
            syncCursor()
            root.dragEnded()
        }
        onClicked: (mouse) => {
            if (dragMoved)
                return
            root.forceActiveFocus()
            if (root.selected && !(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))) {
                root.editing = true
                return
            }
            root.selectRequested(mouse.modifiers, mouse.x, mouse.y)
        }
        onDoubleClicked: (mouse) => {
            if (dragMoved)
                return
            if (!(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                root.editing = true
        }
    }
}