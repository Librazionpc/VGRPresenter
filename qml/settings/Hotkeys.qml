pragma Singleton
import QtQuick
import VGRPresenterUI

// THE APP'S KEYBOARD MAP — and the one dispatcher for Esc and Enter.
//
// Two jobs, deliberately together:
//
//  1. THE MAP (`bindings`). Every keyboard action in the app is named here,
//     once, and the QML that performs it says `Shortcut { action: "saveShow" }`
//     instead of writing the chord. Adding a hotkey later (and, when a settings
//     screen grows one, letting an operator rebind it) is a change to this
//     object and nothing else. `sequence(action)` is the only reader.
//
//  2. THE SCOPE STACK (Esc = cancel, Enter = accept). A surface that is OPEN —
//     a dialog, the settings card, the quick-search overlay — pushes what
//     Esc and Enter mean while it is up, and pops it when it closes:
//
//         onShownChanged: shown ? scopeId = Hotkeys.open({ name: "Name dialog",
//                                                         cancel: close,
//                                                         accept: confirm })
//                               : Hotkeys.close(scopeId)
//
//     `cancel()` / `accept()` walk the stack from the TOP down, so the dialog
//     the operator is looking at is the one that answers, and a confirm opened
//     on top of a dialog takes the keys while it is up and hands them back when
//     it closes. Nothing here draws: the window's own two bindings
//     (qml/Main.qml) call cancel()/accept(), and they are enabled only while a
//     scope asks for them (`canCancel` / `canAccept`) — so with no dialog open
//     Esc and Enter behave exactly as they always did in whatever has focus
//     (a canvas item, a rename field, a search box).
//
// Why a stack instead of each surface's own Keys handler: a Keys handler only
// sees a key the FOCUSED item did not consume, and inside a dialog the focus is
// usually in a text field — so "Enter saves" worked in some dialogs and not
// others, and Esc only worked on the ones that happened to have a handler. The
// window's Shortcut sees the key first (Qt delivers shortcuts before the focus
// item), and the stack decides who gets it. The order is also the reason a
// destructive confirm must push WITHOUT an accept (see ConfirmDialog): Enter
// then does nothing there rather than pressing Delete for you.
//
// A scope's handlers are plain functions; one that needs to know whether it ran
// can return true. This object never holds a reference to a destroyed item:
// every opener pops its own scope id in onShownChanged / onDestruction, and a
// stale id is simply ignored.
QtObject {
    id: root

    // ---- 1. THE MAP ------------------------------------------------------
    //
    // REDOALT / canonical names are the app's own vocabulary. "cancel" and
    // "accept" are the two the window always owns (see the scope stack); the
    // rest are the menu accelerators and the Edit canvas' single-key tools,
    // moved here from Main.qml / EditScreen.qml so there is ONE list.
    property var bindings: ({
        cancel: "Esc",
        // TWO chords on purpose: Qt's key sequence "Enter" is the NUMPAD key
        // (Qt::Key_Enter) and "Return" is the main one (Qt::Key_Return) — a
        // binding that spells only "Enter" then silently does nothing when
        // the operator presses the Enter they actually have. Both are the
        // app's accept key, so both are bound.
        accept: ["Return", "Enter"],
        // File / session (the app-wide menu accelerators).
        search: "Ctrl+K",
        importShow: "Ctrl+I",
        record: "Ctrl+R",
        newShow: "Ctrl+N",
        openShow: "Ctrl+O",
        saveShow: "Ctrl+S",
        saveShowAs: "Ctrl+Shift+S",
        // Edit canvas — only while the canvas owns the keyboard.
        undo: "Ctrl+Z",
        redo: "Ctrl+Shift+Z",
        redoAlt: "Ctrl+Y",
        copy: "Ctrl+C",
        paste: "Ctrl+V",
        duplicateSelected: "Ctrl+D",
        // Del — the slide list's own delete (the canvas reads Delete straight
        // from its Keys handler, so this name is the LIST's binding).
        deleteSelected: "Del",
        // The canvas menu's "Select all" row lists this chord too — one
        // binding, so the menu hint and the shortcut cannot disagree.
        selectAll: "Ctrl+A",
        addText: "T",
        // Text style, applied to the selected TEXT item — FreeShow's own chords
        // for its B/I/U toggles (see its edit/values/boxes.ts).
        //
        // Ctrl+I is ALSO File › Import. They cannot both be live: two
        // window-level bindings on one sequence are ambiguous and Qt fires
        // neither. So the edit screen owns Ctrl+I while a text item is being
        // styled on the canvas, and Import's binding switches itself off for
        // exactly that state (see Main.qml's import Shortcut and EditScreen's
        // textStyleKeysLive). Outside the canvas, Ctrl+I still imports.
        boldText: "Ctrl+B",
        italicText: "Ctrl+I",
        underlineText: "Ctrl+U",
        // Reference pane (Scripture / The Table) — the arrow keys over the
        // passage list. Up/Down walk the PICK: the pane's own preview follows,
        // and nothing is staged, so what is going out cannot change. Left/Right
        // walk the pick's slides and STAGE the one they land on — the Main
        // Output tile's preview moves exactly as it does for the wall's ‹ ›
        // (GO LIVE is still the only thing that commits it to air).
        previousPassage: "Up",
        nextPassage: "Down",
        previousSlide: "Left",
        nextSlide: "Right",
        // THE READING STEP has no chord of its own any more: the wall's ‹ ›
        // step lines while its Scroll mode is on (see MonitorWall.qml) and
        // slides while it is off, so one pair of arrows covers both features.
        // The library dock's tab row (Shows, Media, Overlays, Templates,
        // Scripture, The Table, Calendar, Functions) — Ctrl+1 through the
        // last tab, in the row's own left-to-right order. ONE array entry,
        // read by index (LibraryTabBar's Repeater), so adding a ninth tab
        // is a single new chord here rather than a new named binding.
        libraryTabs: ["Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6", "Ctrl+7", "Ctrl+8"]
    })

    // The sequences for an action name (one entry for most, two for `accept`),
    // or [] for one this map does not know — a `Shortcut { action: "…" }` with
    // an unknown name binds nothing (and says so below) rather than silently
    // matching something else.
    function sequences(action) {
        const s = root.bindings[action]
        if (s === undefined || s === null) {
            console.warn("Hotkeys: no sequence for action '" + action + "'")
            return []
        }
        if (Array.isArray(s))
            return s.map((v) => String(v))
        return [String(s)]
    }

    // The first (primary) sequence — for a display label, a menu accelerator
    // hint, or a single-chord `sequence:` read.
    function sequence(action) {
        const list = root.sequences(action)
        return list.length > 0 ? list[0] : ""
    }

    // ---- 2. THE SCOPE STACK ---------------------------------------------
    //
    // `scopes` is replaced (never mutated in place) on every change: the
    // window's two Shortcuts bind `canCancel` / `canAccept`, and a var holding
    // an array only notifies on reassignment.
    property var scopes: []
    property int lastScopeId: 0

    // What the window's Esc / Enter bindings are enabled by. A scope only
    // counts for the key it actually names: a dialog with Save but no Cancel
    // still takes Enter, and leaves Esc to whatever is behind it.
    readonly property bool canCancel: root.scopes.some((s) => s.cancel !== null)
    readonly property bool canAccept: root.scopes.some((s) => s.accept !== null)
    // Who owns the keys right now — diagnostics only ("which dialog ate my Esc").
    readonly property string cancelOwner: {
        for (let i = root.scopes.length - 1; i >= 0; --i)
            if (root.scopes[i].cancel !== null)
                return root.scopes[i].name
        return ""
    }
    readonly property string acceptOwner: {
        for (let i = root.scopes.length - 1; i >= 0; --i)
            if (root.scopes[i].accept !== null)
                return root.scopes[i].name
        return ""
    }

    // Pushes a scope and returns its id (0 when there is nothing to push).
    // `actions.name` is for the diagnostics above; `cancel`/`accept` are
    // functions or null (null = "this key is not mine").
    function open(actions) {
        const a = actions === undefined || actions === null ? {} : actions
        const cancel = a.cancel !== undefined && a.cancel !== null ? a.cancel : null
        const accept = a.accept !== undefined && a.accept !== null ? a.accept : null
        if (cancel === null && accept === null)
            return 0
        const id = ++root.lastScopeId
        const list = root.scopes.slice()
        list.push({
            id: id,
            name: a.name === undefined || a.name === null ? "scope " + id : String(a.name),
            cancel: cancel,
            accept: accept
        })
        root.scopes = list
        return id
    }

    // Pops one scope by id. Idempotent and safe for an id that was never
    // pushed (0) or is already gone — every opener calls this from both its
    // close handler and its destruction handler.
    function close(id) {
        if (!id)
            return
        const list = root.scopes.filter((s) => s.id !== id)
        if (list.length !== root.scopes.length)
            root.scopes = list
    }

    function clear() { root.scopes = [] }

    // ---- the two keys ----------------------------------------------------
    //
    // Top-down: the newest scope that names the key answers it; the first one
    // to return anything stops the walk. Returns true when something handled
    // it (the window's binding uses that only for its own bookkeeping).
    function _dispatch(pick) {
        for (let i = root.scopes.length - 1; i >= 0; --i) {
            const fn = pick(root.scopes[i])
            if (fn === null)
                continue
            fn()
            return true
        }
        return false
    }
    // One line per key that a surface actually took, naming WHICH surface —
    // the answer to "I pressed Esc and nothing happened, what ate it?" (a scope
    // that answers with nothing to do is exactly the bug that reads as a dead
    // key). Silent when no scope claims the key: then the key belongs to
    // whatever has focus, as it always did.
    function cancel() {
        const owner = root.cancelOwner
        const handled = root._dispatch((s) => s.cancel)
        if (handled)
            EngineBridge.log("info", "Hotkeys", "Esc -> " + owner)
        return handled
    }
    function accept() {
        const owner = root.acceptOwner
        const handled = root._dispatch((s) => s.accept)
        if (handled)
            EngineBridge.log("info", "Hotkeys", "Enter -> " + owner)
        return handled
    }
}
