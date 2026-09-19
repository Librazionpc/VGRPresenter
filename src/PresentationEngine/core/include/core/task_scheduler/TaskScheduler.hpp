#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace bps {

using TaskId = uint64_t;

enum class TaskKind : int { OneShot = 0, Recurring, Cron, Heartbeat, Countdown, Animation, Sequence };

// Minimal cron spec (docs/specs/07 §3). -1 = wildcard.
// dayOfWeek: 0 = Sunday .. 6 = Saturday.
struct CronSpec {
    int minute = -1;
    int hour = -1;
    int dayOfMonth = -1;
    int month = -1;      // 1..12
    int dayOfWeek = -1;
};

// The engine's authority on time (docs/specs/07).
class TaskScheduler final : public IService {
public:
    static TaskScheduler& Instance();

    Result<void> Initialize() override;
    Result<void> Shutdown() override;

    Result<TaskId> ScheduleOnce(std::function<void()> fn, std::chrono::microseconds delay);
    Result<TaskId> ScheduleEvery(std::function<void()> fn, std::chrono::microseconds period);
    Result<TaskId> ScheduleCron(std::function<void()> fn, const CronSpec& spec);
    Result<TaskId> ScheduleHeartbeat(std::function<void()> fn, std::chrono::microseconds period);

    // Countdown timer (07 §Countdown): `tick` fires every `period` until `total`
    // has elapsed, then `done` fires once and the task removes itself.
    Result<TaskId> ScheduleCountdown(std::function<void()> tick, std::chrono::microseconds period,
                                     std::chrono::microseconds total, std::function<void()> done);

    // Animation timer (07 §Animation): `tick` fires every `period` with a
    // normalized progress in [0,1] until `total` elapses; `done` fires once.
    // Microsecond resolution for 60/120/144 Hz work.
    Result<TaskId> ScheduleAnimation(std::function<void(double progress)> tick,
                                     std::chrono::microseconds period,
                                     std::chrono::microseconds total,
                                     std::function<void()> done = {});

    // Sequence / autoplay (07 §Autoplay): `step(i)` fires once for each i in
    // [0, items) at `perItem` intervals, then the task removes itself. The
    // handle supports the usual Pause / Resume / Cancel control.
    Result<TaskId> ScheduleSequence(std::function<void(size_t index)> step, size_t items,
                                    std::chrono::microseconds perItem);

    // Retry policy (07 §Retry): `fn` runs on `period`; consecutive failures beyond
    // `maxRetries` stop the task (logged). Success resets the failure counter.
    Result<TaskId> ScheduleWithRetry(std::function<void()> fn, std::chrono::microseconds period,
                                     int maxRetries = 3);

    Result<void> Cancel(TaskId id);
    Result<void> Pause(TaskId id);
    Result<void> Resume(TaskId id);
    Result<void> Reschedule(TaskId id, std::chrono::microseconds newPeriod);

    EngineTime Now() const { return bps::Now(); }
    size_t ScheduledCount() const;
    bool IsInitialized() const noexcept { return running_.load(); }

    const char* ServiceName() const noexcept override { return "TaskScheduler"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;

private:
    TaskScheduler() = default;

    struct Task {
        TaskId id;
        TaskKind kind;
        std::function<void()> fn;
        std::function<void()> done;                 // Countdown completion callback
        EngineTime next;
        EngineTime started;                          // Countdown epoch
        std::chrono::microseconds period{0};
        std::chrono::microseconds total{0};          // Countdown budget
        CronSpec cron;
        bool expired = false;                        // Countdown finished (timer thread)
        std::atomic<int> paused{0};
    };

    struct TaskOrder {
        bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) const {
            if (a->next != b->next) return a->next < b->next;
            return a->id < b->id;
        }
    };

    void TimerLoop();
    EngineTime ComputeNext(const Task& t, EngineTime from) const;
    static EngineTime NextCronFire(const CronSpec& spec, EngineTime from);
    Result<TaskId> Schedule(std::shared_ptr<Task> task, EngineTime when);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::set<std::shared_ptr<Task>, TaskOrder> timeline_;     // guarded by mutex_
    std::unordered_map<TaskId, std::shared_ptr<Task>> byId_;  // guarded by mutex_
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint64_t> nextId_{1};
};

} // namespace bps
