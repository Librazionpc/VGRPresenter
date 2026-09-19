#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IEvent.hpp"
#include "interfaces/IService.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bps {

// Decoupled communication backbone (docs/specs/05). No module calls another
// directly — everything goes through the bus.
class EventBus final : public IService {
public:
    static EventBus& Instance();

    struct PublishOptions {
        bool async = false;                             // fan out via ThreadPool (06)
        std::chrono::microseconds delay{0};             // via TaskScheduler (07)
    };

    // --- Publish ---
    template <class EventT>
    Result<void> Publish(const EventT& event, const PublishOptions& opts = {}) {
        return PublishShared(EventT::kTopic, std::make_shared<EventT>(event), opts);
    }

    template <class EventT>
    Result<void> PublishAsync(const EventT& event, const PublishOptions& opts = {}) {
        auto p = opts;
        p.async = true;
        return PublishShared(EventT::kTopic, std::make_shared<EventT>(event), p);
    }

    // --- Subscribe ---
    template <class EventT>
    Subscription Subscribe(std::function<void(const EventT&)> cb, int priority = 0) {
        return SubscribeTopic(EventT::kTopic, priority,
                              [cb = std::move(cb)](const IEvent& e) {
                                  cb(static_cast<const EventT&>(e));
                              });
    }

    Result<void> Unsubscribe(Subscription token);
    size_t SubscriberCount(std::string_view topic) const;

    // --- Sticky (late subscribers receive the last value) ---
    void SetSticky(std::string_view topic, std::shared_ptr<const IEvent> event);
    std::shared_ptr<const IEvent> GetSticky(std::string_view topic) const;

    // --- History / replay ---
    void SetHistoryLimit(size_t limit) { historyLimit_ = limit; }
    std::vector<std::shared_ptr<const IEvent>> History(std::string_view topicFilter = {}) const;
    size_t HistorySize() const { return history_.size(); }

    // Replay: deliver the retained history for this event type to `cb` (05 §History).
    template <class EventT>
    size_t Replay(std::function<void(const EventT&)> cb) {
        return ReplayTopic(EventT::kTopic, [cb = std::move(cb)](const IEvent& e) {
            cb(static_cast<const EventT&>(e));
        });
    }

    // --- Tracing (05 §Tracing): bounded trace ring with dispatch timing ---
    void EnableTracing(bool on) noexcept { tracing_.store(on); }
    bool Tracing() const noexcept { return tracing_.load(); }
    struct TraceEntry {
        std::string topic;
        EngineTime at;
        std::chrono::microseconds dispatchTime{0};
        bool deadLetter = false;
    };
    std::vector<TraceEntry> TraceLog(size_t max = 128) const;

    // --- Diagnostics ---
    size_t PublishedCount() const noexcept { return published_.load(); }
    size_t DeadLetterCount() const noexcept { return deadLetters_.load(); }
    const char* ServiceName() const noexcept override { return "EventBus"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const;

private:
    EventBus() = default;

    struct SubscriberEntry {
        uint64_t id;
        std::string topic;
        int priority;
        std::function<void(const IEvent&)> fn;
        int consecutiveFailures = 0;   // guarded by mutex_ (updated post-dispatch)
    };

    Result<void> PublishShared(std::string_view topic, std::shared_ptr<const IEvent> event,
                               const PublishOptions& opts);
    Subscription SubscribeTopic(std::string_view topic, int priority,
                                std::function<void(const IEvent&)> fn);
    size_t ReplayTopic(std::string_view topic, std::function<void(const IEvent&)> cb);
    void Dispatch(std::string_view topic, const std::shared_ptr<const IEvent>& event);

    mutable std::mutex mutex_;
    std::vector<SubscriberEntry> subscribers_;                        // guarded by mutex_
    std::unordered_map<std::string, std::shared_ptr<const IEvent>> sticky_; // guarded by mutex_
    std::deque<std::pair<std::string, std::shared_ptr<const IEvent>>> history_; // guarded by mutex_
    std::deque<TraceEntry> trace_;                     // guarded by mutex_
    size_t traceLimit_ = 1024;
    std::atomic<bool> tracing_{false};
    size_t historyLimit_ = 10000;
    uint64_t nextSubId_ = 1;
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> deadLetters_{0};
};

} // namespace bps
