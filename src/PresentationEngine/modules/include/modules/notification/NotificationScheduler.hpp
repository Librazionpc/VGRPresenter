#pragma once

// NotificationScheduler (docs/specs/14 §Notification Scheduler): delayed
// notifications, quiet hours, and presentation-mode suppression. Runs on the
// Core TaskScheduler — no module owns a thread.

#include "modules/notification/Notification.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace bps::notification {

class NotificationScheduler {
public:
    using DeliverFn = std::function<void(const Notification&)>;

    NotificationScheduler() = default;

    void SetDeliver(DeliverFn fn) { deliver_ = std::move(fn); }

    // Schedule delivery after a delay (ms). Returns the notification id.
    void Schedule(const Notification& n, int64_t delayMs);

    // Config-driven quiet hours (notify.quietHours.enabled/start/end, HH:MM).
    void SetQuietHours(bool enabled, std::string start, std::string end);
    bool InQuietHours(int64_t nowMs) const;

    // Presentation mode: non-critical notifications are deferred, not dropped.
    void SetPresenting(bool presenting) { presenting_ = presenting; }
    bool Presenting() const { return presenting_; }

    // Called by the Core TaskScheduler heartbeat (1 s).
    void Tick(int64_t nowMs);

    size_t PendingCount() const;
    void Clear();

private:
    DeliverFn deliver_;
    bool presenting_ = false;
    bool quietEnabled_ = false;
    std::string quietStart_ = "22:00";
    std::string quietEnd_ = "07:00";
    mutable std::mutex mutex_;
    std::multimap<int64_t, Notification> due_;   // dueAtMs -> notification
};

} // namespace bps::notification
