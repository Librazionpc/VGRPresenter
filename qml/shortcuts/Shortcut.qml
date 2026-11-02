import QtQuick
// NAMESPACED on purpose: this file's own type is called Shortcut, so the
// QtQuick type it wraps has to be named explicitly (see the delegation below).
import QtQuick as QuickCore

Item {
    id: root

    width: 0
    height: 0

    // The action whose chord(s) this binding uses (Hotkeys.sequences(action)).
    property string action: ""
    // An explicit chord, overriding `action` when set.
    property string sequence: ""
    readonly property var effectiveSequences: {
        if (root.sequence !== "")
            return [root.sequence]
        return root.action === "" ? [] : Hotkeys.sequences(root.action)
    }

    property bool autoRepeat: false
    property int context: Qt.WindowShortcut

    signal activated()
    signal activatedAmbiguously()

    QuickCore.Shortcut {
        sequences: root.effectiveSequences
        enabled: root.enabled && root.effectiveSequences.length > 0
        autoRepeat: root.autoRepeat
        context: root.context
        onActivated: root.activated()
        onActivatedAmbiguously: root.activatedAmbiguously()
    }
}