#pragma once

#include <QThread>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace bps {

// A std::thread-shaped handle for a thread that runs engine code which may touch Qt.
//
// WHY NOT std::thread: the engine paints and measures text with Qt (TextEngine: QFont,
// QFontMetricsF, QTextLayout, QPainter). The first such call on a thread makes Qt create that
// thread's font cache — a QObject — and so register the thread with Qt as an "adopted" thread.
// When a thread Qt does not own ends, Qt's cleanup of it faults (a null thread-data pointer in
// Qt6Core, one fixed offset), and the Windows loader reports the fault while holding its lock:
// every thread of the process freezes (measured: ~130 ms per such thread in an empty process,
// ~20 s in the full app — the freeze after going off air, the 14 s the thread pool took to stop,
// and the "Qt6Core.dll c0000005" crash reports on quit). A QThread owns its Qt state and
// cleans it up in finish(), measured at ~2 ms for the same work.
//
// Usage mirrors the subset of std::thread the engine uses: construct with a callable (and
// arguments), joinable(), join(), move. No detach(): a detached QThread has nobody to delete it.
class QtThread {
public:
    QtThread() noexcept = default;

    template <class F, class... Args,
              class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, QtThread>>>
    explicit QtThread(F&& f, Args&&... args)
        : id_(std::make_shared<std::atomic<std::uint64_t>>(0)) {
        auto body = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto id = id_;
        thread_.reset(QThread::create([body = std::move(body), id]() mutable {
            id->store(static_cast<std::uint64_t>(reinterpret_cast<quintptr>(QThread::currentThreadId())),
                      std::memory_order_release);
            body();
        }));
        thread_->start();
    }

    QtThread(QtThread&&) noexcept = default;
    QtThread& operator=(QtThread&& other) noexcept {
        if (this != &other) {
            join();
            thread_ = std::move(other.thread_);
            id_ = std::move(other.id_);
        }
        return *this;
    }
    QtThread(const QtThread&) = delete;
    QtThread& operator=(const QtThread&) = delete;

    // A QThread must not be destroyed while it runs; where std::thread would terminate the
    // process, finish the work and wait.
    ~QtThread() { join(); }

    bool joinable() const noexcept { return thread_ != nullptr; }

    void join() {
        if (!thread_)
            return;
        thread_->wait();
        thread_.reset();
    }

    // Let the thread end on its own and delete the QThread when it has: for a worker that removes
    // ITSELF from its owner (the pool shrinking). The QThread object lives with its creator, so
    // the deletion is queued there; without an event loop there it simply stays allocated.
    void detach() {
        if (!thread_)
            return;
        QThread* t = thread_.release();
        QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    }

    // The native thread id (the Windows thread id), 0 until the thread has started running —
    // what the PAL needs to set affinity on it.
    std::uint64_t nativeId() const noexcept {
        return id_ ? id_->load(std::memory_order_acquire) : 0;
    }

private:
    std::unique_ptr<QThread> thread_;
    std::shared_ptr<std::atomic<std::uint64_t>> id_;
};

} // namespace bps
