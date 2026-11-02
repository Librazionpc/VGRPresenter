#pragma once

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

    void runOffThreadCancellable(const bps::JobTicketPtr &ticket, std::function<void()> fn);

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