#pragma once

// ============================================================================
// GuardedApplication — a per-EVENT exception boundary for QML/service handlers.
//
// WHY: Qt does not turn a C++ exception thrown from a Q_INVOKABLE or a slot into
// a JS error. It unwinds out through the QML engine, through Qt's event
// delivery, and out of the event loop — where the only two outcomes today are
// std::terminate (a crash record reading "signal=0", which is how the app died
// five times with "filesystem error: cannot make absolute path: Invalid
// argument []") or the top-level guard around app.exec(), which reports and
// then EXITS: by then the loop is gone, so the app cannot keep serving.
//
// Overriding notify() moves the boundary to ONE event. A handler that faults is
// reported through calltrace::reportException() — engine.log plus the app's own
// toast, with the breadcrumb naming the call in flight — the event is dropped,
// and the event loop is untouched, so the next event is served normally. That is
// the difference between "reported" and "reported AND survived".
//
// COVERAGE — the honest scope:
//   * COVERED: every exception raised while an event is delivered — QML handlers
//     reached from QQuickWindow input, QTimer / singleShot timeouts, signal→slot
//     connections, property bindings evaluated during an event, animations.
//   * NOT covered: code that runs with NO event in flight — component creation
//     inside QQmlApplicationEngine::loadFromModule(), a direct C++→C++ call
//     outside event dispatch, or anything thrown on a worker thread. Those still
//     reach the top-level guard around exec() (or CrashHandler's net). Keep a
//     local try/catch at any such boundary that can throw.
// ============================================================================

#include <QEvent>
#include <QGuiApplication>

#include <exception>

#include "services/CallTrace.h"

class GuardedApplication : public QGuiApplication
{
public:
    using QGuiApplication::QGuiApplication;

    bool notify(QObject *receiver, QEvent *event) override
    {
        try {
            return QGuiApplication::notify(receiver, event);
        } catch (const std::exception &e) {
            Report(e.what());
        } catch (...) {
            Report("non-std exception");
        }
        // The event was not delivered, but the process is intact and the loop is
        // still pumping. This is the whole point: the fault is contained.
        return false;
    }

private:
    static void Report(const char *what)
    {
        // reportException() writes the engine log and raises the app's usual
        // toast, and does NOT rethrow — so the caller simply drops the event.
        calltrace::reportException(
            what, "escaped a QML/service handler — caught at the application boundary");
    }
};
