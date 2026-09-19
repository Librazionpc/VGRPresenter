#include "core/threading/ThreadPool.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"
#include <format>

namespace bps {

// Out-of-line definition of the thread_local declared in ThreadPool.hpp —
// see that declaration's comment for why it isn't `inline` there.
thread_local std::shared_ptr<ThreadPool::Task> ThreadPool::tlsCurrentTask_;

ThreadPool& ThreadPool::Instance() {
    static ThreadPool instance;
    return instance;
}

Result<void> ThreadPool::Initialize(size_t workerCount) {
    if (initialized_.load()) return Ok();
    if (workerCount == 0) workerCount = std::thread::hardware_concurrency();
    if (workerCount < 1) workerCount = 1;
    desiredWorkers_.store(workerCount);
    initialized_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i)
            workers_.emplace_back(&ThreadPool::WorkerLoop, this);
    }
    Logger::Instance().Info("ThreadPool initialized with " + std::to_string(workerCount) +
                            " workers");
    return Ok();
}

Result<void> ThreadPool::Shutdown() {
    if (!initialized_.load()) return Ok();
    shuttingDown_.store(true);
    cv_.notify_all();
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toJoin.swap(workers_);
        queue_ = std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>,
                                     TaskCmp>();
    }
    for (auto& t : toJoin)
        if (t.joinable()) t.join();
    initialized_.store(false);
    shuttingDown_.store(false);
    return Ok();
}

Result<TaskHandle> ThreadPool::Submit(std::function<void()> task, const TaskOptions& opts) {
    if (!initialized_.load())
        return Error::Make(Err::InvalidState, "ThreadPool", "not initialized");
    if (shuttingDown_.load())
        return Error::Make(Err::InvalidState, "ThreadPool", "shutting down");
    if (!task)
        return Error::Make(Err::InvalidArgument, "ThreadPool", "null task");

    auto t = std::make_shared<Task>();
    t->id = nextId_.fetch_add(1);
    t->klass = opts.taskClass;
    t->priority = opts.priority;
    t->fn = std::move(task);
    t->enqueuedAt = EngineClock::now();
    t->timeout = opts.timeout;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueued)
            return Error::Make(Err::ThreadPool_Backpressure, "ThreadPool", "queue full");
        queue_.push(t);
        byId_[t->id] = t;
    }
    cv_.notify_one();
    return t->id;
}

Result<TaskHandle> ThreadPool::SubmitBackground(std::function<void()> task, int priority) {
    TaskOptions opts;
    opts.taskClass = TaskClass::Background;
    opts.priority = priority;
    return Submit(std::move(task), opts);
}

Result<TaskHandle> ThreadPool::SubmitRealtime(std::function<void()> task, int priority) {
    TaskOptions opts;
    opts.taskClass = TaskClass::Realtime;
    opts.priority = priority;
    return Submit(std::move(task), opts);
}

Result<void> ThreadPool::Cancel(TaskHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(handle);
    if (it == byId_.end())
        return Error::Make(Err::NotFound, "ThreadPool", "task not found");
    it->second->cancelRequested.store(true);
    // A task still queued becomes Cancelled immediately; the worker observes
    // cancelRequested when it pops it. A dispatched task keeps its Running state.
    if (it->second->state.load() == static_cast<int>(TaskState::Queued))
        it->second->state.store(static_cast<int>(TaskState::Cancelled));
    return Ok();
}

Result<void> ThreadPool::SetWorkerAffinity(size_t workerIndex, int cpu) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workerIndex >= workers_.size())
        return Error::Make(Err::NotFound, "ThreadPool", "worker index out of range");
    if (cpu < 0) return Error::Make(Err::InvalidArgument, "ThreadPool", "cpu must be >= 0");
    // Affinity control goes through the PAL (Phase 2): pthread_setaffinity_np
    // lives in platform/linux/LinuxThreading.cpp, never here.
    auto r = platform::PlatformAccessor::Get().Threading().SetThreadAffinity(
        workers_[workerIndex], static_cast<unsigned>(cpu));
    if (!r.ok())
        return Error::Make(Err::Unsupported, "ThreadPool",
                           std::format("CPU affinity failed for cpu {}: {}", cpu,
                                        r.error().message));
    return Ok();
}

Result<void> ThreadPool::Resize(size_t workers) {
    if (workers < 1) workers = 1;
    size_t previous = desiredWorkers_.exchange(workers);
    if (workers > previous) {
        size_t toAdd = workers - previous;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < toAdd; ++i)
            workers_.emplace_back(&ThreadPool::WorkerLoop, this);
    }
    cv_.notify_all();  // excess workers observe the shrink and exit
    return Ok();
}

bool ThreadPool::CurrentTaskCancelled() {
    return tlsCurrentTask_ && tlsCurrentTask_->cancelRequested.load();
}

TaskHandle ThreadPool::CurrentTask() {
    return tlsCurrentTask_ ? tlsCurrentTask_->id : 0;
}

size_t ThreadPool::WorkerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

size_t ThreadPool::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::optional<TaskState> ThreadPool::TaskStateOf(TaskHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(handle);
    if (it == byId_.end()) return std::nullopt;
    return static_cast<TaskState>(it->second->state.load());
}

std::shared_ptr<ThreadPool::Task> ThreadPool::FindTask(TaskHandle id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

void ThreadPool::WorkerLoop() {
    while (true) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return shuttingDown_.load() || !queue_.empty() ||
                       workers_.size() > desiredWorkers_.load();
            });
            // Cooperative shrink: exit if idle and above the desired count.
            if (queue_.empty() && workers_.size() > desiredWorkers_.load() && !shuttingDown_.load()) {
                auto me = std::this_thread::get_id();
                for (auto it = workers_.begin(); it != workers_.end(); ++it) {
                    if (it->get_id() == me) {
                        it->detach();
                        workers_.erase(it);
                        break;
                    }
                }
                return;
            }
            if (queue_.empty() && shuttingDown_.load()) return;

            task = queue_.top();
            queue_.pop();
            if (task->cancelRequested.load()) {
                byId_.erase(task->id);
                task->state.store(static_cast<int>(TaskState::Cancelled));
                cancelled_.fetch_add(1);
                continue;
            }
            // Timeout check at dispatch.
            if (task->timeout.count() > 0 &&
                (EngineClock::now() - task->enqueuedAt) > task->timeout) {
                byId_.erase(task->id);
                task->state.store(static_cast<int>(TaskState::TimedOut));
                continue;
            }
        }

        task->state.store(static_cast<int>(TaskState::Running));
        tlsCurrentTask_ = task;
        try {
            task->fn();
            task->state.store(static_cast<int>(TaskState::Finished));
        } catch (const std::exception& ex) {
            task->state.store(static_cast<int>(TaskState::Failed));
            Logger::Instance().Error("ThreadPool: task " + std::to_string(task->id) +
                                     " failed: " + ex.what());
        } catch (...) {
            task->state.store(static_cast<int>(TaskState::Failed));
        }
        tlsCurrentTask_ = nullptr;
        executed_.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            byId_.erase(task->id);
        }
    }
}

HealthReport ThreadPool::GetHealth() const {
    HealthReport r;
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Failing;
    r.detail = std::format("{} workers, {} pending, {} executed", workers_.size(),
                           PendingCount(), executed_.load());
    return r;
}

Metrics ThreadPool::MetricsSnapshot() const {
    Metrics m;
    m.cpuPct = 0.0;
    m.queueLength = PendingCount();
    m.threadCount = WorkerCount();
    m.errorCount = 0;
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
