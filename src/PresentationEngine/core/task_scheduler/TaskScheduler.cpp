#include "core/task_scheduler/TaskScheduler.hpp"

#include "core/logging/Logger.hpp"
#include "core/threading/ThreadPool.hpp"

#include <ctime>
#include <format>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod/timeEndPeriod (links winmm, already linked for WindowsAudio)
#endif

namespace bps {

TaskScheduler& TaskScheduler::Instance() {
    static TaskScheduler instance;
    return instance;
}

Result<void> TaskScheduler::Initialize() {
    if (running_.load()) return Ok();
    running_.store(true);
    shuttingDown_.store(false);
#if defined(_WIN32)
    // Windows' default timer resolution is ~15.6ms (64Hz) — every
    // condition-variable timed-wait/sleep in the timer loop below is
    // quantized to that, not the millisecond precision the API promises.
    // For most engine timers this just adds slop; it broke fast-cadence
    // consumers outright (a 10ms-period animation over a 45ms duration
    // could fire only 2 ticks instead of the promised >=4, since ticks
    // land ~15-16ms apart instead of ~10ms). timeBeginPeriod(1) raises the
    // whole PROCESS to 1ms resolution for as long as the scheduler runs;
    // matching timeEndPeriod(1) in Shutdown() releases it.
    timeBeginPeriod(1);
#endif
    thread_ = std::thread(&TaskScheduler::TimerLoop, this);
    return Ok();
}

Result<void> TaskScheduler::Shutdown() {
    if (!running_.load()) return Ok();
    shuttingDown_.store(true);
    {
        // Drop all pending tasks: recurring tasks are re-armed on every tick,
        // so a live recurring task (e.g. the platform OS-event watcher) would
        // otherwise keep the timer loop firing forever and this join would hang
        // (07 §Stop tasks).
        std::lock_guard<std::mutex> lock(mutex_);
        timeline_.clear();
        byId_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
#if defined(_WIN32)
    timeEndPeriod(1);   // pairs with Initialize()'s timeBeginPeriod(1)
#endif
    return Ok();
}

Result<TaskId> TaskScheduler::Schedule(std::shared_ptr<Task> task, EngineTime when) {
    task->next = when;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timeline_.insert(task);
        byId_[task->id] = task;
    }
    cv_.notify_all();
    return task->id;
}

Result<TaskId> TaskScheduler::ScheduleOnce(std::function<void()> fn,
                                           std::chrono::microseconds delay) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::OneShot;
    t->fn = std::move(fn);
    return Schedule(std::move(t), EngineClock::now() + delay);
}

Result<TaskId> TaskScheduler::ScheduleEvery(std::function<void()> fn,
                                            std::chrono::microseconds period) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    if (period.count() <= 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler", "period must be positive");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Recurring;
    t->fn = std::move(fn);
    t->period = period;
    return Schedule(std::move(t), EngineClock::now() + period);
}

Result<TaskId> TaskScheduler::ScheduleCron(std::function<void()> fn, const CronSpec& spec) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Cron;
    t->fn = std::move(fn);
    t->cron = spec;
    return Schedule(std::move(t), NextCronFire(spec, EngineClock::now()));
}

Result<TaskId> TaskScheduler::ScheduleHeartbeat(std::function<void()> fn,
                                                std::chrono::microseconds period) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Heartbeat;
    t->fn = std::move(fn);
    t->period = period;
    return Schedule(std::move(t), EngineClock::now() + period);
}

Result<TaskId> TaskScheduler::ScheduleCountdown(std::function<void()> tick,
                                                std::chrono::microseconds period,
                                                std::chrono::microseconds total,
                                                std::function<void()> done) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    if (period.count() <= 0 || total.count() <= 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler",
                           "countdown period and total must be positive");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Countdown;
    t->fn = std::move(tick);
    t->done = std::move(done);
    t->period = period;
    t->total = total;
    t->started = EngineClock::now();
    return Schedule(std::move(t), EngineClock::now() + period);
}

Result<TaskId> TaskScheduler::ScheduleAnimation(std::function<void(double)> tick,
                                                std::chrono::microseconds period,
                                                std::chrono::microseconds total,
                                                std::function<void()> done) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    if (period.count() <= 0 || total.count() <= 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler",
                           "animation period and total must be positive");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Animation;
    t->done = std::move(done);
    t->period = period;
    t->total = total;
    t->started = EngineClock::now();
    // Capture the deadline (a value), not `t` — the Task owns this lambda.
    EngineTime started = t->started;
    t->fn = [tick = std::move(tick), total, started]() {
        double elapsed = std::chrono::duration<double>(EngineClock::now() - started).count();
        double totalSecs = std::chrono::duration<double>(total).count();
        double p = totalSecs > 0.0 ? elapsed / totalSecs : 1.0;
        if (p > 1.0) p = 1.0;
        tick(p);
    };
    return Schedule(std::move(t), EngineClock::now() + period);
}

Result<TaskId> TaskScheduler::ScheduleSequence(std::function<void(size_t)> step, size_t items,
                                               std::chrono::microseconds perItem) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    if (perItem.count() <= 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler", "per-item period must be positive");
    if (items == 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler", "sequence must have at least one item");
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Sequence;
    t->period = perItem;
    auto index = std::make_shared<std::atomic<size_t>>(0);
    // Capture `id` (not the Task) to avoid a reference cycle; the task removes
    // itself after the last step.
    t->fn = [this, id = t->id, step = std::move(step), items, index]() {
        size_t i = index->fetch_add(1);
        if (i >= items) {
            (void)Cancel(id);   // spurious trailing fire (cancel race)
            return;
        }
        step(i);
        if (i + 1 >= items) (void)Cancel(id);
    };
    return Schedule(std::move(t), EngineClock::now() + perItem);
}

Result<TaskId> TaskScheduler::ScheduleWithRetry(std::function<void()> fn,
                                                std::chrono::microseconds period,
                                                int maxRetries) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "TaskScheduler", "not initialized");
    if (period.count() <= 0)
        return Error::Make(Err::InvalidArgument, "TaskScheduler", "period must be positive");
    if (maxRetries < 0) maxRetries = 0;
    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->kind = TaskKind::Recurring;
    t->period = period;
    auto failures = std::make_shared<std::atomic<int>>(0);
    // NOTE: capture `id` (not the Task shared_ptr) — the Task owns this lambda,
    // so capturing `t` would create a reference cycle and leak cancelled tasks.
    t->fn = [this, id = t->id, fn = std::move(fn), failures, maxRetries]() {
        try {
            fn();
            failures->store(0);
        } catch (const std::exception& ex) {
            if (failures->fetch_add(1) >= maxRetries) {
                Logger::Instance().Error(
                    std::format("TaskScheduler: task {} exceeded its retry budget ({}): {}",
                                id, maxRetries, ex.what()));
                (void)Cancel(id);
            }
        } catch (...) {
            if (failures->fetch_add(1) >= maxRetries) (void)Cancel(id);
        }
    };
    return Schedule(std::move(t), EngineClock::now() + period);
}

Result<void> TaskScheduler::Cancel(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Scheduler_TaskNotFound, "TaskScheduler", "task not found");
    timeline_.erase(it->second);
    byId_.erase(it);
    return Ok();
}

Result<void> TaskScheduler::Pause(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Scheduler_TaskNotFound, "TaskScheduler", "task not found");
    it->second->paused.store(1);
    timeline_.erase(it->second);
    return Ok();
}

Result<void> TaskScheduler::Resume(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Scheduler_TaskNotFound, "TaskScheduler", "task not found");
    auto t = it->second;
    if (t->paused.exchange(0) == 0) return Ok();
    t->next = EngineClock::now() + (t->period.count() > 0 ? t->period : std::chrono::microseconds(0));
    timeline_.insert(t);
    cv_.notify_all();
    return Ok();
}

Result<void> TaskScheduler::Reschedule(TaskId id, std::chrono::microseconds newPeriod) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Scheduler_TaskNotFound, "TaskScheduler", "task not found");
    auto t = it->second;
    if (t->kind == TaskKind::OneShot)
        return Error::Make(Err::InvalidState, "TaskScheduler", "one-shot tasks cannot be rescheduled");
    timeline_.erase(t);
    t->period = newPeriod;
    t->next = EngineClock::now() + newPeriod;
    timeline_.insert(t);
    cv_.notify_all();
    return Ok();
}

size_t TaskScheduler::ScheduledCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeline_.size();
}

EngineTime TaskScheduler::ComputeNext(const Task& t, EngineTime from) const {
    switch (t.kind) {
        case TaskKind::Recurring:
        case TaskKind::Heartbeat:
        case TaskKind::Countdown:
        case TaskKind::Animation:
        case TaskKind::Sequence:
            return from + t.period;
        case TaskKind::Cron:
            return NextCronFire(t.cron, from);
        default:
            return from;
    }
}

EngineTime TaskScheduler::NextCronFire(const CronSpec& spec, EngineTime from) {
    // Convert the steady-clock instant to a wall-clock struct tm.
    auto offset = from - EngineClock::now();
    auto sys = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(offset);
    std::time_t t0 = std::chrono::system_clock::to_time_t(sys);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t0);
#else
    localtime_r(&t0, &tmv);
#endif

    auto normalize = [](std::tm& tm) { return std::mktime(&tm); };

    // Search forward (bounded) for the next matching minute.
    for (int guard = 0; guard < 366 * 24 * 60; ++guard) {
        tmv.tm_sec = 0;
        if (spec.minute >= 0 && tmv.tm_min != spec.minute) {
            tmv.tm_min++;
            normalize(tmv);
            continue;
        }
        if (spec.hour >= 0 && tmv.tm_hour != spec.hour) {
            tmv.tm_min = 0;
            tmv.tm_hour++;
            normalize(tmv);
            continue;
        }
        if (spec.month >= 1 && (tmv.tm_mon + 1) != spec.month) {
            tmv.tm_min = 0;
            tmv.tm_hour = 0;
            tmv.tm_mday = 1;
            tmv.tm_mon++;
            normalize(tmv);
            continue;
        }
        bool domOk = spec.dayOfMonth < 0 || tmv.tm_mday == spec.dayOfMonth;
        bool dowOk = spec.dayOfWeek < 0 || tmv.tm_wday == spec.dayOfWeek;
        bool dayOk = (spec.dayOfMonth < 0 && spec.dayOfWeek < 0) || domOk || dowOk;
        if (!dayOk) {
            tmv.tm_min = 0;
            tmv.tm_hour = 0;
            tmv.tm_mday++;
            normalize(tmv);
            continue;
        }
        break;  // found
    }

    std::time_t nextT = normalize(tmv);
    auto nextSys = std::chrono::system_clock::from_time_t(nextT);
    auto nextSteady = EngineClock::now() +
                      std::chrono::duration_cast<EngineClock::duration>(nextSys - std::chrono::system_clock::now());
    if (nextSteady <= from) nextSteady = from + std::chrono::minutes(1);
    return nextSteady;
}

void TaskScheduler::TimerLoop() {
    while (true) {
        std::vector<std::shared_ptr<Task>> due;
        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Wait until the earliest due time. Recompute the deadline on every
            // wakeup so tasks scheduled mid-wait are picked up promptly.
            while (true) {
                if (shuttingDown_.load() && timeline_.empty()) break;
                EngineTime deadline = EngineClock::now() + std::chrono::hours(24);
                if (!timeline_.empty()) deadline = (*timeline_.begin())->next;
                cv_.wait_until(lock, deadline);
                if (!timeline_.empty() && (*timeline_.begin())->next <= EngineClock::now()) break;
                if (shuttingDown_.load() && timeline_.empty()) break;
                // Spurious wakeup or a newer earlier task: loop and recompute.
            }

            if (shuttingDown_.load() && timeline_.empty()) break;

            auto now = EngineClock::now();
            while (!timeline_.empty()) {
                auto first = *timeline_.begin();
                if (first->next > now) break;
                timeline_.erase(timeline_.begin());
                due.push_back(first);
            }

            if (shuttingDown_.load() && timeline_.empty() && due.empty()) break;

            // Re-arm recurring/cron/heartbeat/countdown/animation tasks; a
            // time-budgeted task (countdown/animation) whose budget is spent is
            // marked expired (its `done` runs at dispatch). Sequences keep
            // firing until their own final step cancels them.
            for (auto& t : due) {
                if (t->kind != TaskKind::OneShot && t->paused.load() == 0) {
                    if ((t->kind == TaskKind::Countdown || t->kind == TaskKind::Animation) &&
                        (now - t->started) >= t->total) {
                        t->expired = true;
                        continue;
                    }
                    // Anchor to the PREVIOUS deadline (t->next, still holding
                    // its just-fired value here), not `now` — computing from
                    // `now` adds however long this cycle's processing/
                    // dispatch/lock-contention took to every single period,
                    // so the effective cadence silently drifts slower than
                    // requested. Small and easy to miss on a scheduler with
                    // low wake-latency, but real: a 10ms-period animation
                    // over a 45ms window intermittently produced only 2
                    // ticks instead of >=4 under real thread-pool dispatch
                    // overhead. t->next and `now` are always within one
                    // loop iteration of each other here, so this changes
                    // nothing about which wall-clock minute Cron aligns to.
                    t->next = ComputeNext(*t, t->next);
                    timeline_.insert(t);
                }
            }
        }

        auto run = [this](TaskId id, const std::function<void()>& f) {
            if (ThreadPool::Instance().IsInitialized()) {
                TaskOptions opts;
                opts.taskClass = TaskClass::Background;
                opts.name = std::format("timer-{}", id);
                (void)ThreadPool::Instance().Submit([id, f]() {
                    try {
                        f();
                    } catch (const std::exception& ex) {
                        Logger::Instance().Error(std::format("TaskScheduler: task {} threw: {}",
                                                             id, ex.what()));
                    }
                }, opts);
            } else {
                try {
                    f();
                } catch (const std::exception& ex) {
                    Logger::Instance().Error(std::format("TaskScheduler: task {} threw: {}",
                                                         id, ex.what()));
                }
            }
        };

        for (auto& t : due) {
            if (t->paused.load() == 1) continue;
            if ((t->kind == TaskKind::Countdown || t->kind == TaskKind::Animation) &&
                t->expired) {
                if (t->done) run(t->id, t->done);   // completion callback, then gone
                continue;
            }
            run(t->id, t->fn);
        }
    }
}

HealthReport TaskScheduler::GetHealth() const {
    HealthReport r;
    r.state = running_.load() ? HealthState::Healthy : HealthState::Failing;
    r.detail = std::format("{} scheduled tasks", ScheduledCount());
    return r;
}

Metrics TaskScheduler::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = ScheduledCount();
    m.threadCount = 1;   // the timer thread
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
