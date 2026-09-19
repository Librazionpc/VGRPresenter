#include "modules/notification/NotificationScheduler.hpp"

#include <cstdio>

namespace bps::notification {

void NotificationScheduler::Schedule(const Notification& n, int64_t delayMs) {
    int64_t due = n.timestampMs + delayMs;
    std::lock_guard<std::mutex> lock(mutex_);
    due_.insert({due, n});
}

void NotificationScheduler::SetQuietHours(bool enabled, std::string start, std::string end) {
    std::lock_guard<std::mutex> lock(mutex_);
    quietEnabled_ = enabled;
    quietStart_ = std::move(start);
    quietEnd_ = std::move(end);
}

namespace {

// "HH:MM" (24h) → minutes since midnight. Returns -1 on malformed input.
int ParseClock(std::string_view s) {
    if (s.size() != 5 || s[2] != ':') return -1;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

} // namespace

bool NotificationScheduler::InQuietHours(int64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!quietEnabled_) return false;
    int start = ParseClock(quietStart_);
    int end = ParseClock(quietEnd_);
    if (start < 0 || end < 0) return false;
    // nowMs → local time-of-day minutes (UTC-based approximation for the core;
    // the PAL locale owns true local time).
    int64_t secs = nowMs / 1000;
    int mins = static_cast<int>((secs % 86400 + 86400) % 86400 / 60);
    if (start <= end) return mins >= start && mins < end;    // same-day window
    return mins >= start || mins < end;                       // overnight window
}

void NotificationScheduler::Tick(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = due_.begin(); it != due_.end();) {
        if (it->first <= nowMs) {
            Notification n = it->second;
            it = due_.erase(it);
            if (deliver_) deliver_(n);
        } else {
            break;
        }
    }
}

size_t NotificationScheduler::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return due_.size();
}

void NotificationScheduler::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    due_.clear();
}

} // namespace bps::notification
