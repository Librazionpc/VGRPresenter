#include "EventBus.h"

#include <QJSEngine>
#include <QQmlEngine>

EventBus::EventBus(QObject *parent)
    : QObject(parent)
{
}

EventBus &EventBus::instance()
{
    static EventBus bus;
    return bus;
}

EventBus *EventBus::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    // The singleton is a static C++ object with no QML-tracked parent —
    // QJSEngine must never try to delete it on teardown.
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

void EventBus::publish(const QString &topic, const QVariantMap &payload)
{
    emit eventPosted(topic, payload);
}
