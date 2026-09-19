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
            duration: modelData.level === "error"
                      ? NotificationCenter.errorDurationMs
                      : NotificationCenter.defaultDurationMs
            onDismissed: NotificationCenter.dismiss(modelData.id)
        }
    }
}
