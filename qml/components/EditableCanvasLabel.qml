import QtQuick

// A canvas text label: single click selects it (see selectRequested below),
// click-and-drag moves the object it belongs to (see moveRequested below),
// double click enters inline edit — an I-beam cursor while hovering signals
// "click to select or drag, double-click to edit". Editing swaps the label
// for a TextInput pre-filled with the current text, focused and
// select-all'd immediately. Enter or losing focus commits via
// `committed(value)`; Escape cancels and reverts.
//
// Dragging the label body (not just DraggableCanvasText's border ring)
// moves the object — the border ring alone was an easy-to-miss, undiscoverable
// way to reposition something, especially since resizing lives there too and
// moving needed a Shift modifier to disambiguate. Now plain drag-anywhere-
// on-the-text moves it, matching how most editors actually work; the ring
// still handles resize.
Item {
    id: root

    property alias text: displayText.text
    property alias color: displayText.color
    property alias font: displayText.font
    property alias horizontalAlignment: displayText.horizontalAlignment
    property alias wrapMode: displayText.wrapMode
    // Display-only (TextInput has no lineHeight of its own in QtQuick) —
    // an acceptable gap since editing mode is transient and doesn't need
    // pixel-identical line spacing to the committed display.
    property alias lineHeight: displayText.lineHeight
    property alias fontSizeMode: displayText.fontSizeMode
    property alias minimumPixelSize: displayText.minimumPixelSize
    // The text's own natural size at the current font/content, regardless
    // of whatever box it's actually confined to — what a consumer's "grow"
    // auto-size mode needs to resize the box to fit the text (see
    // EditScreen.qml's onContentWidthChanged/onContentHeightChanged at the
    // itemTextLabel instantiation).
    readonly property alias contentWidth: displayText.contentWidth
    readonly property alias contentHeight: displayText.contentHeight
    // The text as it was the moment editing began. Live per-keystroke
    // propagation (edited()) writes each change into the consumer's model
    // as it happens, and displayText follows the model — so by Escape time
    // displayText.text is already the TYPED text, and restoring from it
    // would restore nothing. The pre-edit capture is what makes Escape a
    // real cancel.
    property string preEditText: ""
    property bool editing: false
    // Exposed so the owning DraggableCanvasText can drive its hover-wash
    // highlight from this MouseArea's own hover tracking instead of
    // layering a second, overlapping hoverEnabled MouseArea on top of it
    // (two stacked hover-tracking MouseAreas over the same region is what
    // made click delivery flaky after a hover-leave/hover-enter cycle).
    readonly property alias hovered: dragArea.containsMouse

    signal committed(string value)
    // Fired on EVERY keystroke while editing (not on programmatic text
    // changes) so the consumer can push live text into its data model —
    // which is what makes bound thumbnails/update-watching surfaces update
    // as you type instead of only on commit. `committed` still fires at the
    // end, reconciling the final value.
    signal edited(string value)
    // Fired on every single click (regardless of modifiers) so the
    // consumer can commit this object into its durable selection state
    // (see DraggableCanvasText's selectedRequested / EditScreen.qml's
    // handleCanvasSelect) — not just the ephemeral `editing` flag below.
    // That matters because `editing` can flip back to false mid-gesture
    // (grabbing this object's own resize ring calls forceActiveFocus,
    // which commits/exits an in-progress edit) — if selection depended on
    // `editing` alone, the object would go from selected to unselected
    // right as you started dragging its ring, disabling the very MouseArea
    // handling that drag and aborting it.
    signal selectRequested(var modifiers)
    // Fired repeatedly (in scene-coordinate deltas) while dragging the
    // label body past a small threshold — the consumer (EditScreen.qml)
    // adds these straight onto the owning DraggableCanvasText's x/y (and,
    // for a multi-selection, every other selected object's too), optionally
    // running them through alignment snapping first (snapDisabled is true
    // while Alt is held, matching the resize ring's Alt-to-disable-snap).
    // Suppresses the click/double-click below for that same gesture so a
    // drag never also toggles selection or starts editing.
    signal moveRequested(real dx, real dy, bool snapDisabled)
    // Fired on release of a body-drag (whether or not it actually moved),
    // so a consumer can clear its snap-guide/group-drag state.
    signal dragEnded()

    // Fills whatever box it's placed in (the owning DraggableCanvasText's
    // full content area), not just its own text's natural size — content-
    // driven sizing (the old width/height: displayText.width/height) meant
    // an empty or short text item's interactive MouseArea below collapsed
    // to near-zero height, leaving most of the visible box with nothing
    // clickable in it (exactly why a fresh, still-empty text item wouldn't
    // hover or select while camera/placeholder items, which always fill
    // their box via anchors.fill: parent, worked fine).
    anchors.fill: parent

    Text {
        id: displayText
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        visible: !root.editing
    }

    TextInput {
        id: editInput
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        visible: root.editing
        text: displayText.text
        color: displayText.color
        font: displayText.font
        horizontalAlignment: displayText.horizontalAlignment
        selectByMouse: true

        onVisibleChanged: {
            if (visible) {
                preEditText = displayText.text
                forceActiveFocus()
                selectAll()
            }
        }
        Keys.onEscapePressed: {
            // Restore the pre-edit text before exiting — onEditingFinished
            // (focus loss from becoming invisible) then commits the RESTORED
            // value back through the same path a real edit would, so the
            // model (and its bound thumbnail) revert cleanly.
            text = preEditText
            root.editing = false
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
        id: dragArea

        property real pressMouseX: 0
        property real pressMouseY: 0
        property bool dragMoved: false

        anchors.fill: parent
        visible: !root.editing
        hoverEnabled: true
        cursorShape: dragArea.dragMoved ? Qt.ClosedHandCursor : Qt.IBeamCursor

        onPressed: (mouse) => {
            const g = mapToItem(null, mouse.x, mouse.y)
            pressMouseX = g.x
            pressMouseY = g.y
            dragMoved = false
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            const g = mapToItem(null, mouse.x, mouse.y)
            const dx = g.x - pressMouseX
            const dy = g.y - pressMouseY
            if (dragMoved || Math.abs(dx) > 4 || Math.abs(dy) > 4) {
                dragMoved = true
                root.moveRequested(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
                pressMouseX = g.x
                pressMouseY = g.y
            }
        }
        onReleased: root.dragEnded()
        onClicked: (mouse) => {
            if (dragMoved)
                return
            // Neither branch opens a TextInput anymore (that's
            // onDoubleClicked's job now), so this always has to steal
            // focus itself to commit/exit whatever else might be mid-edit.
            root.forceActiveFocus()
            root.selectRequested(mouse.modifiers)
        }
        onDoubleClicked: (mouse) => {
            if (dragMoved)
                return
            if (!(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                root.editing = true
        }
    }
}
