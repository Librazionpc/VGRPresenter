#include "modules/notification/NotificationManager.hpp"

#include <algorithm>

namespace bps::notification {

Result<void> NotificationManager::RegisterProvider(
    std::shared_ptr<INotificationProvider> provider) {
    if (!provider)
        return Error::Make(Err::InvalidArgument, "Notify", "null provider");
    std::lock_guard<std::mutex> lock(mutex_);
    std::string name = provider->Name();
    if (providers_.count(name))
        return Error::Make(Err::AlreadyExists, "Notify",
                           "provider already registered: " + name);
    providers_[std::move(name)] = std::make_shared<ProviderEntry>(
        ProviderEntry{std::move(provider), true});
    return Ok();
}

Result<void> NotificationManager::UnregisterProvider(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(std::string(name));
    if (it == providers_.end())
        return Error::Make(Err::NotFound, "Notify",
                           "provider not registered: " + std::string(name));
    providers_.erase(it);
    return Ok();
}

Result<void> NotificationManager::SetProviderEnabled(std::string_view name, bool enabled) {
    auto entry = FindEntryLocked(name);
    if (!entry)
        return Error::Make(Err::NotFound, "Notify",
                           "provider not registered: " + std::string(name));
    entry->enabled = enabled;
    return Ok();
}

bool NotificationManager::IsProviderEnabled(std::string_view name) const {
    auto entry = FindEntryLocked(name);
    return entry && entry->enabled;
}

std::shared_ptr<INotificationProvider> NotificationManager::FindProvider(
    std::string_view name) const {
    auto entry = FindEntryLocked(name);
    return entry ? entry->provider : nullptr;
}

std::vector<std::string> NotificationManager::ProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, entry] : providers_) {
        (void)entry;
        out.push_back(name);
    }
    return out;
}

size_t NotificationManager::ProviderCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.size();
}

Result<void> NotificationManager::Dispatch(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool routedAny = false;
    for (const auto& [name, entry] : providers_) {
        (void)name;
        if (!entry->enabled) continue;
        // Does this provider support any of the notification's channels?
        bool supports = false;
        for (const auto& ch : entry->provider->SupportedChannels()) {
            if (std::find(n.channels.begin(), n.channels.end(), ch) != n.channels.end()) {
                supports = true;
                break;
            }
        }
        if (!supports) continue;
        routedAny = true;
        auto r = entry->provider->Show(n);
        if (!r.ok()) {
            // Provider failure must not affect other providers.
            (void)r;
        }
    }
    if (!routedAny) return Ok();   // e.g. headless with no center provider
    return Ok();
}

Result<void> NotificationManager::DispatchUpdate(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, entry] : providers_) {
        (void)name;
        if (!entry->enabled) continue;
        if (!entry->provider->Capabilities().supportsProgress) continue;
        (void)entry->provider->Update(n);
    }
    return Ok();
}

Result<void> NotificationManager::DispatchDismiss(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, entry] : providers_) {
        (void)name;
        if (!entry->enabled) continue;
        (void)entry->provider->Dismiss(id);
    }
    return Ok();
}

Result<void> NotificationManager::ConfigureAll(
    const std::map<std::string, json::Value>& configs) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, cfg] : configs) {
        auto it = providers_.find(name);
        if (it != providers_.end()) (void)it->second->provider->Configure(cfg);
    }
    return Ok();
}

std::shared_ptr<NotificationManager::ProviderEntry> NotificationManager::FindEntryLocked(
    std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(std::string(name));
    return it == providers_.end() ? nullptr : it->second;
}

} // namespace bps::notification
