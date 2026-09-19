#pragma once

// NotificationFactory (docs/specs/14 §Notification Factory). The only place
// that translates engine events into NotificationSeeds. Modules publish
// *events*; the factory turns them into notifications with the right severity,
// category, grouping key and correlation id. The service never invents
// notifications of its own.

#include "modules/notification/Notification.hpp"

#include <string>
#include <string_view>

namespace bps::notification {

class NotificationFactory {
public:
    NotificationFactory() = default;

    // --- Content events (Phase 3) ---
    NotificationSeed ImportFinished(std::string_view name, int assetCount,
                                    std::string_view correlationId = {}) const;
    NotificationSeed AssetDeleted(std::string_view uuid) const;
    NotificationSeed ValidationFailed(std::string_view reason) const;

    // --- Resource / system ---
    NotificationSeed LowMemory(std::string_view resource, std::string_view level) const;
    NotificationSeed EngineCrash(std::string_view message) const;

    // --- Platform / display ---
    NotificationSeed DisplayConnected(std::string_view detail) const;
    NotificationSeed DisplayRemoved(std::string_view detail) const;

    // --- Project events (Phase 4) ---
    NotificationSeed ProjectSaved(std::string_view name, bool autosave) const;
    NotificationSeed ProjectCreated(std::string_view name) const;
    NotificationSeed RecoveryAvailable(std::string_view detail) const;
    NotificationSeed PackageExported(std::string_view path) const;
    NotificationSeed BackupCompleted(std::string_view destination) const;
    NotificationSeed PluginFailed(std::string_view pluginName, std::string_view reason) const;

    // Generic seed builder for ad-hoc module events (still flows through the
    // same pipeline: rules → queue → providers).
    NotificationSeed From(std::string_view sourceModule, std::string_view sourceEvent,
                          Severity severity, std::string_view category,
                          std::string_view title, std::string_view message) const;
};

} // namespace bps::notification
