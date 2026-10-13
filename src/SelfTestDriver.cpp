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

// Raise this window before any synthetic click.
//
// WHY: the injected press/release are dispatched to whatever window is
// FOREGROUND, so a background run's "real click" silently landed on nothing —
// every click scenario had to say "the click may have missed" and fall back to
// calling the control's own handler. That made the fallback the normal path
// and left the click itself unproven. The app may raise ITSELF (a foreign
// process may not steal focus), so calling this in-process is enough: after
// it, a synthetic click really is delivered to this window's controls.
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
    // Topmost for the run: Windows refuses foreground changes from a process
    // the user is not driving, and a window left BEHIND the terminal turned
    // every cursor shape read into the terminal's own cursor — the arrow, at
    // whatever point the probe asked for. A selftest run owns the screen, so
    // putting this window in front is what makes the measurement valid at all.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool alreadyThere = GetForegroundWindow() == hwnd && !IsIconic(hwnd);
    if (!alreadyThere) {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        // Pump the loop while the activation lands: the WM_ACTIVATE/WM_SETFOCUS
        // sequence is delivered as events, so sleeping on one thread without
        // processing them never completes the switch (bounded — ~200 ms worst case).
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
    // activate() here for the same reason press() has it: the injected
    // button-up is delivered to whatever window is FOREGROUND, so a release
    // that lands on another window leaves the control under the pointer still
    // believing it is pressed — measured as a canvas grab that stayed on
    // screen after the mouse came up, intermittently (a real drag found the
    // window in front and passed).
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


#ifdef Q_OS_WIN
namespace {

// Names a cursor handle by the standard cursor it is (LoadCursor), which is how
// a user's own eye classifies what the pointer is showing. Returns "unknown"
// for an application-supplied pixmap cursor (none in this app today).
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
    // The measurement is only valid if the pointer is over OUR window: a run
    // that parked while the terminal (or anything else) was in front reported
    // the covering window's cursor and read as a failure. activate() is cheap
    // when the window already has the foreground, so it is safe to call
    // before every read.
    activate();
    move(x, y);
#ifdef Q_OS_WIN
    // Let the OS deliver the move and the app's hover handling — and the
    // window's SetCursor call it ends in — run before reading. A fixed bounded
    // pump, not a wait-for-change: the shape under an ordinary button IS the
    // arrow in the good case, so "wait until it differs" would be a race.
    // ~200 ms of event pumping is a handful of frames and enough for the
    // WM_MOUSEMOVE -> hover -> SetCursor chain to complete.
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
        QThread::msleep(2);
    }
    // One retry when the park did not actually land: either something else is
    // covering the point (WindowFromPoint says so) or the OS moved/clamped the
    // pointer (a window that just resized can swallow the first setPos). Both
    // were seen: the first cut of this probe measured the terminal's window and
    // a point 46px away from the one it asked for, and reported ARROW with
    // complete confidence.
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
        // Qt's own name for the window colour carries the ALPHA in the leading
        // byte ("#aarrggbb"), and that leading byte is the entire point of this
        // list: a window that fills its own pixels in reads "#ff000000", while
        // one that composites reads "#00000000" — the difference between "the
        // audience screen carries the keyed frame" and "it only shows on the
        // tile" (see OutputWindow.qml's `color: "transparent"`).
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
