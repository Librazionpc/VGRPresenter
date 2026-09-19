#pragma once

// NotificationService (docs/specs/14): the ONLY system that decides if/when/
// where/how the user is informed. Subscribes to the EventBus, runs the pipeline
// (Factory → Rules → Queue → Manager → Providers), and never touches the UI.
// Fully headless-capable; providers are the only UI boundary.

#include "modules/notification/INotificationProvider.hpp"
#include "modules/notification/Notification.hpp"
#include "modules/notification/NotificationFactory.hpp"
#include "modules/notification/NotificationHistory.hpp"
#include "modules/notification/NotificationInterfaces.hpp"
#include "modules/notification/NotificationManager.hpp"
#include "modules/notification/NotificationQueue.hpp"
#include "modules/notification/NotificationRules.hpp"
#include "modules/notification/NotificationScheduler.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace bps::notification {

class NotificationService final : public IService {
public:
    static NotificationService& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "NotificationService"; }

    // --- Public API ---
    // Programmatic entry: build + deliver a notification (equivalent to an
    // event reaching the service).
    Result<uint64_t> Publish(const NotificationSeed& seed);
    // Deliver immediately (bypasses queue for direct UI feedback).
    Result<uint64_t> NotifyNow(Notification n);
    // Deliver after a delay via the scheduler.
    Result<uint64_t> NotifyAfter(Notification n, int64_t delayMs);

    // Progress: update an in-flight notification (no duplicates).
    Result<void> UpdateProgress(uint64_t id, double fraction);
    Result<void> Dismiss(uint64_t id);
    Result<void> MarkRead(uint64_t id);

    // --- Provider registry (delegated) ---
    Result<void> RegisterProvider(std::shared_ptr<INotificationProvider> p);
    Result<void> UnregisterProvider(std::string_view name);
    Result<void> SetProviderEnabled(std::string_view name, bool enabled);
    std::vector<std::string> ProviderNames() const;

    // --- Rules ---
    Result<void> RegisterRule(std::shared_ptr<INotificationRule> rule);
    Result<void> RegisterFormatter(std::string_view topic, std::string titleTemplate,
                                   std::string messageTemplate);

    // --- History ---
    std::vector<Notification> History(const HistoryQuery& q = {}) const;
    Result<void> ClearHistory();
    Result<void> ExportHistory(std::string_view hostPath) const;
    size_t HistoryCount() const;

    // --- Presentation mode (never interrupt a live presentation) ---
    void SetPresenting(bool presenting);
    bool Presenting() const { return presenting_.load(); }

    // --- User action (publishes NotificationActionInvoked; modules handle) ---
    Result<void> InvokeAction(uint64_t id, std::string_view actionId);

    // Internal heartbeat (scheduler tick + queue drain), driven by the Core
    // TaskScheduler so no module owns a thread.
    void Poll();

    // Test hooks.
    NotificationQueue& Queue() { return queue_; }
    NotificationHistory& HistoryStore() { return history_; }
    NotificationManager& Manager() { return manager_; }
    NotificationScheduler& Scheduler() { return scheduler_; }
    size_t ProviderCount() const;

private:
    NotificationService() = default;

    uint64_t NextId();
    Result<uint64_t> Deliver(const Notification& n, int64_t delayMs);
    void SeedToNotification(const NotificationSeed& seed, Notification& out) const;
    void ApplyPolicy(Notification& n) const;
    void WireEvents();

    std::atomic<bool> initialized_{false};
    std::atomic<bool> presenting_{false};
    std::atomic<uint64_t> nextId_{1};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> errors_{0};

    NotificationFactory factory_;
    NotificationQueue queue_;
    NotificationManager manager_;
    NotificationHistory history_;
    NotificationScheduler scheduler_;
    TemplateFormatter formatter_;
    ConfigNotificationFilter filter_;
    mutable std::mutex mutex_;
    std::string lastError_;
    std::vector<Subscription> subscriptions_;
    std::vector<std::shared_ptr<INotificationRule>> rules_;
    uint64_t pollTask_ = 0;
};

} // namespace bps::notification
