#include "services/EngineWorker.h"

#include <QDebug>
#include <QThread>

namespace vgr {

EngineWorker &EngineWorker::instance()
{
    // Deliberately leaked: the worker outlives every QML object that posts to
    // it, and is retired explicitly by shutdown() from main.cpp's aboutToQuit
    // (before the engines it calls into are torn down) — a static destructor
    // running after QCoreApplication is gone could not join its thread.
    static EngineWorker *s = nullptr;
    if (!s)
        s = new EngineWorker();
    return *s;
}

EngineWorker::EngineWorker(QObject *parent) : QObject(parent) {}

EngineWorker::~EngineWorker()
{
    shutdown();
}

void EngineWorker::runOffThread(std::function<void()> fn)
{
    if (!fn || shuttingDown_)
        return;   // nothing runs after shutdown(): the engines are going away
    if (!thread_) {
        // One worker for the whole layer, started on the first hand-off.
        thread_ = new QThread;
        thread_->setObjectName(QStringLiteral("engine-work"));
        worker_ = new QObject();   // context only; lives on thread_
        worker_->moveToThread(thread_);
        // The worker destroys ITSELF as its loop ends, in its own thread — the
        // ownership rule every QObject follows, and the one that bit the NDI feed
        // (deleting a worker from the interface thread destroyed its child QTimer
        // from the wrong thread: "Timers cannot be stopped from another thread").
        // This one owns no timer today, which is exactly why the rule belongs here
        // rather than in whoever happens to delete it later.
        connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
        thread_->start();
    }
    // Queued to a single receiver in a single thread: FIFO, one at a time. An
    // abandoned job's lambda still runs (it is already in the queue), but it
    // returns before its work does — see runOffThreadCancellable.
    QMetaObject::invokeMethod(worker_, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
}

void EngineWorker::runOffThreadCancellable(const bps::JobTicketPtr &ticket, std::function<void()> fn)
{
    if (!ticket || !fn)
        return;   // no ticket = no way to take it back: the caller wanted runOffThread
    if (ticket->Cancelled())
        return;   // already abandoned while it was being posted
    runOffThread([ticket, fn = std::move(fn)]() {
        // Asked HERE rather than left to the work: an abandoned job's engine call
        // must not happen at all (that is the point of abandoning a read), and a
        // caller that forgot to ask would pay for it anyway.
        if (!ticket->Begin())
            return;
        fn();
    });
}

void EngineWorker::shutdown()
{
    shuttingDown_ = true;
    if (!thread_)
        return;
    thread_->quit();
    if (!thread_->wait(2000)) {
        // A queued engine call is mid-flight (or the thread is wedged). Do NOT
        // delete under it — detach and let the process teardown own it, the same
        // bounded-quit rule the NDI feed thread follows (an unbounded wait here
        // is the "it crashes when I close it" hang).
        qWarning("EngineWorker: worker did not stop within 2s — leaving it detached");
        thread_->setParent(nullptr);
        worker_->setParent(nullptr);
        worker_ = nullptr;
        thread_ = nullptr;   // posts stop here (the engine is being torn down)
        return;
    }
    // NOT deleted from here: it deletes itself in its own thread as that thread's
    // loop ends (the finished/deleteLater connect where it is created). Killing it
    // from this thread was the same wrong-thread destruction the NDI worker's
    // teardown had — harmless while this object has no children, and a warning
    // followed by undefined behaviour the moment it gains one (a QTimer, an event
    // filter). The GUI thread only drops the pointer.
    worker_ = nullptr;
    delete thread_;
    thread_ = nullptr;
}

} // namespace vgr
