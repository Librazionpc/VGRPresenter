#include "SelfTestDriver.h"

#include <QCursor>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Everything here runs only when VGR_SELFTEST=1 (see main.cpp). Cursor moves
// go through QCursor::setPos: the OS generates real WM_MOUSEMOVE for them,
// so every hover/enter/exit path in the app sees exactly what a user's hand
// produces — the only faithful way to exercise hover code from a script.

namespace {

// The first item called `name` in the window's VISUAL tree. QObject::findChild follows the QObject
// parent chain, which misses delegates a Repeater/Loader creates (they hang off the item they are
// laid out in, not off the Repeater), so the tab bar's tabs were reported as "no item named ...".
QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    const auto children = root->childItems();
    for (QQuickItem *child : children)
        if (QQuickItem *found = findItem(child, name))
            return found;
    return nullptr;
}

QQuickItem *lookup(QQuickWindow *window, const QString &name)
{
    if (!window)
        return nullptr;
    if (auto *item = window->findChild<QQuickItem *>(name))
        return item;
    return findItem(window->contentItem(), name);
}

} // namespace

SelfTestDriver::SelfTestDriver(QObject *parent)
    : QObject(parent)
{
}

void SelfTestDriver::setWindow(QQuickWindow *window)
{
    m_window = window;
}

void SelfTestDriver::move(double x, double y)
{
    if (!m_window)
        return;
    // Logical window coords -> global screen pixels (device pixels). The
    // app runs at scale 1 in this environment; mapFromGlobal-style math
    // kept explicit so a scaled run still lands correctly.
    const QPoint global = m_window->mapToGlobal(QPoint(int(x), int(y)));
    QCursor::setPos(global);
}

void SelfTestDriver::click(double x, double y)
{
    move(x, y);
    // Give the event loop a beat to process the hover the move produced,
    // then synthesize press+release at the cursor's current position — the
    // same WM messages a physical click produces.
#ifdef Q_OS_WIN
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
#else
    // Non-Windows diagnostic builds: click synthesis unsupported (the move
    // half still works for hover tracing).
#endif
}

bool SelfTestDriver::grab(const QString &itemName, const QString &pngPath)
{
    if (!m_window)
        return false;
    QQuickItem *item = itemName.isEmpty()
        ? m_window->contentItem()
        : lookup(m_window, itemName);
    if (!item) {
        qWarning() << "SelfTestDriver: no item named" << itemName;
        return false;
    }
    QSharedPointer<QQuickItemGrabResult> result = item->grabToImage();
    if (!result)
        return false;
    QObject::connect(result.data(), &QQuickItemGrabResult::ready, [result, pngPath, itemName]() {
        const bool ok = result->saveToFile(pngPath);
        qDebug() << "SelfTestDriver: grab" << itemName << "->" << pngPath << (ok ? "ok" : "FAILED");
    });
    return true;
}

void SelfTestDriver::quit()
{
    QGuiApplication::quit();
}

QPointF SelfTestDriver::itemCenter(const QString &objectName)
{
    if (!m_window)
        return QPointF();
    QQuickItem *item = lookup(m_window, objectName);
    if (!item) {
        qWarning() << "SelfTestDriver: no item named" << objectName;
        return QPointF();
    }
    const QPointF center = item->mapToScene(
        QPointF(item->width() / 2, item->height() / 2));
    qDebug() << "SelfTestDriver: itemCenter" << objectName << "->" << center
             << "(item at" << item->x() << item->y() << item->width() << "x"
             << item->height() << "visible" << item->isVisible() << ")";
    return center;
}

void SelfTestDriver::clickItem(const QString &objectName)
{
    const QPointF c = itemCenter(objectName);
    if (!c.isNull())
        click(c.x(), c.y());
}
