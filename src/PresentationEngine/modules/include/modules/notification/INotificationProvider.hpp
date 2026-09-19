#pragma once

// INotificationProvider (docs/specs/14 §Notification Providers): the Open/Closed
// extension point for notification destinations. The NotificationService only
// manages providers — it never creates a toast. Adding a destination (Discord,
// Slack, Email, OBS…) = implement this interface + register. No engine changes.

#include "modules/notification/Notification.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::notification {

// What a provider can render. The dispatcher checks these before routing.
struct ProviderCapabilities {
    bool supportsProgress = false;
    bool supportsActions = false;
    bool supportsAutoDismiss = false;
    bool supportsIcon = false;
    bool supportsPersistence = false;
    bool supportsGrouping = false;
};

class INotificationProvider {
public:
    virtual ~INotificationProvider() = default;

    virtual const char* Name() const noexcept = 0;
    // Channel ids this provider can deliver to, e.g. {"toast", "center"}.
    virtual std::vector<std::string> SupportedChannels() const = 0;
    virtual ProviderCapabilities Capabilities() const = 0;

    virtual Result<void> Initialize() { return Ok(); }
    virtual Result<void> Shutdown() { return Ok(); }

    // Show a notification on this provider. Provider failures must NOT affect
    // other providers (docs/specs/14 §Failure Modes).
    virtual Result<void> Show(const Notification& notification) = 0;
    // Update an existing notification (progress, message).
    virtual Result<void> Update(const Notification& notification) = 0;
    virtual Result<void> Dismiss(uint64_t notificationId) = 0;

    // Optional per-provider configuration (webhook URL, channel, …). Kept out
    // of the core service.
    virtual Result<void> Configure(const json::Value&) { return Ok(); }
};

} // namespace bps::notification
