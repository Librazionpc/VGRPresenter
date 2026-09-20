#pragma once

#include <QString>

// Installs process-wide crash handling: a Windows SEH unhandled-exception
// filter and a POSIX signal handler (SIGSEGV/SIGABRT/SIGFPE/SIGILL) — the
// app is meant to stay a cross-platform Qt build (see agent-notes/README.md
// §6), so both paths exist rather than just the platform we happen to be
// developing on.
//
// Call exactly once, as early as possible in main(), after QGuiApplication
// exists (it needs QStandardPaths, which is only fully reliable once a
// QCoreApplication is constructed).
//
// A handler firing here means the process is already in a broken, possibly
// corrupted state, so it does exactly one thing: append a best-effort
// diagnostic line (timestamped exception/signal record + raw stack addresses)
// to a crash log via raw C file I/O (no Qt/heap-allocating calls, which are
// not safe inside a signal/SEH handler) and then let the OS's default
// handling continue — it does not attempt to recover, show UI, or publish to
// EventBus (the very mechanisms that may have just crashed).
//
// The UI-side notification happens on the NEXT launch instead:
// ConsumePendingCrashSummary() reads the last crash record, rotates the log
// aside, and returns a one-line summary — main.cpp hands it to the QML engine
// as a context property, and Main.qml turns it into a standard
// EventBus.notify(...) toast the moment QML is alive. (Firing a toast
// from inside the dying process is impossible by design — the bus and its
// subscribers are exactly what may have been corrupted.)
void InstallCrashHandler();

// Returns a human-readable one-line summary of the most recent crash (empty
// when the previous run exited cleanly), then rotates the crash log aside so
// each crash is reported exactly once. Call once, on the GUI thread, at
// startup — never from inside a crash handler.
QString ConsumePendingCrashSummary();
