#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bps {

// Task classes (docs/specs/06 §3). Realtime first, Background throttled.
enum class TaskClass : int { Realtime = 0, Foreground, Background };

struct TaskOptions {
    TaskClass taskClass = TaskClass::Foreground;
    int priority = 0;                          // higher = sooner within a class
    std::string name;                          // diagnostics
    bool cancellable = true;
    std::chrono::microseconds timeout{0};      // 0 = none (v1: enforced at dispatch)
};

using TaskHandle = uint64_t;

enum class TaskState : int {
    Queued = 0,
    Running,
    Blocked,
    Cancelled,
    TimedOut,
    Failed,
    Finished
};

// Central execution fabric (docs/specs/06). No module owns threads.
class ThreadPool final : public IService {
public:
    static ThreadPool& Instance();

    Result<void> Initialize(size_t workerCount = 0);   // 0 = hardware_concurrency
    Result<void> Initialize() override { return Initialize(0); }   // engine-wide contract (00 §10)
    Result<void> Shutdown() override;

    Result<TaskHandle> Submit(std::function<void()> task, const TaskOptions& opts = {});
    Result<TaskHandle> SubmitBackground(std::function<void()> task, int priority = 0);
    Result<TaskHandle> SubmitRealtime(std::function<void()> task, int priority = 0);

    template <class F, class... Args>
    Result<std::future<std::invoke_result_t<F, Args...>>> SubmitFuture(TaskClass klass, int priority,
                                                                       F&& f, Args&&... args) {
        using R = std::invoke_result_t<F, Args...>;
        auto pkg = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = pkg->get_future();
        TaskOptions opts;
        opts.taskClass = klass;
        opts.priority = priority;
        auto res = Submit([pkg]() { (*pkg)(); }, opts);
        if (!res.ok()) return res.error();
        return fut;
    }

    Result<void> Cancel(TaskHandle handle);           // cooperative (06 §5)
    Result<void> Resize(size_t workers);              // dynamic grow/shrink

    // CPU affinity (06 §4): pin worker[workerIndex] to a physical CPU. Returns
    // Unsupported on platforms without thread affinity control.
    Result<void> SetWorkerAffinity(size_t workerIndex, int cpu);

    static bool CurrentTaskCancelled();               // cooperative check inside a task
    static TaskHandle CurrentTask();

    size_t WorkerCount() const;
    size_t PendingCount() const;
    size_t ExecutedCount() const noexcept { return executed_.load(); }
    bool IsInitialized() const noexcept { return initialized_.load(); }
    std::optional<TaskState> TaskStateOf(TaskHandle handle) const;

    const char* ServiceName() const noexcept override { return "ThreadPool"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const;

private:
    ThreadPool() = default;

    struct Task {
        TaskHandle id;
        TaskClass klass;
        int priority;
        std::function<void()> fn;
        EngineTime enqueuedAt;
        std::chrono::microseconds timeout{0};
        std::atomic<bool> cancelRequested{false};
        std::atomic<int> state{static_cast<int>(TaskState::Queued)};
    };

    void WorkerLoop();
    std::shared_ptr<Task> FindTask(TaskHandle id) const;

    // Declared, not `static inline thread_local` — MinGW-w64's linker
    // (binutils ld, PE/COFF) doesn't fold the compiler-generated TLS init
    // thunk for an inline thread_local across translation units the way
    // ELF linkers do, producing a real "multiple definition of TLS init
    // function" link error the moment more than one .cpp using this class
    // gets linked together. The single out-of-line definition in
    // ThreadPool.cpp sidesteps it entirely (same fix shape as the
    // pre-C++17 static-member-definition pattern, just for a TLS member).
    static thread_local std::shared_ptr<Task> tlsCurrentTask_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    struct TaskCmp {
        bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) const {
            if (a->klass != b->klass)
                return static_cast<int>(a->klass) > static_cast<int>(b->klass);
            if (a->priority != b->priority) return a->priority < b->priority;
            return a->id > b->id;
        }
    };
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskCmp> queue_;
    std::unordered_map<TaskHandle, std::shared_ptr<Task>> byId_;
    std::vector<std::thread> workers_;
    std::atomic<size_t> desiredWorkers_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint64_t> nextId_{1};
    std::atomic<uint64_t> executed_{0};
    std::atomic<uint64_t> cancelled_{0};
    static constexpr size_t kMaxQueued = 100000;
};

} // namespace bps
