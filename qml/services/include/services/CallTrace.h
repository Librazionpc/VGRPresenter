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