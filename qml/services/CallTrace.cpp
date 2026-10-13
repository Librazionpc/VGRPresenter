#include "services/CallTrace.h"

#include "services/CrashHandler.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

// ============================================================================
// See CallTrace.h for the contract. Two properties shape this implementation:
//
//   * The instrumented path must be cheap and must NOT allocate: it runs on
//     every QML-facing service entry point, and — for the GUI thread — a
//     breadcrumb may well be the last thing that ran before a hang. So the
//     chain lives in a thread_local array of `const char*` and the snapshots
//     readers see are fixed char buffers published with one release store.
//
//   * The CRASH path (CrashHandler's SEH/signal handler) may not allocate,
//     lock, or call into Qt. It reaches the GUI snapshot through
//     guiChainRaw(), which only reads an already-published buffer.
// ============================================================================

namespace calltrace {
namespace {

constexpr int kMaxDepth   = 32;    // deepest chain recorded (a diagnostic, not a stack)
constexpr int kSlotBytes  = 512;   // per-slot text; deeper chains are truncated with ">+…"
constexpr int kGuiSlots   = 8;     // GUI snapshot ring: one slot per publish, so a
constexpr int kLastSlots  = 16;    // reader is never reading the slot being rewritten

// The live chain on THIS thread (outermost first). Plain pointers — the names
// are string literals passed by every VGR_CALL_TRACE site, so they outlive any
// push/pop pair.
thread_local const char *t_names[kMaxDepth];
thread_local int t_depth = 0;

// GUI-thread snapshot ("what is the GUI thread inside right now?"), republished
// on entry AND exit.
char g_guiRing[kGuiSlots][kSlotBytes];
std::atomic<int> g_guiSlot{-1};

// Last-ENTERED snapshot, republished on ENTRY only, from any thread. This is
// what reportException() reads: an exception unwinds and pops every Scope, so
// by the time it is caught the live chain is empty and the only witness left is
// the deepest breadcrumb that was ever entered.
char g_lastRing[kLastSlots][kSlotBytes];
std::atomic<int> g_lastSlot{-1};

// GUI-thread liveness, in steady_clock ticks, plus how many ticks have been
// seen. The COUNT is what keeps the watchdog honest during startup: the tick
// only comes from a running event loop, so until a few ticks have landed the
// GUI thread has not yet proved it can tick at all — and the long synchronous
// boot that happens before the loop starts must not read as a hang.
std::atomic<long long> g_lastHeartbeat{0};
std::atomic<long long> g_heartbeatCount{0};
constexpr long long kTicksBeforeWatchdogArms = 3;
std::atomic<bool> g_watchdogStop{false};

bool onGuiThread()
{
    const QCoreApplication *app = QCoreApplication::instance();
    // No application yet (static init) → nobody is publishing to the GUI ring,
    // which is correct: there is no GUI thread to blame.
    return app != nullptr && QThread::currentThread() == app->thread();
}

// Renders the live chain into `out`, truncating rather than overflowing.
int renderChain(char *out)
{
    int written = 0;
    for (int i = 0; i < t_depth; ++i) {
        const char *name = t_names[i];
        if (name == nullptr) continue;
        const int len = static_cast<int>(std::strlen(name));
        if (written + len + 4 >= kSlotBytes) {
            // " +N" tells a reader the chain was cut, not that it ended here.
            std::snprintf(out + written, static_cast<size_t>(kSlotBytes - written),
                          "%s>+%d", i > 0 ? " > " : "", t_depth - i);
            return kSlotBytes - 1;
        }
        if (i > 0) {
            std::memcpy(out + written, " > ", 3);
            written += 3;
        }
        std::memcpy(out + written, name, static_cast<size_t>(len));
        written += len;
    }
    out[written] = '\0';
    return written;
}

// NOTE: the count parameter is NOT called `slots` — Qt defines that as a
// keyword macro, and `next % slots` then expands to `next %` (a build error
// that reads like a parser bug rather than a name collision).
void publish(char (*ring)[kSlotBytes], int slotCount, std::atomic<int> &slot)
{
    const int next = slot.load(std::memory_order_relaxed) + 1;
    char *dst = ring[next % slotCount];
    renderChain(dst);
    slot.store(next, std::memory_order_release);
}

} // namespace

// ---------------------------------------------------------------------------
// Breadcrumbs
// ---------------------------------------------------------------------------

Scope::Scope(const char *what)
    : onGuiThread_(false)
{
    if (t_depth < kMaxDepth) {
        t_names[t_depth] = what;
        ++t_depth;
    }
    // Publish the chain INCLUDING this entry: the deepest breadcrumb is the one
    // a crash or a hang happened in.
    publish(g_lastRing, kLastSlots, g_lastSlot);
    onGuiThread_ = onGuiThread();
    if (onGuiThread_)
        publish(g_guiRing, kGuiSlots, g_guiSlot);
}

Scope::~Scope()
{
    if (t_depth > 0) --t_depth;
    // On exit the GUI snapshot tracks the LIVE chain again (so guiChain() reads
    // as the current nesting); the last-entered ring deliberately keeps the
    // deeper snapshot — see its comment.
    if (onGuiThread_)
        publish(g_guiRing, kGuiSlots, g_guiSlot);
}

QString current()
{
    char buf[kSlotBytes];
    renderChain(buf);
    return QString::fromUtf8(buf);
}

QString chain()
{
    // The sticky last-entered snapshot — readable after an unwind popped the
    // live chain (reportException's whole reason to exist).
    const int slot = g_lastSlot.load(std::memory_order_acquire);
    if (slot < 0) return QString();
    return QString::fromUtf8(g_lastRing[slot % kLastSlots]);
}

QString guiChain()
{
    const int slot = g_guiSlot.load(std::memory_order_acquire);
    if (slot < 0) return QString();
    return QString::fromUtf8(g_guiRing[slot % kGuiSlots]);
}

int guiChainRaw(const char **out)
{
    const int slot = g_guiSlot.load(std::memory_order_acquire);
    if (slot < 0) {
        if (out) *out = "";
        return 0;
    }
    const char *text = g_guiRing[slot % kGuiSlots];
    if (out) *out = text;
    return static_cast<int>(std::strlen(text));
}

// ---------------------------------------------------------------------------
// Liveness + the hang watchdog
// ---------------------------------------------------------------------------

void heartbeat()
{
    g_lastHeartbeat.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);
    g_heartbeatCount.fetch_add(1, std::memory_order_relaxed);
}

void installHangWatchdog(int stallSeconds)
{
    static std::atomic<bool> installed{false};
    if (installed.exchange(true))
        return;
    const auto threshold =
        std::chrono::seconds(stallSeconds > 0 ? stallSeconds : 5);
    // A raw std::thread, deliberately NOT a QThread: the whole point is to
    // observe a GUI thread that is no longer pumping its event loop.
    std::thread([threshold] {
        bool reported = false;
        while (!g_watchdogStop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_watchdogStop.load(std::memory_order_relaxed)) return;
            const long long ticks = g_lastHeartbeat.load(std::memory_order_relaxed);
            // Not armed yet: the event loop has not ticked a few times, so a
            // quiet GUI thread is startup, not a stall.
            if (g_heartbeatCount.load(std::memory_order_relaxed) < kTicksBeforeWatchdogArms)
                continue;
            const auto last = std::chrono::steady_clock::time_point(
                std::chrono::steady_clock::duration(ticks));
            const auto stalled = std::chrono::steady_clock::now() - last;
            if (stalled < threshold) {
                reported = false;   // recovered; re-arm for the next one
                continue;
            }
            if (reported) continue;   // one report per stall, not one per second
            reported = true;
            const QString where = guiChain();
            const double secs = std::chrono::duration<double>(stalled).count();
            // The stall may be permanent (a deadlock), and this watchdog tick
            // is the only moment the live thread stacks exist. Snapshot them
            // into a minidump NOW — an unattributable hang is what happens
            // without it. Best-effort; see CrashHandler.h.
            {
                const QByteArray reason = QStringLiteral("GUI thread has not ticked for %1 s")
                                              .arg(secs, 0, 'f', 1).toUtf8();
                WriteHangDump(reason.constData());
            }
            // The log is the durable half; the toast is what an operator sees
            // without a log viewer. Both name the call in flight, which is the
            // piece every previous hang report was missing.
            EngineBridge::write(
                QStringLiteral("error"), QStringLiteral("HangWatchdog"),
                QStringLiteral("GUI thread has not ticked for %1 s — it was inside: %2")
                    .arg(secs, 0, 'f', 1)
                    .arg(where.isEmpty()
                             ? QStringLiteral("(no instrumented call in flight — the stall is outside the traced entry points)")
                             : where));
            EventBus::instance().notify(
                QStringLiteral("The interface stopped responding for %1 s. It was inside: %2")
                    .arg(secs, 0, 'f', 1)
                    .arg(where.isEmpty() ? QStringLiteral("(an untraced call)") : where),
                QStringLiteral("error"), QStringLiteral("Interface stalled"),
                QStringLiteral("app.ui.hang"));
        }
    }).detach();
}

void shutdown()
{
    g_watchdogStop.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Exception reporting
// ---------------------------------------------------------------------------

void reportException(const char *what, const char *detail)
{
    const QString where = chain();
    const QString message = QStringLiteral("Caught %1: %2 — last call in flight: %3")
                                .arg(QString::fromUtf8(what != nullptr ? what : "exception"),
                                     QString::fromUtf8(detail != nullptr ? detail : ""),
                                     where.isEmpty()
                                         ? QStringLiteral("(none instrumented — the fault is outside the traced entry points)")
                                         : where);
    EngineBridge::write(QStringLiteral("error"), QStringLiteral("Exception"), message);
    EventBus::instance().notify(message, QStringLiteral("error"),
                                QStringLiteral("Unexpected error"),
                                QStringLiteral("app.exception"));
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

void runEnvSelfTest()
{
    // THREE probes with ONE env var each, none implying another: the breadcrumb
    // wiring (VGR_CALLTRACE_SELFTEST), the watchdog (VGR_HANG_SELFTEST), and the
    // per-event exception boundary (VGR_BOUNDARY_SELFTEST). Each is inert
    // without its own var — this is called once from main() after the message
    // handler and the watchdog exist.
    const bool wantTraceProbe = qEnvironmentVariableIsSet("VGR_CALLTRACE_SELFTEST");
    const bool wantHangProbe = qEnvironmentVariableIsSet("VGR_HANG_SELFTEST");
    const bool wantBoundaryProbe = qEnvironmentVariableIsSet("VGR_BOUNDARY_SELFTEST");
    if (!wantTraceProbe && !wantHangProbe && !wantBoundaryProbe)
        return;
    if (wantTraceProbe) {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("CallTrace"),
                            QStringLiteral("self-test: expecting ONE annotated warning and ONE 'Caught ...' exception line below"));
        Scope outer("CallTraceSelfTest.outer");
        {
            Scope inner("CallTraceSelfTest.inner");
            // A warning from inside the chain: the message handler appends
            // "[in flight: CallTraceSelfTest.outer > CallTraceSelfTest.inner]"
            // to THIS line in engine.log, which is the whole point of the
            // module (a bare warning says nothing about which call it was in).
            qWarning("CallTrace self-test: a warning raised inside a breadcrumb");
        }
        // A caught exception from inside the chain: reportException() must name
        // the breadcrumb it came from. The inner breadcrumb is already popped,
        // so the sticky last-entered slot is what supplies the answer.
        try {
            throw std::runtime_error("CallTrace self-test exception");
        } catch (const std::exception &e) {
            reportException(e.what(), "raised by the VGR_CALLTRACE_SELFTEST probe");
        }
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("CallTrace"),
                            QStringLiteral("self-test done"));
    }

    // The WATCHDOG probe: the watchdog is the one path that otherwise only ever
    // runs for real. It has to fire from INSIDE the event
    // loop (the heartbeat counts ticks the loop actually made, and the watchdog
    // stays unarmed until a few have landed), so it is scheduled rather than
    // done here: by the time it runs the loop has ticked many times and the
    // watchdog is armed. The GUI thread then blocks past the stall threshold,
    // inside a breadcrumb, so the watchdog's report must name the call it was
    // inside — exactly the scenario the module exists for.
    if (wantHangProbe) {
        QTimer::singleShot(2000, [] {
            Scope stall("HangSelfTest.blockingTheGuiThread");
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("CallTrace"),
                                QStringLiteral("hang self-test: blocking the GUI thread for 7 s — the watchdog must report it naming this call"));
            std::this_thread::sleep_for(std::chrono::seconds(7));
        });
    }

    // The BOUNDARY probe (VGR_BOUNDARY_SELFTEST=1): an exception thrown from a
    // handler delivered by the event loop must be caught at
    // GuardedApplication::notify(), reported with this breadcrumb, and the app
    // must KEEP RUNNING. Scheduled rather than thrown here so it runs with an
    // event in flight — the only condition the per-event boundary covers.
    if (wantBoundaryProbe) {
        QTimer::singleShot(2500, [] {
            Scope throwing("BoundarySelfTest.throwingFromEventHandler");
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("CallTrace"),
                                QStringLiteral("boundary self-test: throwing out of an event handler — the app must report it and stay alive"));
            throw std::runtime_error("BoundarySelfTest exception");
        });
    }
}

} // namespace calltrace
