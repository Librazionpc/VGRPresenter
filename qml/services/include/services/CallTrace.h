#pragma once

// ============================================================================
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
int guiChainRaw(const char **out);

// "Am I the interface thread?" WITHOUT touching Qt's thread bookkeeping. QThread::currentThread()
// is not a read: on a thread Qt did not start it creates and registers an "adopted" thread for the
// caller, and when that thread later ends Qt's own cleanup faults (a null thread-data pointer, in
// Qt6Core at one fixed offset). The loader reports that fault with its lock held, which freezes
// EVERY thread of the process for ~20 s — the stall after going off air — and, when the report
// escapes, is the Windows "stopped working" crash. Engine threads (the live render loop, the
// log pump, the NDI feed) end constantly and must never be adopted, so no code that can run on
// one may ask Qt which thread it is on: compare the native thread id with this one instead.
// markGuiThread() is called once, on the interface thread, as soon as the application exists.
void markGuiThread();
bool onGuiThreadNow();

// GUI-thread liveness, read by the watchdog. Cheap: one relaxed atomic store.
void heartbeat();

void installHangWatchdog(int stallSeconds = 5);

void reportException(const char *what, const char *detail);

// Stops the watchdog. Call from the app's own aboutToQuit: the watchdog thread
// outlives it otherwise, and a late report would touch services mid-teardown.
void shutdown();

void runEnvSelfTest();
} // namespace calltrace

// Name the enclosing block for the breadcrumb chain.
#define VGR_CALL_TRACE(name) ::calltrace::Scope _vgrCallScope##__LINE__(name)