#include "SelfTestDriver.h"

#include <QCursor>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QEvent>
#include <QKeyEvent>
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

void SelfTestDriver::type(const QString &text)
{
    if (!m_window)
        return;
    // One QKeyEvent press+release per character, delivered to the window's
    // focus object through the SAME event pipeline a hardware key uses
    // (window->event() is what QQuickWindow's key handling routes through).
    for (const QChar ch : text) {
        const QString s(ch);
        // '\b' = a real backspace (Qt::Key_Backspace, no text) — the erase path
        // must be drivable, a Space here would poison the scenario.
        const bool isBackspace = ch == QLatin1Char('\b');
        // Non-alphanumerics map to their REAL character (key + text): the old
        // "everything else = Space" made SelfTest.type("47-") type "47 " — and
        // a bare-47 query is deliberately NOT a code, so the code-query probe
        // always saw zero rows. Only control chars fall back to Space.
        const int key = isBackspace ? Qt::Key_Backspace
                                    : (ch.isLetterOrNumber() ? ch.toUpper().unicode()
                                                             : (ch.isPrint() ? ch.unicode() : Qt::Key_Space));
        QEvent::Type t = QEvent::KeyPress;
        QKeyEvent press(t, key, Qt::NoModifier, isBackspace ? QString() : s);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, isBackspace ? QString() : s);
        QGuiApplication::sendEvent(m_window, &press);
        QGuiApplication::sendEvent(m_window, &release);
    }
}

QObject *SelfTestDriver::findItem(const QString &objectName)
{
    return m_window ? lookup(m_window, objectName) : nullptr;
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

bool SelfTestDriver::focusItem(const QString &objectName)
{
    QQuickItem *item = m_window ? lookup(m_window, objectName) : nullptr;
    if (!item) {
        qWarning() << "SelfTestDriver: no item named" << objectName;
        return false;
    }
    item->forceActiveFocus();
    return item->hasActiveFocus();
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
