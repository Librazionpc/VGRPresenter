#include "SelfTestDriver.h"

#include <QCursor>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QColor>
#include <QVariantList>
#include <QVariantMap>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QThread>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
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
    QQuickItem *first = nullptr;
    const QList<QQuickItem *> matches = window->findChildren<QQuickItem *>(name);
    for (QQuickItem *item : matches) {
        if (item->isVisible() && item->width() > 0 && item->height() > 0)
            return item;
        if (first == nullptr)
            first = item;
    }
    if (first != nullptr)
        return first;
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
    const QPoint global = m_window->mapToGlobal(QPoint(int(x), int(y)));
    QCursor::setPos(global);
}

void SelfTestDriver::activate()
{
    if (!m_window)
        return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd)
        return;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool alreadyThere = GetForegroundWindow() == hwnd && !IsIconic(hwnd);
    if (!alreadyThere) {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        for (int i = 0; i < 20; ++i) {
            if (GetForegroundWindow() == hwnd)
                break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
        qDebug() << "SelfTestDriver: activated window for input"
                 << "foreground=" << (GetForegroundWindow() == hwnd);
    }
#endif
    m_window->requestActivate();
}

void SelfTestDriver::click(double x, double y)
{
    activate();
    move(x, y);
#ifdef Q_OS_WIN
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
#else
    // Non-Windows diagnostic builds: click synthesis unsupported (the move
    // half still works for hover tracing).
#endif
}

void SelfTestDriver::press(double x, double y)
{
    activate();   // a drag's press must land on this window too (see click)
    move(x, y);
#ifdef Q_OS_WIN
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
#endif
}

void SelfTestDriver::release(double x, double y)
{
    activate();
    move(x, y);
#ifdef Q_OS_WIN
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
#endif
}

void SelfTestDriver::type(const QString &text)
{
    if (!m_window)
        return;
    for (const QChar ch : text) {
        const QString s(ch);
        // '\b' = a real backspace (Qt::Key_Backspace, no text) — the erase path
        // must be drivable, a Space here would poison the scenario.
        const bool isBackspace = ch == QLatin1Char('\b');
        // The arrow glyphs are the caret keys (no text): ← → ↑ ↓ — the caret/arrow paths
        // must be drivable too.
        int arrowKey = 0;
        if (ch == QChar(0x2190)) arrowKey = Qt::Key_Left;
        else if (ch == QChar(0x2192)) arrowKey = Qt::Key_Right;
        else if (ch == QChar(0x2191)) arrowKey = Qt::Key_Up;
        else if (ch == QLatin1Char('\n')) arrowKey = Qt::Key_Return;
        else if (ch == QChar(0x2193)) arrowKey = Qt::Key_Down;
        const bool noText = isBackspace || arrowKey != 0;
        const int key = arrowKey != 0 ? arrowKey
                      : isBackspace ? Qt::Key_Backspace
                                    : (ch.isLetterOrNumber() ? ch.toUpper().unicode()
                                                             : (ch.isPrint() ? ch.unicode() : Qt::Key_Space));
        QEvent::Type t = QEvent::KeyPress;
        QKeyEvent press(t, key, Qt::NoModifier, noText ? QString() : s);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, noText ? QString() : s);
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

#ifdef Q_OS_WIN
namespace {
QString cursorName(HCURSOR handle)
{
    static const struct { LPCWSTR id; const char *name; } kStandard[] = {
        { IDC_ARROW,       "ARROW" },
        { IDC_IBEAM,       "IBEAM" },
        { IDC_WAIT,        "WAIT" },
        { IDC_CROSS,       "CROSS" },
        { IDC_UPARROW,     "UPARROW" },
        { IDC_NO,          "NO" },
        { IDC_SIZEALL,     "SIZEALL" },
        { IDC_SIZENESW,    "SIZENESW" },
        { IDC_SIZENS,      "SIZEVER" },
        { IDC_SIZENWSE,    "SIZENWSE" },
        { IDC_SIZEWE,      "SIZEHOR" },
        { IDC_HAND,        "HAND" },
        { IDC_APPSTARTING, "APPSTARTING" },
        { IDC_HELP,        "HELP" },
    };
    for (const auto &s : kStandard) {
        if (LoadCursorW(nullptr, s.id) == handle)
            return QString::fromLatin1(s.name);
    }
    return QStringLiteral("unknown");
}
} // namespace
#endif

QString SelfTestDriver::cursorAt(double x, double y)
{
    if (!m_window)
        return QStringLiteral("no-window");
    activate();
    move(x, y);
#ifdef Q_OS_WIN
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
        QThread::msleep(2);
    }
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    const auto landedOk = [this, x, y]() {
        const QPoint p = m_window->mapFromGlobal(QCursor::pos());
        return qAbs(p.x() - int(x)) <= 3 && qAbs(p.y() - int(y)) <= 3;
    };
    const auto pump = []() {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
            QThread::msleep(2);
        }
    };
    {
        const QPoint at = QCursor::pos();
        const POINT pt = { at.x(), at.y() };   // WindowFromPoint takes a Win32 POINT
        const bool covered = hwnd && WindowFromPoint(pt) != hwnd;
        if (covered || !landedOk()) {
            activate();
            move(x, y);
            pump();
        }
    }
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !ci.hCursor)
        return QStringLiteral("probe-failed");
    const QPoint at = QCursor::pos();
    const POINT pt = { at.x(), at.y() };
    const bool overUs = hwnd && WindowFromPoint(pt) == hwnd;
    const QString name = cursorName(ci.hCursor);
    char cls[64] = { 0 };
    const HWND got = WindowFromPoint(pt);
    if (got)
        GetClassNameA(got, cls, sizeof(cls) - 1);
    const QPoint landed = m_window->mapFromGlobal(QCursor::pos());
    qDebug() << "SelfTestDriver: cursorAt" << x << y
             << "landed=" << landed << "landedOk=" << landedOk()
             << "overWindow=" << overUs << "shape=" << name
             << "atPointHwnd=" << got << "class=" << cls
             << "foreground=" << GetForegroundWindow()
             << "windowGeom=" << m_window->geometry();
    return name;
#else
    return QStringLiteral("unsupported");
#endif
}

bool SelfTestDriver::itemVisible(const QString &objectName)
{
    QQuickItem *item = m_window ? lookup(m_window, objectName) : nullptr;
    if (!item)
        return false;
    return item->isVisible() && item->width() > 0 && item->height() > 0;
}

void SelfTestDriver::clickItem(const QString &objectName)
{
    const QPointF c = itemCenter(objectName);
    if (!c.isNull())
        click(c.x(), c.y());
}

QVariantList SelfTestDriver::windows() const
{
    QVariantList out;
    const auto tops = QGuiApplication::topLevelWindows();
    for (QWindow *w : tops) {
        auto *qw = qobject_cast<QQuickWindow *>(w);
        if (!qw)
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("objectName"), qw->objectName());
        row.insert(QStringLiteral("title"), qw->title());
        row.insert(QStringLiteral("visible"), qw->isVisible());
        row.insert(QStringLiteral("color"), qw->color().name(QColor::HexArgb));
        row.insert(QStringLiteral("geometry"),
                   QVariantMap{{QStringLiteral("x"), qw->x()},
                               {QStringLiteral("y"), qw->y()},
                               {QStringLiteral("width"), qw->width()},
                               {QStringLiteral("height"), qw->height()}});
        out.append(row);
    }
    return out;
}