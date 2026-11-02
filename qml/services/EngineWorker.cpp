#include "services/EngineWorker.h"

#include <QDebug>
#include <QThread>

namespace vgr {
EngineWorker &EngineWorker::instance()
{
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
        connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
        thread_->start();
    }
    QMetaObject::invokeMethod(worker_, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
}

void EngineWorker::runOffThreadCancellable(const bps::JobTicketPtr &ticket, std::function<void()> fn)
{
    if (!ticket || !fn)
        return;   // no ticket = no way to take it back: the caller wanted runOffThread
    if (ticket->Cancelled())
        return;   // already abandoned while it was being posted
    runOffThread([ticket, fn = std::move(fn)]() {
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
        qWarning("EngineWorker: worker did not stop within 2s — leaving it detached");
        thread_->setParent(nullptr);
        worker_->setParent(nullptr);
        worker_ = nullptr;
        thread_ = nullptr;   // posts stop here (the engine is being torn down)
        return;
    }
    worker_ = nullptr;
    delete thread_;
    thread_ = nullptr;
}
} // namespace vgr