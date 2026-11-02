#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class FontService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString defaultFamily READ defaultFamily NOTIFY changed)
    // The host's fixed-width UI face (Qt's FixedFont) — the log and code
    // surfaces read this instead of naming a monospace font of their own.
    Q_PROPERTY(QString monoFamily READ monoFamily NOTIFY changed)
    Q_PROPERTY(QString defaultContentFamily READ defaultContentFamily CONSTANT)

public:
    static FontService &instance();
    static FontService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    Q_INVOKABLE QString resolve(const QString &requested) const;

    QString defaultFamily() const;
    QString monoFamily() const;
    static QString defaultContentFamily();

signals:
    void changed();

private:
    explicit FontService(QObject *parent = nullptr);

    mutable QHash<QString, QString> cache_;
};