import QtQuick
import VGRPresenterUI

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
    // Fired when the user presses an undo/redo chord (Ctrl+Z / Ctrl+Shift+Z
    // / Ctrl+Y) while editing. A focused TextEdit ACCEPTS the ShortcutOverride
    // for those chords (it's editable), so the EditScreen-level Shortcuts
    // never activate and the TextEdit's own INTERNAL undo would eat them —
    // exactly the "undo doesn't work while the item is in edit mode; it
    // works once I click away" report. The edit forwards the chords out
    // instead (see editInput's Keys.onPressed); the consumer routes them to
    // its real history. Deferred execution is the CONSUMER's job — this
    // signal is emitted from inside the TextEdit's own key-event dispatch,
    // and an undo applies a snapshot that destroys this very delegate.
    signal undoRedoRequested(bool undo)

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

    // TextEdit, not TextInput — TextInput is inherently single-line (it has
    // no representation for an embedded newline at all), so Enter could
    // never insert one; it could only ever end the edit. TextEdit accepts
    // Enter as a real newline like any multi-line editor, matching
    // displayText's own wrapMode-based multi-line rendering.
    TextEdit {
        id: editInput
        anchors.fill: parent
        verticalAlignment: Text.AlignVCenter
        visible: root.editing
        text: displayText.text
        color: displayText.color
        font: displayText.font
        horizontalAlignment: displayText.horizontalAlignment
        wrapMode: displayText.wrapMode
        selectByMouse: true

            // Fixed-color blinking caret — TextEdit draws its cursor using its
            // own `color` (the text color) unless given a cursorDelegate, so
            // without one, changing the text's color also changed the caret's
            // color, making it hard to see (or invisible) against some colors.
            // TextEdit positions this automatically; a CUSTOM delegate is only
            // responsible for its own visual AND its own blink — the built-in
            // automatic blink is specifically a property of the default cursor,
            // not something TextEdit keeps driving once you supply your own.
            cursorDelegate: Rectangle {
                width: 2
                color: "#ffffff"
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    PropertyAnimation { to: 0; duration: 500 }
                    PropertyAnimation { to: 1; duration: 500 }
                }
            }

        // Hover-scoped edit cursor: the I-beam override is only on the
        // stack while the pointer is actually over the field. Pushing it
        // unconditionally for the whole edit session made it stick
        // everywhere after clicking away — a click on a non-focusable
        // panel doesn't steal focus, so `visible` never flipped and the
        // pop never ran (same scale limitation as ever: TextInput's
        // built-in cursor can't apply under the zoom transform).
        function syncEditCursor() {
            // AppCursor.hovered(editInput) — position truth — is the ONLY
            // signal here now. The old editHoverArea.containsMouse
            // disjunct is a trap in this build: hover-exit never delivers
            // to MouseAreas (KNOWN_ISSUES.md), so containsMouse latches
            // true and the I-beam stuck after leaving the field. The
            // catcher's pointer-position stream covers both directions —
            // including the edit-opens-under-a-stationary-pointer case
            // that motivated the original disjunct.
            if (visible && AppCursor.hovered(editInput))
                AppCursor.push(Qt.IBeamCursor, editInput)
            else
                AppCursor.pop(editInput)
        }
        onVisibleChanged: {
            if (visible) {
                preEditText = displayText.text
                forceActiveFocus()
                selectAll()
            }
            syncEditCursor()
        }
        Component.onDestruction: AppCursor.pop(editInput)
        Keys.onEscapePressed: {
            // Restore the pre-edit text before exiting — onEditingFinished
            // (focus loss from becoming invisible) then commits the RESTORED
            // value back through the same path a real edit would, so the
            // model (and its bound thumbnail) revert cleanly.
            text = preEditText
            root.editing = false
        }
        // Intercept the undo/redo chords BEFORE the TextEdit's own key
        // handling (Keys priority is BeforeItem by default): a focused,
        // editable TextEdit accepts the ShortcutOverride for them, so the
        // app-level Shortcuts never fire and the internal QQuickTextEdit
        // undo would otherwise run instead — the canvas history would never
        // see the chord (probe-verified: with focus on a TextEdit, a
        // window-level Ctrl+Z Shortcut fires 0 times). accepted: true both
        // blocks the internal undo and keeps the Shortcut suppressed
        // (ShortcutOverride was accepted by us, the focused item).
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

    // Tracks the pointer over the label while editing (the drag area below
    // hides itself then, so it can't do it) — feeds syncEditCursor above.
    // Declared after editInput so it's topmost. acceptedButtons: NoButton
    // keeps it invisible to clicks — presses still reach editInput below.
    // Trigger is the pointer-POSITION stream, not containsMouse: exit never
    // delivers to MouseAreas in this build (KNOWN_ISSUES.md), so a
    // containsMouse-driven trigger latched and the I-beam stuck after the
    // pointer left the field. Position truth covers both directions, and
    // onVisibleChanged still covers edit-open/close under a stationary
    // pointer.
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

        anchors.fill: parent
        visible: !root.editing
        hoverEnabled: true
        cursorShape: dragArea.dragMoved ? Qt.ClosedHandCursor : Qt.IBeamCursor

        // Cursor via the AppCursor override stack, not cursorShape alone:
        // the canvas sits under mCanvas's scale (zoom) transform, and
        // cursorShape silently stops applying under scaled ancestors while
        // hover delivery (containsMouse) keeps working — so hover IS known
        // here but the OS cursor never changed. Pushing the shape onto the
        // app-wide stack (rendered by Main.qml's unscaled AppCursorCatcher)
        // forces it at any zoom. cursorShape stays as the zoom==1 fallback;
        // the pushed shape is identical so the two can't disagree.
        function syncCursor() {
            // Position truth + pressed. The old containsMouse disjunct is a
            // latch trap: hover-exit never delivers to MouseAreas in this
            // build (KNOWN_ISSUES.md), so containsMouse stuck true and this
            // push outlived the pointer forever. `pressed` still covers the
            // active drag (the grab keeps press alive outside the item); on
            // release, AppCursor.hovered decides from where the pointer IS.
            if (visible && (pressed || AppCursor.hovered(root)))
                AppCursor.push(dragMoved ? Qt.ClosedHandCursor : Qt.IBeamCursor, dragArea)
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
        // While editing, this area hides itself — an invisible MouseArea
        // stops receiving hover events, so containsMouse can stay true with
        // nobody left to clear it. Pop explicitly on hide (syncCursor only
        // re-pushes from live hover/press events, never on restore, so a
        // stale containsMouse can't resurrect the cursor after editing).
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
            syncCursor()
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
                syncCursor() // hand cursor once the drag is live
                root.moveRequested(dx, dy, (mouse.modifiers & Qt.AltModifier) !== 0)
                pressMouseX = g.x
                pressMouseY = g.y
            }
        }
        onReleased: {
            syncCursor()
            root.dragEnded()
        }
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
