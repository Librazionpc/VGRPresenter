pragma Singleton
import QtQuick
import VGRPresenterUI

// Turns EventBus traffic into transient toasts. Deliberately has no publish
// API of its own — everything (the C++ CrashHandler's log, Qt's own
// qWarning/qCritical forwarded from main.cpp, QQmlApplicationEngine's own
// `warnings` signal, and eventually real engine-bridge events) goes through
// EventBus.publish() instead, so this stays a pure subscriber and any of
// those producers work without knowing NotificationCenter exists.
//
// Convention: an event only becomes a toast when its payload carries a
// non-empty "message" — topics without one (e.g. future state-change
// telemetry like "av.routeChanged") are still delivered to every other
// EventBus subscriber, just not shown here. See EventBus.h.
// Item, not QtObject — QtObject has no default property, so it can't hold
// the Connections {} child below (this singleton is never placed in the
// visual tree, so being an Item has no visible effect).
Item {
    id: root

    // [{ id, level, title, message, topic, ts }] — oldest first.
    property var items: []

    property int _nextId: 1
    readonly property int defaultDurationMs: 5000
    readonly property int errorDurationMs: 8000

    function push(level, title, message, topic) {
        const entry = {
            id: root._nextId++,
            level: level || "info",
            title: title || "",
            message: message,
            topic: topic || "",
            ts: Date.now(),
        }
        root.items = root.items.concat([entry])
        return entry.id
    }

    function dismiss(id) {
        root.items = root.items.filter((e) => e.id !== id)
    }

    function clear() {
        root.items = []
    }

    Connections {
        target: EventBus
        function onEventPosted(topic, payload) {
            if (!payload || !payload.message)
                return
            // Settings > General > Notifications (and Lock In Mode): switched off, only errors still interrupt.
            const settings = SettingsService.values
            if ((settings["notifications.show"] === false || settings["appearance.lockInMode"] === true) && payload.level !== "error")
                return
            root.push(payload.level, payload.title, payload.message, topic)
        }
    }
}
