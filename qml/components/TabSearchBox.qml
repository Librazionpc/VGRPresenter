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
    // A tab whose matches live in the pane (The Table) sets this: the floating
    // suggestion popup never opens, no matter what (rows empty + explicit gate).
    property bool popupSuppressed: false
    signal suggestionPicked(int index, var payload)
    signal suggestionHovered(int index, bool hovering)
    signal stepMatch(int delta)
    property bool listMode: false
    signal stepColumn(int delta)
    // Esc: the search is abandoned, not just emptied (see the key's own note).
    signal dismissed()

    signal edited(string text)
    signal accepted(string text)

    readonly property int minWidth: 150
    // The seeded line asks for more room than a tab name does; capped so the tabs
    // keep their row on a narrow dock (the tab bar's own clamp is the harder one).
    readonly property int seedMaxWidth: 330
    implicitWidth: Math.max(minWidth, glass.x + glass.width + 12
                                      + Math.min(Math.max(hint.implicitWidth, input.contentWidth),
                                                 seedLine !== "" ? seedMaxWidth - 38 : 1e9) + 26)
    implicitHeight: 31

    function showSuggestions(items) {
        if (sugPanel && items.length === 0) {
            sugPanel.visible = false
            return
        }
        if (items.length === 0)
            return
        if (root.popupSuppressed)
            return
        if (!sugPanel)
            sugPanel = sugComp.createObject(root)
        sugPanel.model = items
        // Wider than the box: sermon titles must read in full ("Exhortation Of ...").
        sugPanel.width = Math.max(root.width, 430)
        sugPanel.maxHeight = Math.min(342, items.length * 34 + 16)
        sugPanel.highlightedIndex = -1
        root.sugOpenedAt = Date.now()
        sugPanel.openAt(root, 0, root.height + 4, root.Window.contentItem)
    }
    // Re-asks the provider for the current text — the refresh everything calls.
    function refreshSuggestions() {
        if (!root.suggestions || root.text === "") {
            if (sugPanel)
                sugPanel.visible = false
            return
        }
        const res = root.suggestions(root.text)
        const items = Array.isArray(res) ? res : (res && res.rows) || []
        const completion = Array.isArray(res) ? "" : ((res && res.complete) || "")
        root.showSuggestions(items)
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
            freezeTimer.restart()
            // The committed text usually resolves — re-ask once so the popup closes
            // (a resolved reference lists no rows) instead of lingering stale.
            root.refreshSuggestions()
        }
    }
    // The freeze guard's state: the completed box content letters are frozen against
    // until a digit or backspace ("" = not frozen). FreeShow's 1.5s timeout applies.
    property string frozenText: ""
    Timer {
        id: freezeTimer
        interval: 1500
        onTriggered: root.frozenText = ""
    }
    property bool suppressing: false
    // The LAST textEdited was an erasure (Backspace/Delete): do not re-complete\r
    // the text the user is deleting.\r
    property bool lastEditShrank: false
    function resetText(value) {
        closeSuggestions()
        root.frozenText = ""
        root.lastEditShrank = false
        root.text = value
        input.text = value
        input.cursorPosition = value.length
        if (value === "" && input.activeFocus)
            root.applySeed()
    }
    function closeSuggestions() {
        if (sugPanel)
            sugPanel.visible = false
    }
    function applySeed() {
        const line = root.seedLine
        if (line === "" || input.text !== "")
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
            || (sugPanel !== null && sugPanel.visible && AppCursor.hovered(sugPanel))
    }
    Connections {
        target: AppCursor
        function onPointerMoved() { root.updatePointerInArea() }
    }
    // Freshly shown under a stationary pointer (a tab switch back) must recompute
    // at once rather than wait for a move.
    onVisibleChanged: visible ? root.updatePointerInArea() : root.pointerInArea = false
    property Item sugPanel: null
    // The live popup carries the self-test name so the UI harness can grab it.
    onSugPanelChanged: if (sugPanel) sugPanel.objectName = "selfTestSuggestPopup"
    // When the outside-click catcher closed the popup (its press precedes this MouseArea's
    // clicked), the toggle below must not instantly re-open it — remembered open time.
    property real sugOpenedAt: 0
    Component {
        id: sugComp
        DropdownPanel {
            dismissOnOutsideClick: true
            onItemHovered: (label, hovering, rowItem, index) => {
                root.suggestionHovered(index, hovering)
            }
            onItemPicked: (index, payload) => {
                visible = false
                input.focus = true
                if (payload && payload.cite &&
                        root.text.trim().toLowerCase() !== payload.cite.toLowerCase()) {
                    input.text = payload.cite
                    input.cursorPosition = input.text.length
                    root.text = input.text
                    root.edited(input.text)
                }
                root.suggestionPicked(index, payload)
            }
            onItemActivated: (label) => {
                // Clicked a row in menu mode (no payload consumers): keep behaving as before.
                visible = false
                input.focus = true
            }
        }
    }

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
                if (/\d$/.test(text) || text.length < root.frozenText.length) {
                    root.frozenText = ""
                } else {
                    suppressing = true
                    input.text = root.frozenText
                    input.cursorPosition = input.text.length
                    suppressing = false
                    return
                }
            }
            root.text = text
            root.edited(text)
            Qt.callLater(root.refreshSuggestions)
        }
        Keys.onUpPressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.highlightedIndex = Math.max(0, sugPanel.highlightedIndex - 1)
                k.accepted = true
            } else if (root.popupSuppressed) {
                // (the walk lives in the pane; the caret must not jump to the ends)
                k.accepted = true
                root.stepMatch(-1)
            }
        }
        Keys.onDownPressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.highlightedIndex = Math.min(sugPanel.model.length - 1, sugPanel.highlightedIndex + 1)
                k.accepted = true
            } else if (root.popupSuppressed) {
                k.accepted = true
                root.stepMatch(1)
            }
        }
        Keys.onLeftPressed: (k) => {
            if (root.listMode) {
                k.accepted = true
                root.stepColumn(-1)
            }
        }
        Keys.onRightPressed: (k) => {
            if (root.listMode) {
                k.accepted = true
                root.stepColumn(1)
            }
        }
        Keys.onReturnPressed: (k) => {
            if (sugPanel && sugPanel.visible && sugPanel.highlightedIndex >= 0) {
                k.accepted = true
                sugPanel.itemPicked(sugPanel.highlightedIndex, sugPanel.model[sugPanel.highlightedIndex].payload)
                return
            }
            // Plain Enter: the box's own text is the pick (see `accepted`).
            k.accepted = true
            root.accepted(root.text)
        }
        Keys.onEscapePressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.visible = false
                k.accepted = true
                return
            }
            k.accepted = true
            root.dismissed()
            input.clear()
            root.frozenText = ""
            root.lastEditShrank = false
            root.text = ""
            root.edited("")
        }
        onActiveFocusChanged: {
            if (!activeFocus)
                return
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
            if (root.suggestions && root.text !== "") {
                if (sugPanel && sugPanel.visible) {
                    sugPanel.visible = false   // catcher saw the press, popup already closing
                } else if (Date.now() - root.sugOpenedAt > 350) {
                    // (a catcher-closed popup lands here within milliseconds — stays closed)
                    root.refreshSuggestions()
                }
            }
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
            onClicked: { input.clear(); root.frozenText = ""; root.lastEditShrank = false; root.text = ""; root.edited(""); input.forceActiveFocus() }
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