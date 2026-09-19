#pragma once

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
// diagnostic line to a crash log via raw C file I/O (no Qt/heap-allocating
// calls, which are not safe inside a signal/SEH handler) and then let the
// OS's default handling continue — it does not attempt to recover, show UI,
// or publish to EventBus (the very mechanisms that may have just crashed).
void InstallCrashHandler();
