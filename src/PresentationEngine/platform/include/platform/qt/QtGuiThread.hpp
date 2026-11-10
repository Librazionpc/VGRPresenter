#pragma once

#include "core/common/Common.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <optional>
#include <utility>

namespace bps::platform {

// Runs `fn` on the GUI thread and returns its result: directly when the caller is already there,
// otherwise queued with the caller waiting. For the Qt services that must live on the GUI thread
// (the clipboard, the platform theme's dialogs). `fn` must return a bps::Result (or something
// constructible from an Error), so a dead GUI thread surfaces as an error instead of a hang.
// Never call this from the GUI thread's own blocking path (the GUI thread waits for no one).
template <class Fn>
auto OnGuiThread(Fn&& fn) -> decltype(fn()) {
    using R = decltype(fn());
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        return fn();
    std::optional<R> result;
    const bool queued = QMetaObject::invokeMethod(app, [&] { result.emplace(fn()); }, Qt::BlockingQueuedConnection);
    if (!queued || !result)
        return Error::Make(Err::InvalidState, "Platform", "the interface thread did not answer");
    return std::move(*result);
}

// The same for a value that has a natural "nothing" answer (a list, a bool): `fallback` is returned when
// there is no Qt application or the GUI thread does not answer.
template <class Fn, class R = decltype(std::declval<Fn&>()())>
R OnGuiThreadOr(Fn&& fn, R fallback) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return fallback;
    if (QThread::currentThread() == app->thread()) return fn();
    std::optional<R> result;
    const bool queued = QMetaObject::invokeMethod(app, [&] { result.emplace(fn()); }, Qt::BlockingQueuedConnection);
    return (queued && result) ? std::move(*result) : std::move(fallback);
}

} // namespace bps::platform
