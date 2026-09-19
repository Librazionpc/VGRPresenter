#pragma once

// NotificationQueue (docs/specs/14 §Notification Queue): priority FIFO with
// rate limiting, burst handling, deduplication, and grouping. The queue is the
// async buffer between the EventBus and the providers — notifications never
// interrupt the engine (docs/specs/14 §Notification Queue).

#include "modules/notification/Notification.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bps::notification {

struct QueueStats {
    size_t pending = 0;
    size_t delivered = 0;
    size_t grouped = 0;      // notifications merged into existing entries
    size_t dropped = 0;      // rate-limit / overflow drops
    size_t maxPending = 0;
};

class NotificationQueue {
public:
    explicit NotificationQueue(size_t maxPending = 4096, size_t maxPerSecond = 200);

    // Push a notification. Returns true if enqueued; false if dropped
    // (rate limit overflow) or merged into an existing grouped entry.
    bool Push(const Notification& n);

    // Pop the highest-priority notification (severity/priority, then FIFO).
    std::optional<Notification> Pop();

    // Non-blocking peek of pending count.
    size_t Pending() const;

    // Update an in-flight notification by id (progress updates).
    bool Update(uint64_t id, std::function<void(Notification&)> mutate);

    // Fetch a queued notification by id (used to dispatch progress updates).
    std::optional<Notification> Find(uint64_t id) const;

    // Remove a notification by id (dismiss).
    bool Remove(uint64_t id);

    void Clear();
    QueueStats Stats() const;

    // Reconfigure capacity/rate (used at Initialize instead of move-assign).
    void Configure(size_t maxPending, size_t maxPerSecond);

    // Grouping window: notifications with the same groupKey within this many
    // milliseconds merge (groupCount++), instead of queueing duplicates.
    void SetGroupingWindowMs(int64_t ms) { groupingWindowMs_ = ms; }
    void SetRateLimit(size_t perSecond) { maxPerSecond_ = perSecond; }

private:
    mutable std::mutex mutex_;
    size_t maxPending_;
    size_t maxPerSecond_;
    int64_t groupingWindowMs_ = 5000;

    struct Entry {
        Notification n;
        uint64_t seq = 0;
    };
    // Priority buckets: index by (severity+priority) weight; each bucket is FIFO.
    std::deque<Entry> buckets_[5];
    std::vector<std::pair<int64_t, uint64_t>> window_;   // timestamp -> seq (rate limit)
    std::map<std::string, uint64_t> groupLatest_;        // groupKey -> id of latest entry
    uint64_t nextSeq_ = 0;
    QueueStats stats_;

    size_t PendingLocked() const;   // requires mutex_
};

} // namespace bps::notification
