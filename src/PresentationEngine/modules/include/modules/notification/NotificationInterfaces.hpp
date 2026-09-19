#pragma once

// Additional Notification interfaces (docs/specs/14 §Interfaces):
// INotificationRule (policy), INotificationFormatter (templates + localization),
// INotificationStorage (history persistence), INotificationFilter.

#include "modules/notification/Notification.hpp"
#include "core/config/Json.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bps::notification {

// --- Rules / Policy Engine ------------------------------------------------
// A rule decides: show?, which channels, priority, auto-dismiss, persistent,
// suppress-in-presenting. Rules are evaluated in registration order; the first
// matching rule wins, then the default policy applies.
struct NotificationPolicy {
    bool show = true;
    std::vector<std::string> channels;   // empty = provider default mapping
    Priority priority = Priority::Normal;
    int64_t autoDismissMs = 0;           // 0 = severity default
    bool persistent = false;
    bool suppressInPresenting = false;
};

class INotificationRule {
public:
    virtual ~INotificationRule() = default;
    virtual const char* Name() const noexcept = 0;
    // Return a policy when this rule matches; nullopt = not applicable.
    virtual std::optional<NotificationPolicy> Evaluate(
        const NotificationSeed& seed) const = 0;
};

// Default severity→policy mapping used when no rule matches.
NotificationPolicy DefaultPolicyFor(Severity s);

// --- Formatter -------------------------------------------------------------
// Modules never build user-facing strings; the formatter owns them
// (docs/specs/14 §Notification Formatter). Enables localization later.
class INotificationFormatter {
public:
    virtual ~INotificationFormatter() = default;
    // Returns {title, message} for a seed. The default formatter uses
    // registered per-topic templates, falling back to the seed's own strings.
    virtual void Format(const NotificationSeed& seed, std::string& title,
                        std::string& message) const = 0;
};

class TemplateFormatter final : public INotificationFormatter {
public:
    void Format(const NotificationSeed& seed, std::string& title,
                std::string& message) const override;

    // Template syntax: "{title}" / "{message}" / "{module}" / "{event}".
    Result<void> RegisterTemplate(std::string_view topic, std::string titleTemplate,
                                  std::string messageTemplate);
    void Clear();

private:
    mutable std::mutex mutex_;
    struct Tmpl {
        std::string title;
        std::string message;
    };
    std::map<std::string, Tmpl, std::less<>> templates_;
};

// --- Storage ----------------------------------------------------------------
// History persistence: memory + persistent (DatabaseManager-backed) so history
// can survive restarts (docs/specs/14 §Notification Storage).
class INotificationStorage {
public:
    virtual ~INotificationStorage() = default;
    virtual Result<void> Append(const Notification& n) = 0;
    virtual Result<void> Update(const Notification& n) = 0;
    virtual Result<void> Remove(uint64_t id) = 0;
    virtual Result<std::vector<Notification>> All() const = 0;
    virtual Result<void> Clear() = 0;
    virtual size_t Count() const = 0;
};

class MemoryNotificationStorage final : public INotificationStorage {
public:
    Result<void> Append(const Notification& n) override;
    Result<void> Update(const Notification& n) override;
    Result<void> Remove(uint64_t id) override;
    Result<std::vector<Notification>> All() const override;
    Result<void> Clear() override;
    size_t Count() const override;
    void SetMaxEntries(size_t max) { maxEntries_ = max; }

private:
    mutable std::mutex mutex_;
    std::map<uint64_t, Notification, std::greater<>> byId_;   // newest first
    size_t maxEntries_ = 1000;
};

// --- Filter -----------------------------------------------------------------
// Users can suppress categories/severities (docs/specs/14 §Notification Filters).
class INotificationFilter {
public:
    virtual ~INotificationFilter() = default;
    virtual bool Allow(const Notification& n) const = 0;
};

class ConfigNotificationFilter final : public INotificationFilter {
public:
    bool Allow(const Notification& n) const override;
};

} // namespace bps::notification
