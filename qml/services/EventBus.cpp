#include "services/EventBus.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QThread>

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
    // Producers include engine threads and Qt's message handler (any thread).
    // eventPosted feeds QML, so it must only ever fire on the GUI thread.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, topic, payload] { emit eventPosted(topic, payload); },
                                  Qt::QueuedConnection);
        return;
    }
    emit eventPosted(topic, payload);
}

void EventBus::notify(const QString &message, const QString &level,
                      const QString &title, const QString &topic)
{
    // UI-originated, standard-shaped: same payload convention as the engine
    // relay (level/title/message), plus origin="ui" so any subscriber — and
    // the toast overlay — can tell UI-raised from engine-raised traffic
    // (the relay stamps origin="engine"). Callers who want a filterable
    // topic pass one ("ui.edit.textCopied"); the generic fallback keeps the
    // feed free of unnamed topics.
    QVariantMap payload{
        {QStringLiteral("level"), level.isEmpty() ? QStringLiteral("info") : level},
        {QStringLiteral("title"), title.isEmpty() ? QStringLiteral("Application") : title},
        {QStringLiteral("message"), message},
        {QStringLiteral("origin"), QStringLiteral("ui")},
    };
    publish(topic.isEmpty() ? QStringLiteral("ui.notification") : topic, payload);
}
