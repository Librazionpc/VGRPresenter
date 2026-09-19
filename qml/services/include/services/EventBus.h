#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <qqml.h>

class QJSEngine;
class QQmlEngine;

// App-wide event bus — the UI-process mirror of the backend engine's own
// EventBus::Subscribe<T>() (see PresentationEngine docs/api/README.md §"event
// bus"). QML has no templates to match that typed-Subscribe<T>() pattern
// with, so this is topic-string + payload-map instead of typed events. Any
// C++ code (CrashHandler, the Qt message handler installed in main.cpp, and
// eventually a bridge relaying the real engine's EventBus into the UI
// process) publishes through EventBus::instance() without touching
// QQmlEngine at all; any QML subscribes via
// `Connections { target: EventBus; function onEventPosted(topic, payload) {} }`.
//
// Kept deliberately generic rather than notification-specific: NotificationCenter.qml
// is just one subscriber. Anything else that wants to react to app-wide
// events (future telemetry, a debug console, etc.) can subscribe to the same
// channel instead of every producer growing its own bespoke signal.
class EventBus : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // Plain C++ access (works even before a QQmlEngine exists — the crash
    // handler and main()'s message handler both need this).
    static EventBus &instance();

    // QML singleton factory — returns the SAME instance as instance(), so
    // C++-side publishes and QML-side subscriptions are never two different
    // objects.
    static EventBus *create(QQmlEngine *engine, QJSEngine *jsEngine);

    // topic: dotted namespace, e.g. "log.warning", "app.crash",
    // "av.routeChanged". payload: arbitrary key/value data. By convention
    // (see NotificationCenter.qml) a payload with a non-empty "message" is
    // treated as user-facing and becomes a toast; topics with no message are
    // still delivered to every subscriber, just not surfaced as one.
    Q_INVOKABLE void publish(const QString &topic, const QVariantMap &payload = QVariantMap());

signals:
    void eventPosted(const QString &topic, const QVariantMap &payload);

private:
    explicit EventBus(QObject *parent = nullptr);
    Q_DISABLE_COPY(EventBus)
};
