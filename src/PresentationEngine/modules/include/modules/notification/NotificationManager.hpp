#pragma once

// NotificationManager (docs/specs/14 §Notification Manager): the provider
// registry + dispatcher. Knows nothing about concrete providers — it only
// registers, enables, finds, and routes to them (Manager → Registry →
// Interface → Providers).

#include "modules/notification/INotificationProvider.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::notification {

class NotificationManager {
public:
    NotificationManager() = default;

    // --- Provider registry ---
    Result<void> RegisterProvider(std::shared_ptr<INotificationProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);
    Result<void> SetProviderEnabled(std::string_view name, bool enabled);
    bool IsProviderEnabled(std::string_view name) const;
    std::shared_ptr<INotificationProvider> FindProvider(std::string_view name) const;
    std::vector<std::string> ProviderNames() const;
    size_t ProviderCount() const;

    // --- Dispatch ---
    // Route a notification to every enabled provider that supports one of its
    // channels. A failing provider must not affect the others.
    Result<void> Dispatch(const Notification& n);
    Result<void> DispatchUpdate(const Notification& n);   // progress updates
    Result<void> DispatchDismiss(uint64_t id);

    Result<void> ConfigureAll(const std::map<std::string, json::Value>& configs);

private:
    struct ProviderEntry {
        std::shared_ptr<INotificationProvider> provider;
        bool enabled = true;
    };
    std::shared_ptr<ProviderEntry> FindEntryLocked(std::string_view name) const;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ProviderEntry>, std::less<>> providers_;
};

} // namespace bps::notification
