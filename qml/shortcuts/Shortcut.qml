import QtQuick
// NAMESPACED on purpose: this file's own type is called Shortcut, so the
// QtQuick type it wraps has to be named explicitly (see the delegation below).
import QtQuick as QuickCore

// A keyboard binding, named by ACTION instead of by chord: the component for
// every hotkey in the app (see qml/settings/Hotkeys.qml for the map, and for
// Esc / Enter, which no surface declares itself — they are the window's two
// bindings, dispatched through Hotkeys' scope stack).
//
//     Shortcut { action: "saveShow"; onActivated: editScreen.saveShow() }
//
// It is a drop-in for QtQuick's own Shortcut as well: `sequence`, `enabled`,
// `autoRepeat`, `context`, `activated()` and `activatedAmbiguously()` all mean
// exactly what they meant there, so an existing literal binding
// (`Shortcut { sequence: "Ctrl+S" }`) keeps working unchanged while it is
// migrated to an action name. When BOTH are given, `sequence` wins — an
// explicit chord is the more specific statement.
//
// Why a wrapper at all instead of QtQuick.Shortcut + a string map read at each
// site: the map is only useful if every binding is FORCED through it. A file
// that imports this directory gets this type for the name `Shortcut`, so a new
// binding cannot quietly spell its own chord — it has to name an action and the
// action has to exist in Hotkeys (an unknown name warns and binds nothing).
//
// No UI: this item is 0x0 and draws nothing. It only exists because a
// QQuickShortcut needs an item to belong to a window.
Item {
    id: root

    width: 0
    height: 0

    // The action whose chord(s) this binding uses (Hotkeys.sequences(action)).
    property string action: ""
    // An explicit chord, overriding `action` when set.
    property string sequence: ""
    // One action can own more than one chord — `accept` is both "Return" (the
    // main Enter key) and "Enter" (the numpad one), because Qt treats those as
    // two different keys and a binding that spells only one of them looks
    // broken on the keyboard the operator actually has.
    readonly property var effectiveSequences: {
        if (root.sequence !== "")
            return [root.sequence]
        return root.action === "" ? [] : Hotkeys.sequences(root.action)
    }

    // `enabled` is the base Item's own (default true), on purpose: redeclaring
    // it here only makes Qt warn that it overrides a base member. Callers set
    // it exactly as they would on any Item, and it gates the binding below.
    property bool autoRepeat: false
    property int context: Qt.WindowShortcut

    signal activated()
    signal activatedAmbiguously()

    QuickCore.Shortcut {
        sequences: root.effectiveSequences
        // A binding with neither an action nor a sequence is not a binding:
        // Qt would treat an empty sequence as "no shortcut" anyway, but making
        // it explicit keeps `enabled` meaning what it says.
        enabled: root.enabled && root.effectiveSequences.length > 0
        autoRepeat: root.autoRepeat
        context: root.context
        onActivated: root.activated()
        onActivatedAmbiguously: root.activatedAmbiguously()
    }
}
