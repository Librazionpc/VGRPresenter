#include "modules/notification/NotificationQueue.hpp"

#include <algorithm>

namespace bps::notification {

namespace {

int BucketIndex(const Notification& n) {
    // 0=lowest … 4=highest. Priority dominates, severity breaks ties.
    int p = static_cast<int>(n.priority);
    int s = static_cast<int>(n.severity);
    int idx = p * 2;
    if (s == static_cast<int>(Severity::Critical)) idx += 1;
    if (s == static_cast<int>(Severity::Error)) idx += 1;
    idx = std::max(0, std::min(4, idx));
    return idx;
}

} // namespace

NotificationQueue::NotificationQueue(size_t maxPending, size_t maxPerSecond)
    : maxPending_(maxPending), maxPerSecond_(maxPerSecond) {}

void NotificationQueue::Configure(size_t maxPending, size_t maxPerSecond) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxPending_ = maxPending;
    maxPerSecond_ = maxPerSecond;
}

bool NotificationQueue::Push(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = n.timestampMs;
    if (!n.groupKey.empty()) {
        // Dedup + grouping: merge into the latest entry with the same key
        // within the window.
        auto it = groupLatest_.find(n.groupKey);
        if (it != groupLatest_.end()) {
            for (auto& bucket : buckets_) {
                for (auto& e : bucket) {
                    if (e.n.id == it->second) {
                        e.n.groupCount += n.groupCount;
                        e.n.message = n.message;   // newest message wins
                        e.n.timestampMs = now;
                        ++stats_.grouped;
                        return false;   // merged, not enqueued
                    }
                }
            }
        }
    }

    // Rate limiting: drop if we exceed maxPerSecond in the last second.
    while (!window_.empty() && window_.front().first <= now - 1000)
        window_.erase(window_.begin());
    if (window_.size() >= maxPerSecond_) {
        ++stats_.dropped;
        return false;
    }

    // Overflow: drop the lowest-priority oldest entry to make room.
    size_t total = 0;
    for (const auto& b : buckets_) total += b.size();
    if (total >= maxPending_) {
        for (auto& bucket : buckets_) {
            if (!bucket.empty()) {
                if (!bucket.front().n.groupKey.empty())
                    groupLatest_.erase(bucket.front().n.groupKey);
                bucket.pop_front();
                ++stats_.dropped;
                break;
            }
        }
    }

    Entry e{n, nextSeq_++};
    if (!e.n.groupKey.empty()) groupLatest_[e.n.groupKey] = e.n.id;
    buckets_[BucketIndex(e.n)].push_back(std::move(e));
    window_.push_back({now, e.seq});
    stats_.pending = PendingLocked();
    stats_.maxPending = std::max(stats_.maxPending, stats_.pending);
    return true;
}

std::optional<Notification> NotificationQueue::Pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 4; i >= 0; --i) {
        if (!buckets_[i].empty()) {
            Notification n = buckets_[i].front().n;
            if (!n.groupKey.empty()) groupLatest_.erase(n.groupKey);
            buckets_[i].pop_front();
            ++stats_.delivered;
            stats_.pending = PendingLocked();
            return n;
        }
    }
    return std::nullopt;
}

size_t NotificationQueue::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PendingLocked();
}

bool NotificationQueue::Update(uint64_t id, std::function<void(Notification&)> mutate) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& bucket : buckets_) {
        for (auto& e : bucket) {
            if (e.n.id == id) {
                mutate(e.n);
                return true;
            }
        }
    }
    return false;
}

std::optional<Notification> NotificationQueue::Find(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& bucket : buckets_) {
        for (const auto& e : bucket) {
            if (e.n.id == id) return e.n;
        }
    }
    return std::nullopt;
}

bool NotificationQueue::Remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& bucket : buckets_) {
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (it->n.id == id) {
                if (!it->n.groupKey.empty()) groupLatest_.erase(it->n.groupKey);
                bucket.erase(it);
                return true;
            }
        }
    }
    return false;
}

void NotificationQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : buckets_) b.clear();
    groupLatest_.clear();
    window_.clear();
    stats_.pending = 0;
}

QueueStats NotificationQueue::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    QueueStats s = stats_;
    s.pending = PendingLocked();
    return s;
}

size_t NotificationQueue::PendingLocked() const {
    size_t total = 0;
    for (const auto& b : buckets_) total += b.size();
    return total;
}

} // namespace bps::notification
