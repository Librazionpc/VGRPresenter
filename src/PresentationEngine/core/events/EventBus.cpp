#include "core/events/EventBus.hpp"

#include "core/threading/ThreadPool.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/logging/Logger.hpp"

namespace bps {

EventBus& EventBus::Instance() {
    static EventBus instance;
    return instance;
}

Result<void> EventBus::PublishShared(std::string_view topic, std::shared_ptr<const IEvent> event,
                                     const PublishOptions& opts) {
    if (!event)
        return Error::Make(Err::InvalidArgument, "EventBus", "null event");

    auto deliver = [this, topic, event]() { Dispatch(topic, event); };

    if (opts.delay.count() > 0) {
        auto& sched = TaskScheduler::Instance();
        if (sched.IsInitialized()) {
            auto res = sched.ScheduleOnce(deliver, opts.delay);
            if (res.ok()) return Ok();
        }
    }
    if (opts.async) {
        auto& pool = ThreadPool::Instance();
        if (pool.IsInitialized()) {
            auto r = pool.SubmitBackground(deliver);
            return r.ok() ? Result<void>(Ok()) : Result<void>(r.error());
        }
    }
    deliver();
    return Ok();
}

Subscription EventBus::SubscribeTopic(std::string_view topic, int priority,
                                      std::function<void(const IEvent&)> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = nextSubId_++;
    subscribers_.push_back(SubscriberEntry{id, std::string(topic), priority, std::move(fn), 0});
    return Subscription{id};
}

Result<void> EventBus::Unsubscribe(Subscription token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(subscribers_.begin(), subscribers_.end(),
                           [&](const SubscriberEntry& s) { return s.id == token.id; });
    if (it == subscribers_.end())
        return Error::Make(Err::NotFound, "EventBus", "subscription not found");
    subscribers_.erase(it);
    return Ok();
}

size_t EventBus::SubscriberCount(std::string_view topic) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& s : subscribers_)
        if (s.topic == topic) ++n;
    return n;
}

void EventBus::SetSticky(std::string_view topic, std::shared_ptr<const IEvent> event) {
    std::lock_guard<std::mutex> lock(mutex_);
    sticky_[std::string(topic)] = std::move(event);
}

std::shared_ptr<const IEvent> EventBus::GetSticky(std::string_view topic) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sticky_.find(std::string(topic));
    return it == sticky_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const IEvent>> EventBus::History(std::string_view topicFilter) const {
    std::vector<std::shared_ptr<const IEvent>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (topicFilter.empty() || it->first == topicFilter) out.push_back(it->second);
    }
    return out;
}

size_t EventBus::ReplayTopic(std::string_view topic, std::function<void(const IEvent&)> cb) {
    std::vector<std::shared_ptr<const IEvent>> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = history_.rbegin(); it != history_.rend(); ++it)
            if (it->first == topic) events.push_back(it->second);
    }
    size_t delivered = 0;
    for (const auto& e : events) {
        try {
            cb(*e);
            ++delivered;
        } catch (...) {
            deadLetters_.fetch_add(1);   // replay failures are also dead letters
        }
    }
    return delivered;
}

std::vector<EventBus::TraceEntry> EventBus::TraceLog(size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEntry> out;
    out.reserve(std::min(max, trace_.size()));
    for (auto it = trace_.rbegin(); it != trace_.rend() && out.size() < max; ++it) out.push_back(*it);
    return out;
}

void EventBus::Dispatch(std::string_view topic, const std::shared_ptr<const IEvent>& event) {
    published_.fetch_add(1);
    EngineTime dispatchBegin = EngineClock::now();

    std::vector<SubscriberEntry> matches;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : subscribers_)
            if (s.topic == topic) matches.push_back(s);
        history_.emplace_back(std::string(topic), event);
        while (history_.size() > historyLimit_) history_.pop_front();
    }

    std::stable_sort(matches.begin(), matches.end(),
                     [](const SubscriberEntry& a, const SubscriberEntry& b) {
                         return a.priority > b.priority;
                     });

    std::vector<uint64_t> failedIds;
    for (auto& s : matches) {
        try {
            s.fn(*event);
        } catch (const std::exception& ex) {
            failedIds.push_back(s.id);
            deadLetters_.fetch_add(1);
            Logger::Instance().Error("EventBus: subscriber threw for '" + std::string(topic) +
                                     "': " + ex.what(), "EventBus");
        } catch (...) {
            failedIds.push_back(s.id);
            deadLetters_.fetch_add(1);
        }
    }
    if (!failedIds.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : subscribers_) {
            if (std::find(failedIds.begin(), failedIds.end(), s.id) != failedIds.end())
                s.consecutiveFailures++;
        }
    }
    if (tracing_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        TraceEntry t;
        t.topic = std::string(topic);
        t.at = dispatchBegin;
        t.dispatchTime =
            std::chrono::duration_cast<std::chrono::microseconds>(EngineClock::now() - dispatchBegin);
        t.deadLetter = !failedIds.empty();
        trace_.push_back(std::move(t));
        while (trace_.size() > traceLimit_) trace_.pop_front();
    }
}

HealthReport EventBus::GetHealth() const {
    HealthReport r;
    r.errorCount = deadLetters_.load();
    r.state = deadLetters_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("{} events published, {} dead letters", published_.load(),
                           deadLetters_.load());
    return r;
}

Metrics EventBus::MetricsSnapshot() const {
    Metrics m;
    std::lock_guard<std::mutex> lock(mutex_);
    m.queueLength = subscribers_.size();
    m.errorCount = deadLetters_.load();
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
