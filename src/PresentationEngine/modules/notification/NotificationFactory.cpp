#include "modules/notification/NotificationFactory.hpp"
#include <format>

namespace bps::notification {

NotificationSeed NotificationFactory::ImportFinished(std::string_view name, int assetCount,
                                                     std::string_view correlationId) const {
    NotificationSeed s;
    s.sourceModule = "ContentManager";
    s.sourceEvent = "content.asset_imported";
    s.severity = Severity::Success;
    s.category = Category::kImport;
    s.title = "Import finished";
    s.message = std::format("{} ({} asset", name, assetCount) +
                (assetCount == 1 ? "" : "s") + ")";
    s.groupKey = "import";
    s.correlationId = std::string(correlationId);
    return s;
}

NotificationSeed NotificationFactory::AssetDeleted(std::string_view uuid) const {
    NotificationSeed s;
    s.sourceModule = "ContentManager";
    s.sourceEvent = "content.asset_deleted";
    s.severity = Severity::Information;
    s.category = Category::kContent;
    s.title = "Asset deleted";
    s.message = std::string(uuid);
    return s;
}

NotificationSeed NotificationFactory::ValidationFailed(std::string_view reason) const {
    NotificationSeed s;
    s.sourceModule = "ContentManager";
    s.sourceEvent = "content.validation_failed";
    s.severity = Severity::Warning;
    s.category = Category::kContent;
    s.title = "Validation failed";
    s.message = std::string(reason);
    return s;
}

NotificationSeed NotificationFactory::LowMemory(std::string_view resource,
                                                std::string_view level) const {
    NotificationSeed s;
    s.sourceModule = "ResourceManager";
    s.sourceEvent = "resource.pressure_high";
    s.severity = Severity::Warning;
    s.category = Category::kPerformance;
    s.title = "Low memory";
    s.message = std::string(resource) + " pressure: " + std::string(level);
    return s;
}

NotificationSeed NotificationFactory::ResourcePressure(std::string_view resource,
                                                       std::string_view level) const {
    if (resource == "memory")
        return LowMemory(resource, level);
    NotificationSeed s;
    s.sourceModule = "ResourceManager";
    s.sourceEvent = "resource.pressure_high";
    s.severity = Severity::Warning;
    s.category = Category::kPerformance;
    if (resource == "cpu") {
        s.title = "High CPU usage";
        s.message = std::string("The CPU has stayed at ") + std::string(level) +
                    " load for a while. Heavy work (video, builds, other apps) may be slowing the presentation.";
    } else if (resource == "disk") {
        s.title = "Low disk space";
        s.message = std::string("Disk usage: ") + std::string(level);
    } else {
        s.title = "High " + std::string(resource) + " usage";
        s.message = std::string(resource) + " load: " + std::string(level);
    }
    return s;
}

NotificationSeed NotificationFactory::EngineCrash(std::string_view message) const {
    NotificationSeed s;
    s.sourceModule = "Kernel";
    s.sourceEvent = "kernel.panic";
    s.severity = Severity::Critical;
    s.category = Category::kSystem;
    s.title = "Engine crash";
    s.message = std::string(message);
    s.persistent = true;
    return s;
}

NotificationSeed NotificationFactory::DisplayConnected(std::string_view detail) const {
    NotificationSeed s;
    s.sourceModule = "Platform";
    s.sourceEvent = "monitor.connected";
    s.severity = Severity::Information;
    s.category = Category::kDisplay;
    s.title = "Display connected";
    s.message = std::string(detail);
    return s;
}

NotificationSeed NotificationFactory::DisplayRemoved(std::string_view detail) const {
    NotificationSeed s;
    s.sourceModule = "Platform";
    s.sourceEvent = "monitor.disconnected";
    s.severity = Severity::Warning;
    s.category = Category::kDisplay;
    s.title = "Display removed";
    s.message = std::string(detail);
    return s;
}

NotificationSeed NotificationFactory::ProjectSaved(std::string_view name, bool autosave) const {
    NotificationSeed s;
    s.sourceModule = "ProjectManager";
    s.sourceEvent = "project.saved";
    s.severity = Severity::Success;
    s.category = Category::kProject;
    s.title = autosave ? "Auto save complete" : "Project saved";
    s.message = std::string(name);
    return s;
}

NotificationSeed NotificationFactory::ProjectCreated(std::string_view name) const {
    NotificationSeed s;
    s.sourceModule = "ProjectManager";
    s.sourceEvent = "project.created";
    s.severity = Severity::Success;
    s.category = Category::kProject;
    s.title = "Project created";
    s.message = std::string(name);
    return s;
}

NotificationSeed NotificationFactory::RecoveryAvailable(std::string_view detail) const {
    NotificationSeed s;
    s.sourceModule = "RecoveryManager";
    s.sourceEvent = "project.recovery_available";
    s.severity = Severity::Warning;
    s.category = Category::kProject;
    s.title = "Recovery available";
    s.message = std::string(detail);
    s.persistent = true;
    return s;
}

NotificationSeed NotificationFactory::PackageExported(std::string_view path) const {
    NotificationSeed s;
    s.sourceModule = "PackageManager";
    s.sourceEvent = "project.package_exported";
    s.severity = Severity::Success;
    s.category = Category::kExport;
    s.title = "Package exported";
    s.message = std::string(path);
    return s;
}

NotificationSeed NotificationFactory::BackupCompleted(std::string_view destination) const {
    NotificationSeed s;
    s.sourceModule = "BackupManager";
    s.sourceEvent = "project.backup_completed";
    s.severity = Severity::Information;
    s.category = Category::kProject;
    s.title = "Backup completed";
    s.message = std::string(destination);
    return s;
}

NotificationSeed NotificationFactory::PluginFailed(std::string_view pluginName,
                                                   std::string_view reason) const {
    NotificationSeed s;
    s.sourceModule = "PluginManager";
    s.sourceEvent = "plugin.failed";
    s.severity = Severity::Warning;
    s.category = Category::kPlugin;
    s.title = "Plugin failed";
    s.message = std::string(pluginName) + ": " + std::string(reason);
    return s;
}

NotificationSeed NotificationFactory::From(std::string_view sourceModule,
                                           std::string_view sourceEvent, Severity severity,
                                           std::string_view category, std::string_view title,
                                           std::string_view message) const {
    NotificationSeed s;
    s.sourceModule = std::string(sourceModule);
    s.sourceEvent = std::string(sourceEvent);
    s.severity = severity;
    s.category = std::string(category);
    s.title = std::string(title);
    s.message = std::string(message);
    return s;
}

} // namespace bps::notification
