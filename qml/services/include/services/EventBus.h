#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <qqml.h>

class QJSEngine;
class QQmlEngine;

class EventBus : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // Plain C++ access (works even before a QQmlEngine exists — the crash
    // handler and main()'s message handler both need this).
    static EventBus &instance();

    static EventBus *create(QQmlEngine *engine, QJSEngine *jsEngine);

    Q_INVOKABLE void publish(const QString &topic, const QVariantMap &payload = QVariantMap());

    Q_INVOKABLE void notify(const QString &message,
                            const QString &level = QStringLiteral("info"),
                            const QString &title = QStringLiteral("Application"),
                            const QString &topic = QString(),
                            const QVariant &crux = QVariant(),
                            const QVariant &action = QVariant());

signals:
    void eventPosted(const QString &topic, const QVariantMap &payload);

private:
    explicit EventBus(QObject *parent = nullptr);
    Q_DISABLE_COPY(EventBus)
};