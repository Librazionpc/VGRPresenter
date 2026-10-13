#pragma once

// ============================================================================
// CallTrace — "what was running?" breadcrumbs, for the app's anonymous failures.
//
// Every failure this app reports today is anonymous: the log ends on a UI
// heartbeat and nothing says WHICH call was in flight. That is true both of the
// AppHangB1 kills (the GUI thread stops pumping and Windows closes the process)
// and of an exception crossing the QML boundary (the event-loop net logs
// what() with no context at all).
//
// This module records a per-thread breadcrumb chain at the chokepoints that can
// block or throw — the QML-facing service entry points and the engine calls
// they make. Three consumers then name the culprit:
//
//   * the HANG WATCHDOG (installHangWatchdog) watches a GUI-thread heartbeat;
//     when the GUI thread stops ticking for a few seconds it writes the chain
//     the GUI thread was inside, to the engine log.
//   * reportException() logs a caught exception WITH the chain (an exception
//     unwinds and pops every Scope, so the chain is read from the sticky
//     "last entered" slot, not from the live stack).
//   * the SEH/signal path in CrashHandler.cpp appends the raw chain snapshot —
//     a pre-encoded char buffer, because that handler may not allocate or call
//     into Qt.
//
// Cost: one push/pop of a `const char*` per instrumented call, plus a memcpy of
// a small fixed buffer when the call is on the GUI thread. No locks on the
// owning thread's stack; the GUI snapshot is mutex-guarded but only touched
// when a breadcrumb is entered/left (dozens per second at most).
// ============================================================================

#include <QString>

namespace calltrace {

// RAII breadcrumb: name the block you are about to run. Pops on scope exit, so
// nest them freely — the chain reads outermost-first.
class Scope
{
public:
    explicit Scope(const char *what);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    bool onGuiThread_ = false;
};

// The active chain on THIS thread, outermost first
// ("LiveOutputService.goLive > PresentationEngine.PresentLive"); "" when none.
QString current();
QString chain();

// The GUI thread's chain, readable from ANY thread — the watchdog's accessor.
QString guiChain();
// The GUI thread's chain as a NUL-terminated raw buffer, updated in place with
// no allocation — for CrashHandler's SEH/signal path (see the header note).
// Returns the current length; never null.
int guiChainRaw(const char **out);

// GUI-thread liveness, read by the watchdog. Cheap: one relaxed atomic store.
void heartbeat();

// Starts the watchdog thread. Idempotent. Call once from main() after the
// engine logger exists. stallSeconds is how long the GUI thread may go without
// a heartbeat before the current chain is written to the log.
void installHangWatchdog(int stallSeconds = 5);

// A caught exception, reported with the breadcrumb that was in flight. Writes
// the chain to the ENGINE LOG (engine.log is the only window onto a failure
// after the fact) and raises the app's usual toast — it does NOT rethrow, so
// the UI survives.
void reportException(const char *what, const char *detail);

// Stops the watchdog. Call from the app's own aboutToQuit: the watchdog thread
// outlives it otherwise, and a late report would touch services mid-teardown.
void shutdown();

// Env-gated self-tests, the codebase's usual shape (see
// LiveOutputService::runEnvSelfTest). One probe per var, none implying another:
//   * VGR_CALLTRACE_SELFTEST=1 — a warning and a reported exception raised from
//     inside a breadcrumb; both MUST appear in engine.log with the chain
//     attached ("the log is the only window onto a failure").
//   * VGR_HANG_SELFTEST=1 — block the GUI thread past the stall threshold; the
//     watchdog must report it naming the call, and WriteHangDump must leave a
//     crash-log record + dump + gdb capture.
//   * VGR_BOUNDARY_SELFTEST=1 — throw out of an event handler; the app must
//     report it and STAY ALIVE (GuardedApplication's per-event boundary).
// Inert without the matching env var.
void runEnvSelfTest();

} // namespace calltrace

// Name the enclosing block for the breadcrumb chain.
#define VGR_CALL_TRACE(name) ::calltrace::Scope _vgrCallScope##__LINE__(name)
