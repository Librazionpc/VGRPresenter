#include "services/EventBus.h"

#include "services/CallTrace.h"

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
    // NOT QThread::currentThread(): on an engine thread that registers the thread with Qt, and
    // Qt then faults when the thread ends (see calltrace::markGuiThread).
    if (!calltrace::onGuiThreadNow()) {
        QMetaObject::invokeMethod(this, [this, topic, payload] { emit eventPosted(topic, payload); },
                                  Qt::QueuedConnection);
        return;
    }
    emit eventPosted(topic, payload);
}

void EventBus::notify(const QString &message, const QString &level,
                      const QString &title, const QString &topic, const QVariant &crux,
                      const QVariant &action)
{
    QVariantMap payload{
        {QStringLiteral("level"), level.isEmpty() ? QStringLiteral("info") : level},
        {QStringLiteral("title"), title.isEmpty() ? QStringLiteral("Application") : title},
        {QStringLiteral("message"), message},
        {QStringLiteral("origin"), QStringLiteral("ui")},
    };
    // Only carried when there IS one: a notice without a crux must stay exactly
    // the payload it was before this key existed (see the header).
    if (crux.isValid())
        payload.insert(QStringLiteral("crux"), crux);
    // Same for the action — the label/id map NotificationCenter hands the card,
    // which turns it into the notice's one button.
    if (action.isValid())
        payload.insert(QStringLiteral("action"), action);
    publish(topic.isEmpty() ? QStringLiteral("ui.notification") : topic, payload);
}