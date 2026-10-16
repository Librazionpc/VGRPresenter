#pragma once

// EngineWorker — ONE place the QML-facing layer hands engine work off the
// interface thread, and the answer back onto it.
//
// Why this exists. Every service and model in qml/services and qml/models is a
// thin adapter over an ENGINE singleton (BibleEngine, FlowEngine,
// RecordingEngine, ProductionEngine, SongEngine, DisplayEngine, RenderEngine,
// the SearchEngine, the libraries). Those singletons take their own locks and
// touch devices and disks, and the QML layer calls them straight from
// invokables and property getters — i.e. from the interface thread, the one
// thread whose milliseconds the user sees. tools/check_gui_thread_blocks.mjs
// flags exactly that (its soft inventory): a call whose cost belongs to the
// engine, sitting in a body a QML binding can evaluate.
//
// The two honest shapes to fix one, both in this file:
//
//   * COMMANDS (start/pause/stop/apply/set…): nothing needs the answer before
//     the call returns, so the work goes to the worker with runOffThread(), and
//     the QML-visible state is updated by whatever the worker marshals back.
//   * ANSWERS: the worker does the engine read and marshals the RESULT onto the
//     interface thread with onInterfaceThread(), where a plain member is
//     published for the property getter to read — so a binding reads a value
//     this thread already owns instead of calling into the engine at all.
//
// BOTH SHAPES CAN BE TAKEN BACK (runOffThreadCancellable, see JobTicket): the
// single queue that gives order also means a job nobody wants any more sits in
// front of the work the screen that IS showing needs — an operator who closes a
// screen or opens another show used to pay for the read they stopped caring
// about, and its verdict still arrived for a caller that had to remember the
// token and ignore it. A job posted with a ticket can be abandoned while queued
// (the work never runs) or have its verdict withdrawn while it runs (the work
// finishes — the file it writes is still wanted — and nothing is reported).

// ORDER MATTERS. Everything posted here runs on ONE worker thread, in the order
// it was posted, one call at a time. A slider dragged across the panel posts
// twenty gain changes; they must reach the engine in that order, with the last
// one landing last (QtConcurrent or a pool would reorder them, which is why this
// is a single thread with a queued event loop and not a pool). A long engine
// call delays the next post — that is the price of "in order", and every call
// here is a command, not a wait.
//
// LIFETIME. The worker thread belongs to this object; the object is created on
// first use and torn down (bounded join) at exit, before the engines go away.
// A lambda must capture BY VALUE: it runs later, on another thread, and must
// never dereference anything the poster could drop meanwhile. A result marshalled
// back with onInterfaceThread(host, ...) is dropped by Qt if `host` is gone —
// that is why the host is always the QObject the work belongs to.

#include "core/threading/JobTicket.hpp"

#include <QMetaObject>
#include <QObject>

#include <functional>
#include <utility>

class QThread;

namespace vgr {

class EngineWorker : public QObject
{
    Q_OBJECT
public:
    // The single worker for the whole QML-facing layer (see ORDER MATTERS).
    static EngineWorker &instance();

    // Run `fn` on the worker thread. Never on the caller's thread; never
    // inline. `fn` must capture by value.
    void runOffThread(std::function<void()> fn);

    // Run `fn` on the worker thread with a CANCELLATION TICKET (see
    // core/threading/JobTicket.hpp): the poster keeps `ticket` and can abandon
    // the job at any point — before it starts (it never runs: Begin() is asked
    // here, so a caller cannot forget it) or while it runs (Begin answered true,
    // so the work finishes and whoever marshals the answer back must ask
    // Deliver() before publishing it). A null or already-cancelled ticket is
    // never posted at all.
    //
    // The ticket rides BY VALUE into the lambda, like everything else here: the
    // poster may cancel and forget it while the job is still queued.
    void runOffThreadCancellable(const bps::JobTicketPtr &ticket, std::function<void()> fn);

    // Marshall `fn` back onto the interface thread to run on `host`'s thread —
    // the shape every worker answer uses to publish a result. Dropped if
    // `host` is destroyed first.
    template <typename F>
    static void onInterfaceThread(QObject *host, F &&fn)
    {
        QMetaObject::invokeMethod(host, std::forward<F>(fn), Qt::QueuedConnection);
    }

    // Bounded stop, called from main.cpp's aboutToQuit BEFORE the engines are
    // torn down (a queued engine call must not land in a dead engine).
    void shutdown();

private:
    explicit EngineWorker(QObject *parent = nullptr);
    ~EngineWorker() override;

    QThread *thread_ = nullptr;      // created lazily on the first runOffThread
    QObject *worker_ = nullptr;      // lives on thread_
    bool shuttingDown_ = false;      // set by shutdown(): no new hand-offs
};

} // namespace vgr
