#include "modules/notification/NotificationService.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "modules/notification/NotificationProviders.hpp"

#include <algorithm>
#include <chrono>

namespace bps::notification {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

NotificationService& NotificationService::Instance() {
    static NotificationService instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> NotificationService::Initialize() {
    if (initialized_.load()) return Ok();
    auto& config = ConfigurationManager::Instance();

    size_t maxQueue = static_cast<size_t>(config.GetInt("notify.queue.max", 4096));
    size_t rate = static_cast<size_t>(config.GetInt("notify.queue.ratePerSecond", 200));
    queue_.Configure(maxQueue, rate);
    queue_.SetGroupingWindowMs(config.GetInt("notify.queue.groupWindowMs", 5000));

    size_t histMax = static_cast<size_t>(config.GetInt("notify.history.maxEntries", 1000));
    if (auto mem = std::dynamic_pointer_cast<MemoryNotificationStorage>(history_.Storage()))
        mem->SetMaxEntries(histMax);

    // Default providers: Console + Center (+ StatusBar for the UI layer).
    auto console = std::make_shared<ConsoleNotificationProvider>();
    auto center = std::make_shared<CenterNotificationProvider>();
    auto status = std::make_shared<StatusBarNotificationProvider>();
    if (auto r = manager_.RegisterProvider(console); !r.ok()) return r;
    if (auto r = manager_.RegisterProvider(center); !r.ok()) return r;
    if (auto r = manager_.RegisterProvider(status); !r.ok()) return r;

    // Optional webhook provider: only registered when a destination is
    // configured (notify.webhook.url), so default runs never see network
    // errors from an unconfigured endpoint (docs/specs/14 §Providers).
    if (config.Has("notify.webhook.url")) {
        auto webhook = std::make_shared<WebhookNotificationProvider>();
        json::Value::Object wh;
        wh["url"] = json::Value::String(config.GetString("notify.webhook.url"));
        if (auto r = webhook->Configure(json::Value(std::move(wh))); !r.ok())
            return r;
        if (auto r = manager_.RegisterProvider(webhook); !r.ok()) return r;
    }

    // Default policy rules (docs/specs/14 §Policy Engine) + factory.
    if (auto r = RegisterRule(MakeDefaultRules()); !r.ok()) return r;

    scheduler_.SetDeliver([this](const Notification& n) {
        auto copy = n;
        ApplyPolicy(copy);
        if (queue_.Push(copy)) ++delivered_;
    });

    // Default formatter templates (localizable later).
    (void)formatter_.RegisterTemplate("content.asset_imported",
                                      "Import finished", "Imported {message}");
    (void)formatter_.RegisterTemplate("content.asset_deleted",
                                      "Asset deleted", "{message}");
    (void)formatter_.RegisterTemplate("content.validation_failed",
                                      "Validation failed", "{message}");
    (void)formatter_.RegisterTemplate("project.saved",
                                      "Project saved", "Project '{message}' saved successfully.");

    WireEvents();

    // Scheduler heartbeat via the Core TaskScheduler (no module owns a thread).
    auto& sched = TaskScheduler::Instance();
    auto r = sched.ScheduleEvery([this]() { Poll(); }, std::chrono::milliseconds(500));
    if (r.ok()) pollTask_ = r.value();

    initialized_.store(true);
    Logger::Instance().Info("NotificationService ready: " +
                                std::format("{} providers", manager_.ProviderCount()),
                            "Notify");
    return Ok();
}

Result<void> NotificationService::Start() { return Ok(); }

Result<void> NotificationService::Stop() {
    if (pollTask_ != 0) {
        (void)TaskScheduler::Instance().Cancel(pollTask_);
        pollTask_ = 0;
    }
    return Ok();
}

Result<void> NotificationService::Shutdown() {
    if (!initialized_.load()) return Ok();
    (void)Stop();
    auto& bus = EventBus::Instance();
    for (auto& sub : subscriptions_) {
        if (sub.Valid()) (void)bus.Unsubscribe(sub);
    }
    subscriptions_.clear();
    queue_.Clear();
    scheduler_.Clear();
    (void)history_.Clear();
    for (const auto& name : manager_.ProviderNames()) (void)manager_.UnregisterProvider(name);
    initialized_.store(false);
    return Ok();
}

Result<void> NotificationService::Reload() { return Ok(); }

Result<void> NotificationService::Reset() {
    (void)Shutdown();
    return Ok();
}

HealthReport NotificationService::GetHealth() const {
    HealthReport h;
    h.state = errors_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    h.detail = std::format("{} providers, {} in history, {} queued",
                           manager_.ProviderCount(), history_.Count(), queue_.Pending());
    h.lastError = lastError_;
    h.errorCount = errors_.load();
    return h;
}

Metrics NotificationService::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = queue_.Pending();
    m.errorCount = errors_.load();
    m.health = errors_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    return m;
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

uint64_t NotificationService::NextId() { return nextId_.fetch_add(1); }

void NotificationService::SeedToNotification(const NotificationSeed& seed,
                                             Notification& out) const {
    out.id = 0;
    out.correlationId = seed.correlationId;
    out.timestampMs = NowMs();
    out.sourceModule = seed.sourceModule;
    out.sourceEvent = seed.sourceEvent;
    out.severity = seed.severity;
    out.category = seed.category;
    out.priority = seed.priority;
    out.persistent = seed.persistent;
    out.progress = seed.progress;
    out.actions = seed.actions;
    out.tags = seed.tags;
    out.groupKey = seed.groupKey;
    out.autoDismissMs = seed.autoDismissMs;
    formatter_.Format(seed, out.title, out.message);
    if (out.title.empty()) out.title = seed.title;
    if (out.message.empty()) out.message = seed.message;
}

void NotificationService::ApplyPolicy(Notification& n) const {
    // Rules first (registration order).
    NotificationSeed seed;
    seed.sourceModule = n.sourceModule;
    seed.sourceEvent = n.sourceEvent;
    seed.severity = n.severity;
    seed.category = n.category;
    seed.priority = n.priority;
    for (const auto& rule : rules_) {
        auto policy = rule->Evaluate(seed);
        if (!policy) continue;
        if (!policy->show) {
            n.dismissed = true;
            return;
        }
        if (!policy->channels.empty()) n.channels = policy->channels;
        if (policy->priority != Priority::Normal) n.priority = policy->priority;
        if (policy->autoDismissMs > 0) n.autoDismissMs = policy->autoDismissMs;
        if (policy->persistent) n.persistent = true;
        if (policy->suppressInPresenting && presenting_.load()) {
            // Never interrupt a live presentation: downgrade to log-only for
            // the duration of the presentation (the user sees it in the
            // notification center afterwards if it remains relevant).
            n.channels = {"console"};
        }
        return;
    }
    // No rule: default policy by severity.
    NotificationPolicy p = DefaultPolicyFor(n.severity);
    if (n.channels.empty()) n.channels = p.channels;
    if (n.autoDismissMs == 0) n.autoDismissMs = p.autoDismissMs;
    if (p.persistent) n.persistent = true;
    if (presenting_.load() && n.severity < Severity::Warning) {
        n.channels = {"console"};
    }
}

Result<uint64_t> NotificationService::Publish(const NotificationSeed& seed) {
    if (!initialized_.load())
        return Error::Make(Err::InvalidState, "Notify", "service not initialized");
    Notification n;
    SeedToNotification(seed, n);
    n.id = NextId();
    ApplyPolicy(n);
    if (!filter_.Allow(n)) return Result<uint64_t>{0};
    return Deliver(n, 0);
}

Result<uint64_t> NotificationService::NotifyNow(Notification n) {
    if (n.id == 0) n.id = NextId();
    if (n.timestampMs == 0) n.timestampMs = NowMs();
    ApplyPolicy(n);
    if (!filter_.Allow(n)) return Result<uint64_t>{0};
    return Deliver(n, 0);
}

Result<uint64_t> NotificationService::NotifyAfter(Notification n, int64_t delayMs) {
    if (n.id == 0) n.id = NextId();
    if (n.timestampMs == 0) n.timestampMs = NowMs();
    ApplyPolicy(n);
    if (!filter_.Allow(n)) return Result<uint64_t>{0};
    scheduler_.Schedule(n, std::max<int64_t>(0, delayMs));
    return Result<uint64_t>{n.id};
}

Result<uint64_t> NotificationService::Deliver(const Notification& n, int64_t) {
    auto copy = n;
    if (queue_.Push(copy)) {
        ++delivered_;
        // Drain immediately (the queue is the async buffer; Poll drains later).
        Poll();
    }
    return Result<uint64_t>{copy.id};
}

Result<void> NotificationService::UpdateProgress(uint64_t id, double fraction) {
    bool updated = queue_.Update(id, [fraction](Notification& n) {
        n.progress = fraction;
        n.message = std::format("{}%", static_cast<int>(fraction * 100));
    });
    if (updated) {
        // Dispatch the mutated entry to every supporting provider (progress
        // updates never create new notifications — the id stays the same).
        auto cur = queue_.Find(id);
        if (cur) (void)manager_.DispatchUpdate(*cur);
    }
    return Ok();
}

Result<void> NotificationService::Dismiss(uint64_t id) {
    (void)queue_.Remove(id);
    (void)manager_.DispatchDismiss(id);
    (void)history_.Dismiss(id);
    return Ok();
}

Result<void> NotificationService::MarkRead(uint64_t id) {
    return history_.MarkRead(id);
}

Result<void> NotificationService::RegisterProvider(std::shared_ptr<INotificationProvider> p) {
    return manager_.RegisterProvider(std::move(p));
}

Result<void> NotificationService::UnregisterProvider(std::string_view name) {
    return manager_.UnregisterProvider(name);
}

Result<void> NotificationService::SetProviderEnabled(std::string_view name, bool enabled) {
    return manager_.SetProviderEnabled(name, enabled);
}

std::vector<std::string> NotificationService::ProviderNames() const {
    return manager_.ProviderNames();
}

Result<void> NotificationService::RegisterRule(std::shared_ptr<INotificationRule> rule) {
    if (!rule) return Error::Make(Err::InvalidArgument, "Notify", "null rule");
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.push_back(std::move(rule));
    return Ok();
}

Result<void> NotificationService::RegisterFormatter(std::string_view topic,
                                                    std::string titleTemplate,
                                                    std::string messageTemplate) {
    return formatter_.RegisterTemplate(topic, std::move(titleTemplate),
                                       std::move(messageTemplate));
}

std::vector<Notification> NotificationService::History(const HistoryQuery& q) const {
    return history_.Query(q);
}

Result<void> NotificationService::ClearHistory() { return history_.Clear(); }

Result<void> NotificationService::ExportHistory(std::string_view hostPath) const {
    return history_.Export(hostPath);
}

size_t NotificationService::HistoryCount() const { return history_.Count(); }

void NotificationService::SetPresenting(bool presenting) { presenting_.store(presenting); }

Result<void> NotificationService::InvokeAction(uint64_t id, std::string_view actionId) {
    auto& bus = EventBus::Instance();
    // Look up correlationId from history (best effort).
    std::string corr;
    for (const auto& n : history_.All()) {
        if (n.id == id) {
            corr = n.correlationId;
            break;
        }
    }
    (void)bus.Publish(events::NotificationActionInvoked{id, std::string(actionId), corr});
    return Ok();
}

size_t NotificationService::ProviderCount() const { return manager_.ProviderCount(); }

void NotificationService::Poll() {
    // Deliver any scheduled notifications that are due.
    scheduler_.Tick(NowMs());
    // Drain the queue into history + providers.
    while (auto n = queue_.Pop()) {
        (void)history_.Record(*n);
        (void)manager_.Dispatch(*n);
    }
}

// ---------------------------------------------------------------------------
// EventBus wiring: engine events → notification seeds (docs/specs/14 §Consumes)
// ---------------------------------------------------------------------------

void NotificationService::WireEvents() {
    auto& bus = EventBus::Instance();

    subscriptions_.push_back(bus.Subscribe<events::ContentAssetImported>(
        [this](const events::ContentAssetImported& e) {
            (void)Publish(factory_.ImportFinished(e.name, e.assetsCreated, e.uuid));
        }));
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetDeleted>(
        [this](const events::ContentAssetDeleted& e) {
            (void)Publish(factory_.AssetDeleted(e.uuid));
        }));
    subscriptions_.push_back(bus.Subscribe<events::ContentValidationFailed>(
        [this](const events::ContentValidationFailed& e) {
            (void)Publish(factory_.ValidationFailed(e.reason));
        }));
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureHigh>(
        [this](const events::ResourcePressureHigh& e) {
            (void)Publish(factory_.ResourcePressure(e.resource, ToString(e.level)));
        }));
    subscriptions_.push_back(bus.Subscribe<events::KernelPanic>(
        [this](const events::KernelPanic& e) {
            (void)Publish(factory_.EngineCrash(e.error.message));
        }));
    subscriptions_.push_back(bus.Subscribe<events::MonitorConnected>(
        [this](const events::MonitorConnected& e) {
            (void)Publish(factory_.DisplayConnected(e.monitorId));
        }));
    subscriptions_.push_back(bus.Subscribe<events::MonitorDisconnected>(
        [this](const events::MonitorDisconnected& e) {
            (void)Publish(factory_.DisplayRemoved(e.monitorId));
        }));
    subscriptions_.push_back(bus.Subscribe<events::ProjectSaved>(
        [this](const events::ProjectSaved& e) {
            (void)Publish(factory_.ProjectSaved(e.name, e.autosave));
        }));
    subscriptions_.push_back(bus.Subscribe<events::ProjectCreated>(
        [this](const events::ProjectCreated& e) {
            (void)Publish(factory_.ProjectCreated(e.name));
        }));
    subscriptions_.push_back(bus.Subscribe<events::RecoveryAvailable>(
        [this](const events::RecoveryAvailable& e) {
            (void)Publish(factory_.RecoveryAvailable(e.detail));
        }));
    subscriptions_.push_back(bus.Subscribe<events::PackageExported>(
        [this](const events::PackageExported& e) {
            (void)Publish(factory_.PackageExported(e.path));
        }));
    subscriptions_.push_back(bus.Subscribe<events::BackupCompleted>(
        [this](const events::BackupCompleted& e) {
            (void)Publish(factory_.BackupCompleted(e.destination));
        }));
}


} // namespace bps::notification
