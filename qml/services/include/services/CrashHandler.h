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

// A HANG (not a crash) caught by the run-time watchdog: the GUI thread has
// stopped pumping, and the SEH/signal paths will never fire because nothing
// faulted — Windows just ghosts the window and eventually closes the process
// (Event Log Id 1002 "Application hang"). Without this, a hang leaves NO
// artifact at all and is unattributable after the fact.
//
// Called from the watchdog thread, when the GUI thread first exceeds the stall
// threshold. Three artifacts, best-effort, in this order:
//   * a text record (timestamp + the GUI thread's breadcrumb) in the crash log;
//   * hang-<pid>-gdb.txt — a gdb attached to THIS process, whose "thread apply
//     all bt" is a fully SYMBOLIZED per-thread backtrace. gdb is found via
//     VGR_GDB_BIN, then <exe dir>/tools/gdb/gdb.exe (what tools/make_portable.sh
//     ships), then PATH. This is the artifact that actually names the blocked
//     call on a target PC with no developer tools.
//   * hang-<pid>.dmp — a full-thread minidump for richer offline analysis.
//     NOTE: the MinGW gdb cannot read Windows minidumps (ELF cores only), so the
//     dmp needs a different reader; the gdb text capture is the primary one.
//
// Not a crash handler: it runs on a healthy thread with the process still
// intact, so normal allocation/dbghelp use is fine. Best-effort and never
// throws; a failure to dump is logged as a line, not an error.
void WriteHangDump(const char *reason);
