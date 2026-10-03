import QtQuick
import VGRPresenterUI

// The search at the right end of the library dock's tab bar, drawn like one of the tabs: a magnifier,
// the word "Search" and the tab bar's red underline. It is tied to the ACTIVE tab: the tab bar keeps one
// query per tab, so Shows filters the shows, Media the media, and coming back to a tab finds its search
// as you left it. This component is just the box - it shows `text`, reports what the user types through
// `edited`, and clears itself on Esc or the x.
Item {
    id: root

    property string text: ""
    // Shown while empty and idle ("Search"); while the box has focus it says what it will search.
    property string placeholder: qsTr("Search")
    property string focusedPlaceholder: ""

    // Live autocomplete (Scripture/The Table's reference suggestions): `suggestions` is a
    // provider FUNCTION (text) => { rows: [{ label, detail, payload }], complete: "" } —
    // called fresh on every edit. `rows` render in the popup; `complete` is the inline
    // autofill tail ("sis " for "gene") appended INTO the box as selected text —
    // FreeShow's replace-the-value autofill. A special complete marker ": " is inserted
    // committed (no selection) so the next digits type the verse. Null provider = no
    // autocomplete (plain filter tabs), popup never built.
    property var suggestions: null
    // A tab whose matches live in the pane (The Table) sets this: the floating
    // suggestion popup never opens, no matter what (rows empty + explicit gate).
    property bool popupSuppressed: false
    signal suggestionPicked(int index, var payload)
    // A row is hovered / un-hovered (index = its position in the popup's model;
    // hovering=false on exit). The tab bar relays it to the pane that offered the
    // suggestions, which previews the row's content (The Table: the sermon's
    // paragraphs) until the pointer leaves.
    signal suggestionHovered(int index, bool hovering)

    signal edited(string text)

    // Hugs its own content — placeholder OR the typed text, whichever is wider
    // (was placeholder-only: a completed citation ("47-1102 - The Angel Of God
    // 1 7") ran past the visible box and the user could not see what they had
    // entered). LibraryTabBar still clamps it so it can't run into the tabs.
    implicitWidth: Math.max(112, glass.x + glass.width + 12
                            + Math.max(hint.implicitWidth, input.contentWidth) + 26)
    implicitHeight: 31

    // The popup lives at the WINDOW ROOT (via DropdownPanel's own reparenting) so the
    // clipped, tab-bar-row containers can't cut it — it overlays the pane below. Anchored to
    // the box's left-bottom; moved on resize/layout changes like any DropdownPanel menu.
    function showSuggestions(items) {
        if (sugPanel && items.length === 0) {
            sugPanel.visible = false
            return
        }
        if (items.length === 0)
            return
        // THE TABLE: rows never come from there (the matches render IN THE PANE —
        // user call: the floating popup covered the search box and the hover
        // preview), so the popup stays shut even when old code paths try to open
        // it (click-to-browse, refocus-with-text, a stale non-empty result).
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
        // FreeShow's inline autofill (their Scripture input sets searchValue =
        // result.autocompleted): the completed word is COMMITTED into the box with the
        // caret at the end — no selection, no ghost. "est" -> "esther |"; the next
        // keystroke is the chapter digit (the auto-colon then makes the verse digits
        // follow a colon). Letters typed right after a commit are FROZEN OUT (the
        // freeze guard below); a digit or a backspace unfreezes instantly.
        if (completion !== "" && completion !== ": " && completion.trim() !== "" && root.text !== "" &&
                !root.lastEditShrank &&
                input.selectionStart === input.text.length && input.selectionEnd === input.text.length) {
 suppressing = true
            input.text = root.text + completion
            input.cursorPosition = input.text.length
            suppressing = false
            root.text = input.text
            root.edited(input.text)
            // Freeze guard (FreeShow's freezeInput): for a moment after the commit,
            // further LETTERS do nothing (the word is already whole); typing a digit
            // (the chapter) or backspacing unfreezes immediately.
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
    // Guards the programmatic writes above: setting `input.text` fires TextEdit's own
    // textEdited, which would re-enter edited()/refreshSuggestions() and fight the
    // selection the autofill just made.
    property bool suppressing: false
    // The LAST textEdited was an erasure (backspace/delete): the autofill must never
    // re-complete what the user is deleting. Without this, backspacing "genesis 1: "
    // -> "genesis 1" re-answered the provider (book + valid chapter => ": ") and the
    // auto-colon re-inserted the colon instantly — the box was stuck and could never
    // be emptied by deleting. Same for the word commit ("genesi" re-completing "s ").
    property bool lastEditShrank: false
    // The tab bar pushes the active tab's stored query back in here on a tab switch
    // (this replaces the old `text: root.text` binding on `input`, which the autofill's
    // programmatic writes would silently break — after which switching tabs stopped
    // restoring that tab's query).
    function resetText(value) {
        closeSuggestions()
        root.frozenText = ""
        root.lastEditShrank = false
        root.text = value
        input.text = value
        input.cursorPosition = value.length
    }
    function closeSuggestions() {
        if (sugPanel)
            sugPanel.visible = false
    }
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
                // A sermon row carries its full citation line ("47-0412 - Faith Is The
                // Substance"): the box ends up naming the sermon the user actually
                // picked — a multi-match search ("47-11" -> the year's 11xx sermons)
                // doesn't leave half a needle standing when the needle is a PREFIX of
                // the citation ("faith" keeps its own text; only "47-041" -> full line).
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

    readonly property string hintText: input.activeFocus && root.focusedPlaceholder !== "" ? root.focusedPlaceholder : root.placeholder
    Text {
        id: hint
        visible: false   // measurement-only twin; the real placeholder is inside `input`, drawn with the same font
        text: root.hintText
        font: input.font
    }
    // (The old ghost-overlay Text is gone: the completion now lives IN the box as
    // selected text — FreeShow's replace-the-value autofill — so there is nothing
    // to paint beside the caret.)

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
        anchors.right: clearButton.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: 15
        font.weight: Font.DemiBold
        clip: true
        selectByMouse: true
        // The box is the source of truth while typing; root.text mirrors it imperatively.
        // (A `text: root.text` binding here was broken by the autofill's first programmatic
        // write — QML drops bindings on imperative assignment — so it is gone.)
        Component.onCompleted: input.text = root.text
        onTextEdited: {
            if (root.suppressing)
                return   // the autofill's own programmatic write, not a user keystroke
            // Grew = a character was ADDED (typing); shrank = erased. root.text still
            // holds the pre-edit content here (it is assigned below).
            const grew = text.length > root.text.length
            root.lastEditShrank = !grew
            // Backspacing out of an auto-inserted ": ": the lone space deletion is
            // invisible ("genesis 1: " looks stuck) and the next press would hit the
            // re-colon — so erase the whole marker in ONE press ("genesis 1: " -> "genesis 1").
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
            // Freeze guard (FreeShow's): while a committed completion is standing,
            // more LETTERS are ignored (the word is already whole) — the box snaps
            // back to the completed word. A digit (the chapter!) or a shrink
            // (backspace) unfreezes and the keystroke goes through.
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
            // Auto-colon (FreeShow): the moment the text resolves to book + valid chapter
            // the provider asks for ": " — insert it committed (no selection; the next
            // digits type the verse straight through).
            const res = root.suggestions ? root.suggestions(text) : null
            if (grew && res && !Array.isArray(res) && res.complete === ": ") {
                suppressing = true
                input.text = text + ": "
                input.cursorPosition = input.text.length
                suppressing = false
                root.text = input.text
                root.edited(input.text)
                root.refreshSuggestions()
                return
            }
            root.edited(text)
            // Deferred one loop turn: Qt emits textEdited BEFORE the cursor settles
            // at the insert position, and the commit below only fires when the caret
            // is at the end (typing off the completion). Called synchronously, the
            // cursor check always saw the stale pre-insert position and the commit
            // never ran — the exact bug the self-test trace exposed (c5/c6 false).
            Qt.callLater(root.refreshSuggestions)
        }
        // Autocomplete keys: Up/Down highlight, Enter accepts the highlighted row (plain
        // Enter — no highlight — falls through to the tab's own onAccepted search), Esc
        // closes the list first and only clears on a second press. The inline completion
        // needs no keys: it is selected text — typing replaces it, backspace shrinks the
        // typed part, Enter commits the box as-is (the selected tail is display-only
        // filler; the pane's resolver reads the TYPED prefix, and onAccepted's resolve
        // clears the tail when the box trims to the same reference).
        Keys.onUpPressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.highlightedIndex = Math.max(0, sugPanel.highlightedIndex - 1)
                k.accepted = true
            }
        }
        Keys.onDownPressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.highlightedIndex = Math.min(sugPanel.model.length - 1, sugPanel.highlightedIndex + 1)
                k.accepted = true
            }
        }
        Keys.onReturnPressed: (k) => {
            if (sugPanel && sugPanel.visible && sugPanel.highlightedIndex >= 0) {
                k.accepted = true
                sugPanel.itemPicked(sugPanel.highlightedIndex, sugPanel.model[sugPanel.highlightedIndex].payload)
            }
        }
        Keys.onEscapePressed: (k) => {
            if (sugPanel && sugPanel.visible) {
                sugPanel.visible = false
                k.accepted = true
                return
            }
            input.clear()
            root.frozenText = ""
            root.lastEditShrank = false
            root.text = ""
            root.edited("")
        }
        // Refocus with text already in re-opens the list (click back into the box).
        onActiveFocusChanged: if (activeFocus)
            root.refreshSuggestions()

        Text {
            visible: input.text.length === 0
            anchors.verticalCenter: parent.verticalCenter
            text: input.activeFocus && root.focusedPlaceholder !== "" ? root.focusedPlaceholder : root.placeholder
            color: Theme.textPrimary
            font: input.font
        }
    }

    // Anywhere on the box starts typing. In autocomplete mode (a tab with `suggestions`)
    // the click toggles: popup open → close it and let the click pass, closed → open it
    // (with the current text) — re-clicking the field browses its suggestions again.
    MouseArea {
        id: boxClick
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        onClicked: {
            input.forceActiveFocus()
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
