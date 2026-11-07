import QtQuick
import VGRPresenterUI

Item {
    id: root

    property string text: ""
    property string placeholder: qsTr("Search")

    // ---- The seed: the passage the pane is on --------------------------------
    property string seedText: ""
    property bool seedFollows: false
    // The seed line as it currently stands ("" when there is nothing on screen yet,
    // which falls back to the plain hint below).
    readonly property string seedLine: seedFollows ? seedText : ""
    // What the box shows while its text is empty: the passage, else the tab hint.
    readonly property string emptyHint: seedLine !== "" ? seedLine : placeholder

    property var suggestions: null
    // A tab whose matches live in the pane (The Table) sets this: Up/Down walk those matches.
    property bool matchesInPane: false
    // Set by the host: (text) => the text with its missing verse filled in, or "".
    property var finishText: null
    signal stepMatch(int delta)
    property bool listMode: false
    signal stepColumn(int delta)
    // Esc: the search is abandoned, not just emptied (see the key's own note).
    signal dismissed()

    signal edited(string text)
    signal accepted(string text)

    // True while the field has the keyboard: the box then OPENS UP (wider than its idle size,
    // over the tabs beside it if it must) so the whole line is visible and the caret can travel
    // along it, instead of showing the clipped tail of a long reference.
    readonly property bool editing: input.activeFocus
    readonly property int editMinWidth: 380

    readonly property int minWidth: 150
    // The seeded line asks for more room than a tab name does; capped so the tabs
    // keep their row on a narrow dock (the tab bar's own clamp is the harder one).
    readonly property int seedMaxWidth: 330
    implicitWidth: Math.max(minWidth, glass.x + glass.width + 12
                                      + Math.min(Math.max(hint.implicitWidth, input.contentWidth),
                                                 (seedLine !== "" && !editing) ? seedMaxWidth - 38 : 1e9) + 26)
    implicitHeight: 31

    // Re-asks the provider for the current text and applies its inline COMPLETION (a unique
    // sermon title finishing itself). There is no dropdown: the box only finds a place, it
    // never lists matches under itself.
    function refreshSuggestions() {
        if (!root.suggestions || root.text === "")
            return
        const res = root.suggestions(root.text)
        const completion = Array.isArray(res) ? "" : ((res && res.complete) || "")
        // A CORRECTION: the provider names the proper spelling of what is typed ("1joh" ->
        // "1 John "), and the box takes it as its text.
        const correction = Array.isArray(res) ? "" : ((res && res.replace) || "")
        if (correction !== "" && correction !== root.text && root.text !== "" &&
                !root.lastEditShrank &&
                input.selectionStart === input.text.length && input.selectionEnd === input.text.length) {
            suppressing = true
            const selectTail = Number((res && res.replaceSelect) || 0)
            input.text = correction
            input.cursorPosition = input.text.length
            if (selectTail > 0)
                input.select(input.text.length - selectTail, input.text.length)
            suppressing = false
            root.text = input.text
            root.edited(input.text)
            root.frozenText = input.text
            root.frozenSelTail = selectTail
            freezeTimer.restart()
            return
        }
        if (completion !== "" && completion.trim() !== "" && root.text !== "" &&
                !root.lastEditShrank &&
                input.selectionStart === input.text.length && input.selectionEnd === input.text.length) {
            suppressing = true
            input.text = root.text + completion
            input.cursorPosition = input.text.length
            suppressing = false
            root.text = input.text
            root.edited(input.text)
            root.frozenText = input.text
            root.frozenSelTail = 0
            freezeTimer.restart()
        }
    }
    // The freeze guard's state: the completed box content letters are frozen against
    // until a digit or backspace ("" = not frozen). FreeShow's 1.5s timeout applies.
    property string frozenText: ""
    // How many trailing characters of frozenText are a SELECTED suggestion (the chapter digit of
    // "1 John 1"): a letter typed over them is a keystroke for the word before, not a new chapter.
    property int frozenSelTail: 0
    Timer {
        id: freezeTimer
        interval: 1500
        onTriggered: { root.frozenText = ""; root.frozenSelTail = 0 }
    }
    property bool suppressing: false
    // The LAST textEdited was an erasure (Backspace/Delete): do not re-complete\r
    // the text the user is deleting.\r
    property bool lastEditShrank: false
    function resetText(value) {
        root.userCleared = false
        root.frozenText = ""
        root.lastEditShrank = false
        root.text = value
        input.text = value
        input.cursorPosition = value.length
        if (value === "" && input.activeFocus)
            root.applySeed()
    }
    // The operator emptied the box themself (Backspace / the X / Esc): the passage line must
    // NOT come back on its own when the pane next moves — that read as "Backspace does not
    // clear it". It fills again when the box is clicked or focused afresh, or the tab changes.
    property bool userCleared: false
    function applySeed() {
        const line = root.seedLine
        if (line === "" || input.text !== "" || root.userCleared)
            return false
        root.frozenText = ""
        root.lastEditShrank = false
        root.seededLine = line
        root.text = line
        input.text = line
        input.cursorPosition = line.length
        return true
    }
    property string seededLine: ""
    onSeedTextChanged: {
        // Read the NEW line from seedText itself (not seedLine, whose binding may still
        // be settling in this handler) and only when the seed still follows the pane.
        const next = root.seedFollows ? root.seedText : ""
        if (next === "")
            return
        if (root.seededLine !== "" && input.text === root.seededLine) {
            root.seededLine = next
            root.resetText(next)
            return
        }
        if (input.text === "" && input.activeFocus)
            root.applySeed()
    }

    // ---- pointer tracking ----------------------------------------------------
    // Pointer exit updates hover state only. Keep keyboard focus and the query so
    // typing continues to go to this field while the pointer is over the results.
    property bool pointerInArea: false
    function updatePointerInArea() {
        if (!visible || !AppCursor.windowHovered) {
            root.pointerInArea = false
            return
        }
        root.pointerInArea = AppCursor.hovered(root)
    }
    Connections {
        target: AppCursor
        function onPointerMoved() { root.updatePointerInArea() }
    }
    // Freshly shown under a stationary pointer (a tab switch back) must recompute
    // at once rather than wait for a move.
    onVisibleChanged: visible ? root.updatePointerInArea() : root.pointerInArea = false
    Text {
        id: hint
        visible: false   // measurement only; the real hint is drawn inside `input`
        text: root.emptyHint
        font: input.font
    }

    IconGlyph {
        id: glass
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        // The stock magnifier is a 10.5 px glyph; enlarged (with its line kept at 2 px) to sit with the tabs.
        readonly property real enlarge: 1.45
        width: 10.5; height: 10.5
        scale: enlarge
        strokeWidth: 2 / enlarge
        name: "search"
        color: Theme.textPrimary
    }

    TextInput {
        id: input
        objectName: "selfTestSearchInput"   // the harness focuses THIS (forceActiveFocus on the container Item doesn't reach here)
        anchors.left: glass.right
        anchors.leftMargin: 12
        anchors.right: clearButton.visible ? clearButton.left : parent.right
        anchors.rightMargin: clearButton.visible ? 4 : 8
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: 15
        font.weight: Font.DemiBold
        clip: true
        selectByMouse: true
        Component.onCompleted: input.text = root.text
        onTextEdited: {
            // The operator's own text now: the seed must never replace it.
            root.seededLine = ""
            root.userCleared = (text === "")
            if (root.suppressing)
                return   // the autofill's own programmatic write, not a user keystroke
            // Grew = a character was ADDED (typing); shrank = erased. root.text still
            // holds the pre-edit content here (it is assigned below).
            const grew = text.length > root.text.length
            root.lastEditShrank = !grew
            // Backspace on the trailing space of a seeded "Book chapter: " reference
            // removes the colon and space together so the separator is easy to edit.
            if (!grew && root.text.endsWith(": ") && text === root.text.slice(0, -1)) {
                suppressing = true
                input.text = root.text.slice(0, -2)
                input.cursorPosition = input.text.length
                suppressing = false
                text = input.text
                root.text = text
                root.edited(text)
                Qt.callLater(root.refreshSuggestions)
                return
            }
            // The committed completion ends with a space ("esther "), so the user's
            // own space keystroke would make two — swallow a double space here.
            if (text.endsWith("  ")) {
                suppressing = true
                input.text = text.slice(0, -1)
                input.cursorPosition = input.text.length
                suppressing = false
                text = input.text
            }
            if (root.frozenText !== "") {
                // A letter typed over the selected chapter digit ("1 John 1" + "n") is the next
                // letter of the book, not a replacement chapter: it is dropped like any other
                // letter typed against a fresh completion.
                const tail = root.frozenSelTail
                const base = root.frozenText.slice(0, root.frozenText.length - tail)
                const extra = (tail > 0 && text.startsWith(base)) ? text.slice(base.length) : null
                const letterOverTail = extra !== null && /^[A-Za-z]+$/.test(extra)
                if (!letterOverTail && (/\d$/.test(text) || text.length < root.frozenText.length)) {
                    root.frozenText = ""
                    root.frozenSelTail = 0
                } else {
                    suppressing = true
                    input.text = root.frozenText
                    input.cursorPosition = input.text.length
                    if (tail > 0)
                        input.select(input.text.length - tail, input.text.length)
                    suppressing = false
                    return
                }
            }
            root.text = text
            root.edited(text)
            Qt.callLater(root.refreshSuggestions)
        }
        Keys.onUpPressed: (k) => {
            if (root.matchesInPane) {
                // (the walk lives in the pane; the caret must not jump to the ends)
                k.accepted = true
                root.stepMatch(-1)
            }
        }
        Keys.onDownPressed: (k) => {
            if (root.matchesInPane) {
                k.accepted = true
                root.stepMatch(1)
            }
        }
        // The caret keys belong to the text. While the result list is up they hand over to
        // its columns ONLY once the caret has nowhere left to go in that direction (empty
        // box, or already at that end) — never while there is text to move through, which
        // is what made the box feel frozen to the keyboard.
        Keys.onLeftPressed: (k) => {
            if (root.listMode && input.selectionStart === input.selectionEnd
                    && input.cursorPosition === 0) {
                k.accepted = true
                root.stepColumn(-1)
            } else {
                k.accepted = false   // the input moves the caret / collapses the selection
            }
        }
        Keys.onRightPressed: (k) => {
            if (root.listMode && input.selectionStart === input.selectionEnd
                    && input.cursorPosition === input.text.length) {
                k.accepted = true
                root.stepColumn(1)
            } else {
                k.accepted = false
            }
        }
        Keys.onReturnPressed: (k) => {
            k.accepted = true
            // A chapter with no verse is finished first ("1 John 1" -> "1 John 1:1", and the pane
            // lands on it); the next Enter is then the pick.
            const filled = root.finishText ? root.finishText(root.text) : ""
            if (filled !== "" && filled !== root.text) {
                suppressing = true
                input.text = filled
                input.cursorPosition = input.text.length
                suppressing = false
                root.userCleared = false
                root.text = input.text
                root.frozenText = ""
                root.frozenSelTail = 0
                root.edited(input.text)
                return
            }
            // Plain Enter: the box's own text is the pick (see `accepted`).
            root.accepted(root.text)
        }
        Keys.onEscapePressed: (k) => {
            k.accepted = true
            root.dismissed()
            input.clear()
            root.userCleared = true
            root.frozenText = ""
            root.lastEditShrank = false
            root.text = ""
            root.edited("")
        }
        onActiveFocusChanged: {
            if (!activeFocus)
                return
            root.userCleared = false   // a fresh focus fills the passage line as it always did
            root.applySeed()
            root.refreshSuggestions()
        }

        Text {
            visible: input.text.length === 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.rightMargin: seedLine !== "" ? 8 : 0
            text: root.emptyHint
            color: seedLine !== "" ? Theme.textSecondary : Theme.textPrimary
            font: input.font
            textFormat: Text.PlainText
            elide: seedLine !== "" ? Text.ElideRight : Text.ElideNone
            clip: seedLine !== ""
        }
    }

    MouseArea {
        id: boxClick
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        onClicked: {
            input.forceActiveFocus()
            root.applySeed()
        }
        z: -1
    }

    // Clear.
    Rectangle {
        id: clearButton
        visible: input.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18; radius: 9
        color: clearArea.pressed ? Theme.hoverBg : "transparent"
        IconGlyph {
            anchors.centerIn: parent
            name: "close"
            color: Theme.textSecondary
            width: 9; height: 9
        }
        MouseArea {
            id: clearArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { input.clear(); root.userCleared = true; root.frozenText = ""; root.lastEditShrank = false; root.text = ""; root.edited(""); input.forceActiveFocus() }
        }
    }

    // The tab bar's red underline, the same as the selected tab's.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 2
        radius: 1
        color: Theme.accent
    }
}