#pragma once
// Recording runs the way the app runs it: the Qt application's main thread is spinning its event loop while
// a worker (the recording pump) feeds the container. A test that blocks the main thread itself (sleeping,
// or waiting inside Close()) starves the media backend of that loop and records an empty file.
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <thread>
#include <utility>

namespace bpstest {
template <class F>
void RunWithEventLoop(F&& work) {
    if (!QCoreApplication::instance()) {
        work();
        return;
    }
    std::atomic<bool> done{false};
    std::thread worker([&] {
        work();
        done.store(true);
    });
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done.load()) loop.quit();
    });
    poll.start(10);
    loop.exec();
    worker.join();
}
} // namespace bpstest
