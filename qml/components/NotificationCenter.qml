pragma Singleton
import QtQuick
import VGRPresenterUI

Item {
    id: root

    // FIFO: oldest first. NotificationOverlay displays only items[0], so a
    // queued notice's reading timer does not start until it reaches the front.
    property var items: []
    readonly property var current: items.length > 0 ? items[0] : null

    property int _nextId: 1
    readonly property int defaultDurationMs: 5000
    readonly property int errorDurationMs: 8000
    // A notice that OFFERS something is asking for a decision, not just reporting
    // one, so its card stands longer than a plain one (hovering holds it anyway).
    readonly property int actionDurationMs: 15000

    property var actions: ({})
    function registerAction(id, run) {
        if (!id || typeof run !== "function")
            return
        root.actions[id] = run
    }
    function hasAction(id) {
        return !!id && typeof root.actions[id] === "function"
    }
    function runAction(id) {
        if (root.hasAction(id))
            root.actions[id]()
    }
    function unregisterAction(id) {
        if (id && root.actions[id] !== undefined)
            delete root.actions[id]
    }

    function groupKeyOf(level, title, message) {
        return (level || "info") + "\u001f" + (title || "") + "\u001f" + message
    }

    function push(level, title, message, topic, crux, action) {
        const key = root.groupKeyOf(level, title, message)
        const seen = root.items.find((e) => e.groupKey === key)
        if (seen) {
            root.items = root.items.map((e) => e === seen
                ? Object.assign({}, e, {
                      groupCount: (e.groupCount || 1) + 1,
                      topic: topic || e.topic,
                      crux: crux || e.crux,
                      action: action || e.action,
                  })
                : e)
            return seen.id
        }
        const entry = {
            id: root._nextId++,
            groupKey: key,
            groupCount: 1,
            nonce: 1,
            level: level || "info",
            title: title || "",
            message: message,
            crux: crux || [],
            // What the notice offers, if anything (see NotificationToast.action);
            // also not part of the identity, for the same reason.
            action: action || null,
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
            root.push(payload.level, payload.title, payload.message, topic, payload.crux,
                      payload.action)
        }
    }
}