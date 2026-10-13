#include "services/CursorService.h"

#include <QCursor>
#include <QGuiApplication>
#include <QJSEngine>
#include <QQmlEngine>

CursorService::CursorService(QObject *parent)
    : QObject(parent)
{
}

CursorService &CursorService::instance()
{
    static CursorService service;
    return service;
}

CursorService *CursorService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    // A static C++ object with no QML-tracked parent — QJSEngine must never try
    // to delete it on teardown.
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

void CursorService::apply(int shape)
{
    const QCursor cursor(static_cast<Qt::CursorShape>(shape));
    if (m_held) {
        // Swap in place: setOverrideCursor() again would push a SECOND
        // override that needs a matching restore, and the cursor would be left
        // overridden by whichever shape got there first.
        QGuiApplication::changeOverrideCursor(cursor);
        return;
    }
    QGuiApplication::setOverrideCursor(cursor);
    m_held = true;
}

void CursorService::clear()
{
    if (!m_held)
        return;
    QGuiApplication::restoreOverrideCursor();
    m_held = false;
}
