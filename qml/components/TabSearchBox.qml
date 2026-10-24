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
    // Shown while empty, focused or not, and it NAMES the active tab — "Search
    // Shows", "Search The Table" — supplied by the tab bar. It does not change on
    // focus (the box used to widen on click for that reason); the width is fixed
    // by defaultWidth, so switching tabs never moves the field.
    property string placeholder: qsTr("Search")

    // ---- The seed: the passage the pane is on --------------------------------
    // "Let the search bar by default hold 55-0117 - How The Angel …", so the
    // operator types the paragraph instead of the whole line (the same for
    // Scripture's book and chapter). Two states, and the difference matters:
    //
    //   SEEDED (empty, showing seedText) — the hint IS the passage, but it is not a
    //   query: nothing is filtered and `edited` never fires, so switching tabs costs
    //   no engine work. Clicking the box COMMITS it (that is the one gesture that
    //   says "work from this line"), and the field then takes digits at the end.
    //
    //   TYPED — the operator's own query, from the first keystroke on; the seed only
    //   survives as the prefix they typed over.
    //
    // seedFollows keeps it true as they browse (open another chapter, step a
    // passage) — right up until they type, at which point the query is theirs and
    // is no longer replaced (the tab bar passes `!searches[pane]`).
    property string seedText: ""
    property bool seedFollows: false
    // The seed line as it currently stands ("" when there is nothing on screen yet,
    // which falls back to the plain hint below).
    readonly property string seedLine: seedFollows ? seedText : ""
    // What the box shows while its text is empty: the passage, else the tab hint.
    readonly property string emptyHint: seedLine !== "" ? seedLine : placeholder

    // Live autocomplete (Scripture/The Table's reference suggestions): `suggestions` is a
    // provider FUNCTION (text) => { rows: [{ label, detail, payload }], complete: "" } —
    // called fresh on every edit. `rows` render in the popup; `complete` is the inline
    // autofill tail ("sis " for "gene") appended INTO the box as selected text —
    // FreeShow's replace-the-value autofill. Scripture completes book names only;
    // chapter and verse separators are typed explicitly so multi-digit chapters remain
    // editable. Null provider = no autocomplete (plain filter tabs), popup never built.
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
    // Up/Down while the popup is SUPPRESSED (The Table, whose matches render in the
    // pane): the host walks its own result list by this — one row per press, each
    // showing that row's content in the pane's preview (user call: "if press the
    // down or up button it should search result list and show their paragraph
    // preview").
    signal stepMatch(int delta)
    // A RESULT LIST IS UP IN THE PANE (The Table while a search has matches). Set by
    // the tab bar from the pane's own searchMode: it is what turns this box from a
    // plain text field into the search's controller — Up/Down walk the rows and
    // Left/Right move the selector across the pane's columns (user call: "the moment
    // we are in search mode the controls have change").
    property bool listMode: false
    signal stepColumn(int delta)
    // Esc: the search is abandoned, not just emptied (see the key's own note).
    signal dismissed()

    signal edited(string text)
    // ENTER with no popup row highlighted: take what the box holds. The host routes it
    // to the pane's own accept (ReferencePane::playPicked → stageSlides), which is the
    // gesture that puts the pick on the MONITOR PREVIEW — the same thing Enter does on
    // the pane itself. Without this, typing in the box made the field "the one being
    // typed into" and the pane's accept shortcut stood down, so Enter in the box did
    // nothing at all.
    signal accepted(string text)

    // SIZED BY WHAT THE BOX SHOWS, NOT BY A MAGIC NUMBER — the hint names the active
    // tab ("Search Shows") OR, once the pane has a passage, that passage's line
    // ("55-0117 - How The Angel Came…"), so the field must end where its own words
    // end and must be long enough for the SEED. That is why the seed is CLIPPED
    // (see the hint Text): a full sermon title is never the width of the box, but
    // its head is, and the digits typed at the end stay in view. Past the hint it
    // still grows with what was TYPED, so a completed citation cannot run out.
    // LibraryTabBar clamps the whole thing so it can't run into the tabs.
    readonly property int minWidth: 150
    // The seeded line asks for more room than a tab name does; capped so the tabs
    // keep their row on a narrow dock (the tab bar's own clamp is the harder one).
    readonly property int seedMaxWidth: 330
    implicitWidth: Math.max(minWidth, glass.x + glass.width + 12
                                      + Math.min(Math.max(hint.implicitWidth, input.contentWidth),
                                                 seedLine !== "" ? seedMaxWidth - 38 : 1e9) + 26)
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
        // keystroke can be the chapter digit. Letters typed right after a commit are
        // frozen briefly; a digit or Backspace unfreezes instantly.
        if (completion !== "" && completion.trim() !== "" && root.text !== "" &&
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
    // The LAST textEdited was an erasure (Backspace/Delete): do not re-complete\r
    // the text the user is deleting.\r
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
        // EMPTIED BY A TAB SWITCH (the host pushes the new tab's stored query, "" when
        // it has none) while the field still holds the keyboard: the new tab's passage
        // belongs in it, not an empty box beside a pane that names one.
        if (value === "" && input.activeFocus)
            root.applySeed()
    }
    function closeSuggestions() {
        if (sugPanel)
            sugPanel.visible = false
    }
    // THE BOX IS FILLED WITH THE SEED — real text, caret at the END, ready for the
    // paragraph/verse digits (user call: "it should fill the search with it so that I
    // add 56 at the back"). It is NOT pushed into the pane: no `edited`, so no filter
    // and no resolve run until the operator actually types. That is the difference
    // between holding the line and searching it.
    //
    // The caret at the end is the whole point: "…Commission " + "56" is ¶56 of that
    // sermon, which is what the pane's own resolve already reads (resolveCitation's
    // trailing-number rule). Clicking into the field with something already typed
    // never replaces it — the seed is only for an EMPTY box.
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
    // The seed we last put in the field, so it can be REPLACED when the pane moves
    // to another sermon (updating the field only while it still holds the untouched
    // seed — a query the operator typed is never overwritten).
    property string seededLine: ""
    onSeedTextChanged: {
        // Read the NEW line from seedText itself (not seedLine, whose binding may still
        // be settling in this handler) and only when the seed still follows the pane.
        const next = root.seedFollows ? root.seedText : ""
        if (next === "")
            return
        // UNTOUCHED seed only: the moment those exact characters are in the field the
        // operator has not typed anything of their own, so following the pane is safe —
        // including while the field has focus (that is the case that looked stuck: the
        // box kept naming the previous sermon while the pane moved on).
        if (root.seededLine !== "" && input.text === root.seededLine) {
            root.seededLine = next
            root.resetText(next)
            return
        }
        // AN EMPTY BOX THAT STILL HAS THE KEYBOARD TAKES THE NEW SEED. Switching from
        // The Table to Scripture changes the seed from a citation line to a book +
        // chapter while the shared field keeps focus — and focus is the only thing that
        // fills the box, so without this the field sat EMPTY beside a pane naming
        // "Genesis 1:", and the verse digits typed next replaced the seed instead of
        // appending to it (the reported "scripture has no seed").
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

    // The empty-box line's own measured width (see implicitWidth): the same font the
    // box draws it with, so the frame hugs its words exactly — and UNCLIPPED, so the
    // width is capped against the full line rather than against whatever happens to
    // be on screen.
    Text {
        id: hint
        visible: false   // measurement only; the real hint is drawn inside `input`
        text: root.emptyHint
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
        // The hidden clear control must not reserve width from the empty
        // placeholder: that clipped the final character of labels like
        // "Search Shows". Claim the space only while the clear button exists.
        anchors.right: clearButton.visible ? clearButton.left : parent.right
        anchors.rightMargin: clearButton.visible ? 4 : 8
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
        // SEARCH MODE OWNS THE ARROWS: left/right move the pane's selector column
        // (years -> sermons -> results -> preview) instead of the caret. Without this
        // the keys never reached the pane at all — the field holds focus, so the
        // pane's own shortcuts stand down — which is why ‹ › looked dead in a search.
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
            // ESC LEAVES THE SEARCH: the host puts the pane back where it was before
            // the first keystroke (user call: "when i press esc i expect to take back
            // where i was before entering search mode"). The text clears as before.
            k.accepted = true
            root.dismissed()
            input.clear()
            root.frozenText = ""
            root.lastEditShrank = false
            root.text = ""
            root.edited("")
        }
        // Focus FILLS an empty box from the seed (see applySeed) — this is what makes
        // the field "hold" the passage — and refocusing with text already in re-opens
        // the suggestion list.
        onActiveFocusChanged: {
            if (!activeFocus)
                return
            root.applySeed()
            root.refreshSuggestions()
        }

        // The empty-box line: the passage when the pane has one (see seedLine), else
        // the tab's own hint. It can be much longer than the box, so it ELIDES at the
        // right — the head ("55-0117 - How The Angel …") is what identifies it, and
        // the digits the operator types land at the end of the committed line, not of
        // this hint. Not elided when it is the short tab hint.
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

    // Anywhere on the box starts typing. In autocomplete mode (a tab with `suggestions`)
    // the click toggles: popup open → close it and let the click pass, closed → open it
    // (with the current text) — re-clicking the field browses its suggestions again.
    MouseArea {
        id: boxClick
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        onClicked: {
            // Clicking the empty box fills it with the passage the pane is on (see
            // applySeed) — the focus handler below does it, so the field is ready to
            // take the paragraph/verse digits without a second gesture.
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
