import QtQuick
import VGRPresenterUI
import "."

// Single overlay placed once in Main.qml (same pattern as AppCursorCatcher:
// one topmost instance rather than something each screen owns) — stacks
// every NotificationCenter toast, newest at the bottom, above everything
// including open modals so a crash/error notification is never hidden
// behind a Settings dialog.
Column {
    id: root
    spacing: Theme.space2

    Repeater {
        model: NotificationCenter.items

        delegate: NotificationToast {
            required property var modelData

            level: modelData.level
            title: modelData.title
            message: modelData.message
            // The names this notice is about (a device, a source…): drawn bold
            // inside the message so the eye lands on the subject of the toast
            // rather than on the sentence describing it.
            crux: modelData.crux || []
            // What the notice offers, if anything: the card draws it as one
            // button, and running it is the registry's lookup — the card knows
            // the id, not what it does (NotificationCenter.registerAction).
            action: modelData.action || null
            // Coalesced repeats ride the SAME card (NotificationCenter.push):
            // how many identical notices it stands for, and a nonce that moves
            // when another one arrives so its countdown starts over.
            groupCount: modelData.groupCount || 1
            nonce: modelData.nonce || 1
            duration: modelData.action
                      ? NotificationCenter.actionDurationMs
                      : (modelData.level === "error"
                         ? NotificationCenter.errorDurationMs
                         : NotificationCenter.defaultDurationMs)
            onDismissed: NotificationCenter.dismiss(modelData.id)
            // The click that takes the offer up, and the card goes with it: the
            // action IS the notice's whole point, so leaving it on screen after
            // it ran would read as "it did not work".
            onActionTriggered: {
                NotificationCenter.runAction(modelData.action.id)
                NotificationCenter.dismiss(modelData.id)
            }
        }
    }
}
