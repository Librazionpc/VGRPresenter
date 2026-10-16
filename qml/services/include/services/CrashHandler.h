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
//
//     The capture is BOUNDED and REPORTED HONESTLY. It runs one at a time and
//     not again within a 60 s cooldown, because attaching suspends every thread
//     and would otherwise re-freeze the stalled GUI thread on every report. The
//     spawned gdb runs with SEM_FAILCRITICALERRORS so a gdb that cannot load its
//     own runtime fails SILENTLY instead of raising a modal loader dialog over
//     the app (see tools/make_portable.sh, which now proves the bundled gdb
//     starts before shipping it). A capture that produced no output is deleted
//     and logged as FAILED — the file's mere existence never counts as success,
//     because it is created before gdb is launched.
//   * hang-<pid>.dmp — a full-thread minidump for richer offline analysis.
//     NOTE: the MinGW gdb cannot read Windows minidumps (ELF cores only), so the
//     dmp needs a different reader; the gdb text capture is the primary one.
//
// Not a crash handler: it runs on a healthy thread with the process still
// intact, so normal allocation/dbghelp use is fine. Best-effort and never
// throws; a failure to dump is logged as a line, not an error.
void WriteHangDump(const char *reason);

// Verifies, at startup, that the shipped forensics tooling ACTUALLY WORKS on
// this machine, and logs one line when it does not. Called from main() once the
// app is up.
//
// Every way that tooling fails is silent until the moment evidence is needed:
// a bundled gdb whose runtime DLL is missing dies at process load (the spawn
// even sets SEM_FAILCRITICALERRORS, so it raises no dialog either), a dropped
// analyze.bat leaves the report unreadable on a machine with no dev tools, an
// unreachable dbghelp.dll means no minidump, and an unwritable crash folder
// means NOTHING is recorded at all — the crash that would have been
// attributable simply leaves no trace. So the check exercises the same chain a
// real capture does, not a file-existence test:
//
//   * the gdb a capture would attach (VGR_GDB_BIN, then the bundled
//     tools/gdb/gdb.exe, then PATH — the order ResolveGdb() uses) is RUN ONCE
//     and must answer with its version;
//   * dbghelp.dll resolves MiniDumpWriteDump, loaded the same way the hang path
//     loads it;
//   * the crash log's own folder is append-writable, probed the way the
//     handlers open it;
//   * in a packaged layout (a dist carries the gdb folder and analyze.bat),
//     analyze.bat is present and non-empty.
//
// A dev tree that ships none of this is a WORKING configuration (the text
// record and minidump remain) and passes silently. The result goes to the log
// as one line: "Forensics/crash tooling ready" on a healthy machine, or
// "Forensics/crash tooling is INCOMPLETE — <what is broken and what it costs>"
// naming every problem at once, so one line is enough to act on.
//
// The same verdict is published to the UI, because a line in a log is only read
// by someone who already suspects something: EngineBridge's crashToolingState /
// crashToolingProblem properties carry it to Settings · General's "Crash
// reports" row, and Main.qml adds it as a note to the crash-recovery notice
// when the previous run ended in a crash — the one moment the user is being
// told that details were written down, and the one moment they need to know
// that on this machine they could not be.
//
// Runs the probes on a WORKER thread — the gdb probe spawns a process and waits
// for it, which must not happen on the thread that draws — and logs from the
// interface thread once it has an answer. Returns immediately; nothing waits on
// it, and a failure here never stops the app from starting. Set
// VGR_SKIP_FORENSICS_CHECK (or point VGR_FORENSICS_DIR at another layout) to
// skip/redirect the probe, e.g. from a packaging smoke test.
void VerifyForensicsTooling();
