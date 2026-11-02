#pragma once

#include <QObject>
#include <QString>
#include <qqml.h>

class QJSEngine;
class QQmlEngine;

class CursorService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // Plain C++ access (works without a QQmlEngine).
    static CursorService &instance();

    // QML singleton factory — the SAME instance as instance().
    static CursorService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    Q_INVOKABLE void apply(int shape);

    // Release the override (nothing held → no-op), so the normal per-item
    // cursors apply again.
    Q_INVOKABLE void clear();

    Q_INVOKABLE bool held() const { return m_held; }

private:
    explicit CursorService(QObject *parent = nullptr);
    Q_DISABLE_COPY(CursorService)

    bool m_held = false;
};