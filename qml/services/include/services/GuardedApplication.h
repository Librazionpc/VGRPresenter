#pragma once

// ============================================================================
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