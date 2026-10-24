import QtQuick
import VGRPresenterUI
import "."

// One persistent card displays the head of NotificationCenter's FIFO queue.
// Keeping the same instance alive means enqueuing another notice cannot restart
// the visible notice's timeout. Changing the front item (after dismissal) does.
Column {
    id: root
    spacing: Theme.space2

    NotificationToast {
        id: toast
        active: NotificationCenter.current !== null
        level: NotificationCenter.current ? NotificationCenter.current.level : "info"
        title: NotificationCenter.current ? NotificationCenter.current.title : ""
        message: NotificationCenter.current ? NotificationCenter.current.message : ""
        crux: NotificationCenter.current ? (NotificationCenter.current.crux || []) : []
        action: NotificationCenter.current ? (NotificationCenter.current.action || null) : null
        groupCount: NotificationCenter.current ? (NotificationCenter.current.groupCount || 1) : 1
        // A new front item gets a fresh timeout. Queue appends keep this ID
        // unchanged, so they leave the currently displayed timer alone.
        nonce: NotificationCenter.current ? NotificationCenter.current.id : 0
        duration: toast.action
                  ? NotificationCenter.actionDurationMs
                  : (toast.level === "error"
                     ? NotificationCenter.errorDurationMs
                     : NotificationCenter.defaultDurationMs)
        onDismissed: {
            if (NotificationCenter.current)
                NotificationCenter.dismiss(NotificationCenter.current.id)
        }
        onActionTriggered: {
            const current = NotificationCenter.current
            if (!current || !current.action)
                return
            NotificationCenter.runAction(current.action.id)
            NotificationCenter.dismiss(current.id)
        }
    }
}
