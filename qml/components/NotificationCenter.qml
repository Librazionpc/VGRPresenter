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
    // A notice that OFFERS something is asking for a decision, not just reporting
    // one, so its card stands longer than a plain one (hovering holds it anyway).
    readonly property int actionDurationMs: 15000

    // WHAT A NOTICE'S ACTION DOES, by id. A payload is data — it cannot carry a
    // function — so the component that owns the work registers it here under the
    // id its notice names ("recovery.open"), and the card's button runs it. Small
    // on purpose: this is a lookup for buttons, not a second event bus, and an id
    // with nothing registered simply has no button (the card falls back to the
    // plain notice it was before).
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

    // The identity of a notice is what the operator can SEE: two notices with
    // the same level, title and message are the same notice even when different
    // producers raised them. (Same vocabulary as the engine's own queue —
    // NotificationQueue's groupKey, which merges duplicates within a window
    // instead of queueing them; here the window IS the card's lifetime, because
    // a card that is still on screen is still the thing happening.)
    function groupKeyOf(level, title, message) {
        return (level || "info") + "\u001f" + (title || "") + "\u001f" + message
    }

    // Push a notice. A second copy of one already on screen does NOT become a
    // second card: it refreshes the card that is there — one more occurrence
    // (groupCount), a fresh countdown (nonce) so the newest failure gets a full
    // reading time, and the newest producer's topic. Stacking them six deep was
    // the observable bug: six refused settings writes, six identical toasts,
    // and the operator could read none of them.
    function push(level, title, message, topic, crux, action) {
        const key = root.groupKeyOf(level, title, message)
        const seen = root.items.find((e) => e.groupKey === key)
        if (seen) {
            root.items = root.items.map((e) => e === seen
                ? Object.assign({}, e, {
                      groupCount: (e.groupCount || 1) + 1,
                      nonce: (e.nonce || 1) + 1,
                      topic: topic || e.topic,
                      // Purely presentational, so a repeat may refresh it: the
                      // newest occurrence of the same sentence knows its crux
                      // best (and an older producer may not have had one).
                      crux: crux || e.crux,
                      // Same rule for the action: the newest producer's is the
                      // one still worth offering (an id nobody registered draws
                      // no button — see the registry above).
                      action: action || e.action,
                      ts: Date.now(),
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
            // The names the notice is about — bolded by the toast card (see
            // NotificationToast.crux). Not part of the identity above: the same
            // sentence is the same notice however it was decorated.
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
