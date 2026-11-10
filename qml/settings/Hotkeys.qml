pragma Singleton
import QtQuick
import VGRPresenterUI

//  1. THE MAP (`bindings`). Every keyboard action in the app is named here,
//  2. THE SCOPE STACK (Esc = cancel, Enter = accept). A surface that is OPEN —
QtObject {
    id: root

    // ---- 1. THE MAP ------------------------------------------------------
    property var bindings: ({
        cancel: "Esc",
        accept: ["Return", "Enter"],
        // File / session (the app-wide menu accelerators).
        search: "Ctrl+K",
        importShow: "Ctrl+I",
        record: "Ctrl+R",
        // The wall's timer (auto-advance), so a show can be armed and stopped from the keyboard.
        toggleTimer: "Ctrl+Shift+T",
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
        boldText: "Ctrl+B",
        italicText: "Ctrl+I",
        underlineText: "Ctrl+U",
        previousPassage: "Up",
        nextPassage: "Down",
        previousSlide: "Left",
        nextSlide: "Right",
        libraryTabs: ["Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6", "Ctrl+7", "Ctrl+8"]
    })

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
    property var scopes: []
    property int lastScopeId: 0

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

    function close(id) {
        if (!id)
            return
        const list = root.scopes.filter((s) => s.id !== id)
        if (list.length !== root.scopes.length)
            root.scopes = list
    }

    function clear() { root.scopes = [] }

    // ---- the two keys ----------------------------------------------------
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