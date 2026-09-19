#pragma once

// Canonical engine events (docs/specs/05 §7). Every event used by the core
// systems is declared here so topics stay consistent across the codebase.
//
// Events derive from IEvent (which carries no data members) and declare a
// compile-time kTopic. Explicit constructors are provided so braced init
// ({fields...}) works even though the events are not aggregates.

#include "interfaces/IEvent.hpp"

namespace bps::events {

// --- Kernel (01) ---
struct EngineBooted : IEvent {
    static constexpr const char* kTopic = "engine.kernel.booted";
    std::chrono::microseconds bootTime{0};
    EngineBooted() = default;
    explicit EngineBooted(std::chrono::microseconds t) : bootTime(t) {}
};

struct KernelStateChanged : IEvent {
    static constexpr const char* kTopic = "engine.kernel.state_changed";
    KernelState from = KernelState::Booting;
    KernelState to = KernelState::Booting;
    KernelStateChanged() = default;
    KernelStateChanged(KernelState f, KernelState t) : from(f), to(t) {}
};

struct ShutdownStarted : IEvent {
    static constexpr const char* kTopic = "engine.kernel.shutdown_started";
};

struct ShutdownComplete : IEvent {
    static constexpr const char* kTopic = "engine.kernel.shutdown_complete";
    std::chrono::microseconds uptime{0};
    ShutdownComplete() = default;
    explicit ShutdownComplete(std::chrono::microseconds u) : uptime(u) {}
};

struct KernelPanic : IEvent {
    static constexpr const char* kTopic = "engine.kernel.panic";
    Error error;
    KernelPanic() = default;
    explicit KernelPanic(const Error& e) : error(e) {}
};

// --- Cross-system diagnostics (00 §7) ---
struct SystemHealthChanged : IEvent {
    static constexpr const char* kTopic = "engine.system.health_changed";
    std::string system;
    HealthReport report;
    SystemHealthChanged() = default;
    SystemHealthChanged(std::string s, const HealthReport& r) : system(std::move(s)), report(r) {}
};

// --- Configuration (03) ---
struct ConfigHotReload : IEvent {
    static constexpr const char* kTopic = "engine.config.hot_reload";
    int scope = 0;
    ConfigHotReload() = default;
    explicit ConfigHotReload(int s) : scope(s) {}
};

// --- Resource (10) ---
struct ResourcePressureChanged : IEvent {
    static constexpr const char* kTopic = "engine.resource.pressure_changed";
    std::string resource;
    PressureLevel from = PressureLevel::None;
    PressureLevel to = PressureLevel::None;
    ResourcePressureChanged() = default;
    ResourcePressureChanged(std::string r, PressureLevel f, PressureLevel t)
        : resource(std::move(r)), from(f), to(t) {}
};

struct ResourcePressureHigh : IEvent {
    static constexpr const char* kTopic = "engine.resource.pressure_high";
    std::string resource;
    PressureLevel level = PressureLevel::High;
    ResourcePressureHigh() = default;
    ResourcePressureHigh(std::string r, PressureLevel l) : resource(std::move(r)), level(l) {}
};

struct ResourceModeChanged : IEvent {
    static constexpr const char* kTopic = "engine.resource.mode_changed";
    ResourceMode mode = ResourceMode::Balanced;
    ResourceModeChanged() = default;
    explicit ResourceModeChanged(ResourceMode m) : mode(m) {}
};

// --- ThreadPool (06) ---
struct ThreadPoolCpuPressure : IEvent {
    static constexpr const char* kTopic = "engine.threadpool.cpu_pressure";
    double utilizationPct = 0.0;
    ThreadPoolCpuPressure() = default;
    explicit ThreadPoolCpuPressure(double p) : utilizationPct(p) {}
};

// --- Asset registry (core/assets) ---
struct AssetStateChanged : IEvent {
    static constexpr const char* kTopic = "engine.asset.state_changed";
    std::string id;
    int state = 0;   // AssetState
    AssetStateChanged() = default;
    AssetStateChanged(std::string id_, int state_) : id(std::move(id_)), state(state_) {}
};

// --- Display (core/display) ---
struct DisplayChanged : IEvent {
    static constexpr const char* kTopic = "engine.display.changed";
    std::string display;
    int index = 0;
    bool enabled = false;
    DisplayChanged() = default;
    DisplayChanged(std::string display_, int index_, bool enabled_)
        : display(std::move(display_)), index(index_), enabled(enabled_) {}
};

// --- Module (08) ---
struct ModuleStateChanged : IEvent {
    static constexpr const char* kTopic = "engine.module.state_changed";
    std::string id;
    int from = 0;
    int to = 0;
    ModuleStateChanged() = default;
    ModuleStateChanged(std::string id_, int from_, int to_) : id(std::move(id_)), from(from_), to(to_) {}
};

// --- Plugin (09) ---
struct PluginStateChanged : IEvent {
    static constexpr const char* kTopic = "engine.plugin.state_changed";
    std::string id;
    int from = 0;
    int to = 0;
    PluginStateChanged() = default;
    PluginStateChanged(std::string id_, int from_, int to_) : id(std::move(id_)), from(from_), to(to_) {}
};

struct PluginUpdated : IEvent {
    static constexpr const char* kTopic = "engine.plugin.updated";
    std::string id;
    Version fromVersion;
    Version toVersion;
    PluginUpdated() = default;
    PluginUpdated(std::string id_, const Version& from, const Version& to)
        : id(std::move(id_)), fromVersion(from), toVersion(to) {}
};

struct PluginSandboxDenied : IEvent {
    static constexpr const char* kTopic = "engine.plugin.sandbox_denied";
    std::string id;
    std::string capability;
    std::string reason;
    PluginSandboxDenied() = default;
    PluginSandboxDenied(std::string id_, std::string capability_, std::string reason_)
        : id(std::move(id_)), capability(std::move(capability_)), reason(std::move(reason_)) {}
};

// --- Domain example: presentation (canonical fan-out example, 05 §7) ---
struct SlideChanged : IEvent {
    static constexpr const char* kTopic = "presentation.slide_changed";
    int index = 0;
    int total = 0;
    std::string title;
    SlideChanged() = default;
    SlideChanged(int i, int t, std::string title_) : index(i), total(t), title(std::move(title_)) {}
};

// --- Domain: songs (modules/songs) ---
struct SongSelected : IEvent {
    static constexpr const char* kTopic = "songs.selected";
    std::string songId;
    std::string title;
    SongSelected() = default;
    SongSelected(std::string id_, std::string title_) : songId(std::move(id_)), title(std::move(title_)) {}
};

struct SongVerseChanged : IEvent {
    static constexpr const char* kTopic = "songs.verse_changed";
    std::string songId;
    std::string title;
    int verse = 0;          // 0-based
    int totalVerses = 0;
    std::string text;
    SongVerseChanged() = default;
    SongVerseChanged(std::string id_, std::string title_, int v, int total, std::string text_)
        : songId(std::move(id_)), title(std::move(title_)), verse(v), totalVerses(total),
          text(std::move(text_)) {}
};

// --- Domain: media (modules/media) ---
struct MediaStateChanged : IEvent {
    static constexpr const char* kTopic = "media.state_changed";
    std::string itemId;
    std::string title;
    int state = 0;              // MediaPlaybackState (modules/media)
    double positionSec = 0.0;
    double durationSec = 0.0;
    MediaStateChanged() = default;
    MediaStateChanged(std::string id_, std::string title_, int s, double pos, double dur)
        : itemId(std::move(id_)), title(std::move(title_)), state(s), positionSec(pos),
          durationSec(dur) {}
};

// --- Domain: platform (PAL OS events, Phase 2 / SystemArchitecture §3.6) ---
// Published by the Kernel's platform watcher, which drains
// IPlatform::PollChanges() into the Event Bus on a scheduler tick.
struct MonitorConnected : IEvent {
    static constexpr const char* kTopic = "platform.monitor_connected";
    std::string monitorId;
    MonitorConnected() = default;
    explicit MonitorConnected(std::string id_) : monitorId(std::move(id_)) {}
};

struct MonitorDisconnected : IEvent {
    static constexpr const char* kTopic = "platform.monitor_disconnected";
    std::string monitorId;
    MonitorDisconnected() = default;
    explicit MonitorDisconnected(std::string id_) : monitorId(std::move(id_)) {}
};

struct PowerChanged : IEvent {
    static constexpr const char* kTopic = "platform.power_changed";
    std::string detail;   // e.g. "on battery (87%)"
    PowerChanged() = default;
    explicit PowerChanged(std::string d) : detail(std::move(d)) {}
};

struct BatteryLow : IEvent {
    static constexpr const char* kTopic = "platform.battery_low";
    std::string detail;   // e.g. "12%"
    BatteryLow() = default;
    explicit BatteryLow(std::string d) : detail(std::move(d)) {}
};

struct MonitorResolutionChanged : IEvent {
    static constexpr const char* kTopic = "platform.monitor_resolution_changed";
    std::string monitorId;
    int widthPx = 0;
    int heightPx = 0;
    std::string detail;   // e.g. "eDP-1 1920x1080"
    MonitorResolutionChanged() = default;
    explicit MonitorResolutionChanged(std::string d) : detail(std::move(d)) {}
};

struct SystemSleep : IEvent {
    static constexpr const char* kTopic = "platform.sleep";
    std::string detail;
    SystemSleep() = default;
    explicit SystemSleep(std::string d) : detail(std::move(d)) {}
};

struct SystemWake : IEvent {
    static constexpr const char* kTopic = "platform.wake";
    std::string detail;
    SystemWake() = default;
    explicit SystemWake(std::string d) : detail(std::move(d)) {}
};

struct NetworkChanged : IEvent {
    static constexpr const char* kTopic = "platform.network_changed";
    std::string detail;
    NetworkChanged() = default;
    explicit NetworkChanged(std::string d) : detail(std::move(d)) {}
};

struct DeviceConnected : IEvent {
    static constexpr const char* kTopic = "platform.device_connected";
    std::string device;
    DeviceConnected() = default;
    explicit DeviceConnected(std::string d) : device(std::move(d)) {}
};

struct DeviceRemoved : IEvent {
    static constexpr const char* kTopic = "platform.device_removed";
    std::string device;
    DeviceRemoved() = default;
    explicit DeviceRemoved(std::string d) : device(std::move(d)) {}
};

struct LocaleChanged : IEvent {
    static constexpr const char* kTopic = "platform.locale_changed";
    std::string detail;
    LocaleChanged() = default;
    explicit LocaleChanged(std::string d) : detail(std::move(d)) {}
};

struct ClipboardChanged : IEvent {
    static constexpr const char* kTopic = "platform.clipboard_changed";
    std::string detail;
    ClipboardChanged() = default;
    explicit ClipboardChanged(std::string d) : detail(std::move(d)) {}
};

// --- Content & Asset Management (Phase 3, docs/specs/13) ---
struct ContentAssetAdded : IEvent {
    static constexpr const char* kTopic = "content.asset_added";
    std::string uuid;
    std::string name;
    ContentAssetAdded() = default;
    ContentAssetAdded(std::string u, std::string n) : uuid(std::move(u)), name(std::move(n)) {}
};

struct ContentAssetRemoved : IEvent {
    static constexpr const char* kTopic = "content.asset_removed";
    std::string uuid;
    ContentAssetRemoved() = default;
    explicit ContentAssetRemoved(std::string u) : uuid(std::move(u)) {}
};

struct ContentAssetChanged : IEvent {
    static constexpr const char* kTopic = "content.asset_changed";
    std::string uuid;
    std::string name;
    ContentAssetChanged() = default;
    ContentAssetChanged(std::string u, std::string n) : uuid(std::move(u)), name(std::move(n)) {}
};

struct ContentAssetLoaded : IEvent {
    static constexpr const char* kTopic = "content.asset_loaded";
    std::string uuid;
    std::string name;
    size_t bytes = 0;
    ContentAssetLoaded() = default;
    ContentAssetLoaded(std::string u, std::string n, size_t b)
        : uuid(std::move(u)), name(std::move(n)), bytes(b) {}
};

struct ContentAssetUnloaded : IEvent {
    static constexpr const char* kTopic = "content.asset_unloaded";
    std::string uuid;
    std::string name;
    ContentAssetUnloaded() = default;
    ContentAssetUnloaded(std::string u, std::string n)
        : uuid(std::move(u)), name(std::move(n)) {}
};

struct ContentAssetImported : IEvent {
    static constexpr const char* kTopic = "content.asset_imported";
    std::string uuid;
    std::string name;
    std::string importer;
    size_t assetsCreated = 0;
    ContentAssetImported() = default;
    ContentAssetImported(std::string u, std::string n, std::string imp, size_t count)
        : uuid(std::move(u)), name(std::move(n)), importer(std::move(imp)),
          assetsCreated(count) {}
};

struct ContentAssetExported : IEvent {
    static constexpr const char* kTopic = "content.asset_exported";
    std::string uuid;
    std::string format;
    std::string destination;
    ContentAssetExported() = default;
    ContentAssetExported(std::string u, std::string f, std::string d)
        : uuid(std::move(u)), format(std::move(f)), destination(std::move(d)) {}
};

struct ContentAssetDeleted : IEvent {
    static constexpr const char* kTopic = "content.asset_deleted";
    std::string uuid;
    ContentAssetDeleted() = default;
    explicit ContentAssetDeleted(std::string u) : uuid(std::move(u)) {}
};

struct ContentAssetIndexed : IEvent {
    static constexpr const char* kTopic = "content.asset_indexed";
    std::string uuid;
    size_t tokenCount = 0;
    ContentAssetIndexed() = default;
    ContentAssetIndexed(std::string u, size_t t) : uuid(std::move(u)), tokenCount(t) {}
};

struct ContentCacheUpdated : IEvent {
    static constexpr const char* kTopic = "content.cache_updated";
    size_t entries = 0;
    size_t bytes = 0;
    ContentCacheUpdated() = default;
    ContentCacheUpdated(size_t e, size_t b) : entries(e), bytes(b) {}
};

struct ContentThumbnailGenerated : IEvent {
    static constexpr const char* kTopic = "content.thumbnail_generated";
    std::string uuid;
    std::string size;   // "small" / "large"
    ContentThumbnailGenerated() = default;
    ContentThumbnailGenerated(std::string u, std::string s)
        : uuid(std::move(u)), size(std::move(s)) {}
};

struct ContentValidationFailed : IEvent {
    static constexpr const char* kTopic = "content.validation_failed";
    std::string uuid;
    std::string reason;
    ContentValidationFailed() = default;
    ContentValidationFailed(std::string u, std::string r)
        : uuid(std::move(u)), reason(std::move(r)) {}
};

// --- Notification Service (Phase 4, docs/specs/14) ---
// Published when a user triggers a notification action (Retry, Open Logs...).
// The NotificationService carries the action identifier only; the owning
// module subscribes and performs the work (no business logic in the service).
struct NotificationActionInvoked : IEvent {
    static constexpr const char* kTopic = "notification.action_invoked";
    uint64_t notificationId = 0;
    std::string actionId;
    std::string correlationId;
    NotificationActionInvoked() = default;
    NotificationActionInvoked(uint64_t id, std::string a, std::string c)
        : notificationId(id), actionId(std::move(a)), correlationId(std::move(c)) {}
};

// --- Adaptive Runtime System (Phase 5, docs/specs/16) ---
struct AdaptiveQualityChanged : IEvent {
    static constexpr const char* kTopic = "adaptive.quality_changed";
    std::string level;      // "Ultra" | "High" | "Balanced" | "Performance" | ...
    std::string reason;     // e.g. "battery", "thermal", "memory", "user", "auto"
    AdaptiveQualityChanged() = default;
    AdaptiveQualityChanged(std::string l, std::string r)
        : level(std::move(l)), reason(std::move(r)) {}
};

struct AdaptiveModuleSuspended : IEvent {
    static constexpr const char* kTopic = "adaptive.module_suspended";
    std::string moduleId;
    std::string reason;
    AdaptiveModuleSuspended() = default;
    AdaptiveModuleSuspended(std::string m, std::string r)
        : moduleId(std::move(m)), reason(std::move(r)) {}
};

struct AdaptiveModuleResumed : IEvent {
    static constexpr const char* kTopic = "adaptive.module_resumed";
    std::string moduleId;
    AdaptiveModuleResumed() = default;
    explicit AdaptiveModuleResumed(std::string m) : moduleId(std::move(m)) {}
};

struct AdaptiveMemoryBudgetChanged : IEvent {
    static constexpr const char* kTopic = "adaptive.memory_budget_changed";
    std::string subsystem;
    uint64_t budgetBytes = 0;
    AdaptiveMemoryBudgetChanged() = default;
    AdaptiveMemoryBudgetChanged(std::string s, uint64_t b)
        : subsystem(std::move(s)), budgetBytes(b) {}
};

struct AdaptiveOptimizationApplied : IEvent {
    static constexpr const char* kTopic = "adaptive.optimization_applied";
    std::string detail;
    AdaptiveOptimizationApplied() = default;
    explicit AdaptiveOptimizationApplied(std::string d) : detail(std::move(d)) {}
};

struct AdaptiveFeatureEnabled : IEvent {
    static constexpr const char* kTopic = "adaptive.feature_enabled";
    std::string featureId;
    AdaptiveFeatureEnabled() = default;
    explicit AdaptiveFeatureEnabled(std::string f) : featureId(std::move(f)) {}
};

struct AdaptiveFeatureDisabled : IEvent {
    static constexpr const char* kTopic = "adaptive.feature_disabled";
    std::string featureId;
    std::string reason;
    AdaptiveFeatureDisabled() = default;
    AdaptiveFeatureDisabled(std::string f, std::string r)
        : featureId(std::move(f)), reason(std::move(r)) {}
};

struct AdaptiveRecommendation : IEvent {
    static constexpr const char* kTopic = "adaptive.recommendation";
    std::string message;
    std::string actionId;   // UI action identifier; modules handle it
    AdaptiveRecommendation() = default;
    AdaptiveRecommendation(std::string m, std::string a)
        : message(std::move(m)), actionId(std::move(a)) {}
};

// --- Project & Data System (Phase 4, docs/specs/15) ---
struct ProjectOpened : IEvent {
    static constexpr const char* kTopic = "project.opened";
    std::string projectId;
    std::string name;
    ProjectOpened() = default;
    ProjectOpened(std::string id, std::string n)
        : projectId(std::move(id)), name(std::move(n)) {}
};

struct ProjectSaved : IEvent {
    static constexpr const char* kTopic = "project.saved";
    std::string projectId;
    std::string name;
    bool autosave = false;
    ProjectSaved() = default;
    ProjectSaved(std::string id, std::string n, bool auto_)
        : projectId(std::move(id)), name(std::move(n)), autosave(auto_) {}
};

struct ProjectClosed : IEvent {
    static constexpr const char* kTopic = "project.closed";
    std::string projectId;
    std::string name;
    ProjectClosed() = default;
    ProjectClosed(std::string id, std::string n)
        : projectId(std::move(id)), name(std::move(n)) {}
};

struct ProjectCreated : IEvent {
    static constexpr const char* kTopic = "project.created";
    std::string projectId;
    std::string name;
    ProjectCreated() = default;
    ProjectCreated(std::string id, std::string n)
        : projectId(std::move(id)), name(std::move(n)) {}
};

struct ProjectDeleted : IEvent {
    static constexpr const char* kTopic = "project.deleted";
    std::string projectId;
    std::string name;
    ProjectDeleted() = default;
    ProjectDeleted(std::string id, std::string n)
        : projectId(std::move(id)), name(std::move(n)) {}
};

struct DocumentOpened : IEvent {
    static constexpr const char* kTopic = "project.document_opened";
    std::string documentId;
    std::string documentType;
    DocumentOpened() = default;
    DocumentOpened(std::string id, std::string t)
        : documentId(std::move(id)), documentType(std::move(t)) {}
};

struct DocumentSaved : IEvent {
    static constexpr const char* kTopic = "project.document_saved";
    std::string documentId;
    std::string documentType;
    DocumentSaved() = default;
    DocumentSaved(std::string id, std::string t)
        : documentId(std::move(id)), documentType(std::move(t)) {}
};

struct DocumentClosed : IEvent {
    static constexpr const char* kTopic = "project.document_closed";
    std::string documentId;
    std::string documentType;
    DocumentClosed() = default;
    DocumentClosed(std::string id, std::string t)
        : documentId(std::move(id)), documentType(std::move(t)) {}
};

struct DocumentDirtyChanged : IEvent {
    static constexpr const char* kTopic = "project.document_dirty";
    std::string documentId;
    bool dirty = false;
    DocumentDirtyChanged() = default;
    DocumentDirtyChanged(std::string id, bool d) : documentId(std::move(id)), dirty(d) {}
};

struct UndoPerformed : IEvent {
    static constexpr const char* kTopic = "project.undo_performed";
    std::string commandName;
    size_t depth = 0;
    UndoPerformed() = default;
    UndoPerformed(std::string n, size_t d) : commandName(std::move(n)), depth(d) {}
};

struct RedoPerformed : IEvent {
    static constexpr const char* kTopic = "project.redo_performed";
    std::string commandName;
    size_t depth = 0;
    RedoPerformed() = default;
    RedoPerformed(std::string n, size_t d) : commandName(std::move(n)), depth(d) {}
};

struct SnapshotCreated : IEvent {
    static constexpr const char* kTopic = "project.snapshot_created";
    std::string projectId;
    std::string label;
    bool automatic = false;
    SnapshotCreated() = default;
    SnapshotCreated(std::string id, std::string l, bool a)
        : projectId(std::move(id)), label(std::move(l)), automatic(a) {}
};

struct BackupCompleted : IEvent {
    static constexpr const char* kTopic = "project.backup_completed";
    std::string projectId;
    std::string destination;
    BackupCompleted() = default;
    BackupCompleted(std::string id, std::string d)
        : projectId(std::move(id)), destination(std::move(d)) {}
};

struct PackageExported : IEvent {
    static constexpr const char* kTopic = "project.package_exported";
    std::string projectId;
    std::string path;
    size_t assetCount = 0;
    PackageExported() = default;
    PackageExported(std::string id, std::string p, size_t n)
        : projectId(std::move(id)), path(std::move(p)), assetCount(n) {}
};

struct RecoveryAvailable : IEvent {
    static constexpr const char* kTopic = "project.recovery_available";
    std::string projectId;
    std::string detail;
    RecoveryAvailable() = default;
    RecoveryAvailable(std::string id, std::string d)
        : projectId(std::move(id)), detail(std::move(d)) {}
};

struct DependencyIssueFound : IEvent {
    static constexpr const char* kTopic = "project.dependency_issue";
    std::string projectId;
    std::string assetId;
    std::string reason;   // "missing" / "broken" / "duplicate" / "unused"
    DependencyIssueFound() = default;
    DependencyIssueFound(std::string id, std::string a, std::string r)
        : projectId(std::move(id)), assetId(std::move(a)), reason(std::move(r)) {}
};

// --- Rendering Engine (Phase 6, docs/specs/17) ---
struct RenderFrameRendered : IEvent {
    static constexpr const char* kTopic = "render.frame_rendered";
    std::string sceneId;
    uint64_t frame = 0;
    double frameMs = 0.0;
    uint32_t drawCalls = 0;
    RenderFrameRendered() = default;
    RenderFrameRendered(std::string s, uint64_t f, double ms, uint32_t dc)
        : sceneId(std::move(s)), frame(f), frameMs(ms), drawCalls(dc) {}
};

struct RenderTextureLoaded : IEvent {
    static constexpr const char* kTopic = "render.texture_loaded";
    std::string name;
    int width = 0;
    int height = 0;
    RenderTextureLoaded() = default;
    RenderTextureLoaded(std::string n, int w, int h)
        : name(std::move(n)), width(w), height(h) {}
};

struct RenderShaderCompiled : IEvent {
    static constexpr const char* kTopic = "render.shader_compiled";
    std::string name;
    bool ok = true;
    RenderShaderCompiled() = default;
    RenderShaderCompiled(std::string n, bool o) : name(std::move(n)), ok(o) {}
};

struct RenderGpuOutOfMemory : IEvent {
    static constexpr const char* kTopic = "render.gpu_out_of_memory";
    std::string detail;
    RenderGpuOutOfMemory() = default;
    explicit RenderGpuOutOfMemory(std::string d) : detail(std::move(d)) {}
};

struct RenderFrameDropped : IEvent {
    static constexpr const char* kTopic = "render.frame_dropped";
    std::string sceneId;
    double frameMs = 0.0;
    double budgetMs = 0.0;
    RenderFrameDropped() = default;
    RenderFrameDropped(std::string s, double ms, double b)
        : sceneId(std::move(s)), frameMs(ms), budgetMs(b) {}
};

struct RenderError : IEvent {
    static constexpr const char* kTopic = "render.error";
    std::string detail;
    RenderError() = default;
    explicit RenderError(std::string d) : detail(std::move(d)) {}
};

// --- Display & Output Engine (Phase 7, docs/specs/18) ---
struct DisplayDeviceConnected : IEvent {
    static constexpr const char* kTopic = "display.device_connected";
    std::string deviceId;
    std::string name;
    DisplayDeviceConnected() = default;
    DisplayDeviceConnected(std::string id_, std::string name_)
        : deviceId(std::move(id_)), name(std::move(name_)) {}
};

struct DisplayDeviceDisconnected : IEvent {
    static constexpr const char* kTopic = "display.device_disconnected";
    std::string deviceId;
    DisplayDeviceDisconnected() = default;
    explicit DisplayDeviceDisconnected(std::string id_) : deviceId(std::move(id_)) {}
};

struct DisplayReady : IEvent {
    static constexpr const char* kTopic = "display.ready";
    std::string deviceId;
    std::string detail;
    DisplayReady() = default;
    DisplayReady(std::string id_, std::string d) : deviceId(std::move(id_)), detail(std::move(d)) {}
};

struct DisplayFailed : IEvent {
    static constexpr const char* kTopic = "display.failed";
    std::string deviceId;
    std::string detail;
    DisplayFailed() = default;
    DisplayFailed(std::string id_, std::string d) : deviceId(std::move(id_)), detail(std::move(d)) {}
};

struct DisplayRestored : IEvent {
    static constexpr const char* kTopic = "display.restored";
    std::string outputId;
    std::string deviceId;
    DisplayRestored() = default;
    DisplayRestored(std::string o, std::string d) : outputId(std::move(o)), deviceId(std::move(d)) {}
};

struct DisplayProfileApplied : IEvent {
    static constexpr const char* kTopic = "display.profile_applied";
    std::string name;
    size_t outputs = 0;
    DisplayProfileApplied() = default;
    DisplayProfileApplied(std::string n, size_t o) : name(std::move(n)), outputs(o) {}
};

struct OutputStarted : IEvent {
    static constexpr const char* kTopic = "output.started";
    std::string outputId;
    std::string deviceId;
    int kind = 0;   // OutputKind
    OutputStarted() = default;
    OutputStarted(std::string o, std::string d, int k)
        : outputId(std::move(o)), deviceId(std::move(d)), kind(k) {}
};

struct OutputStopped : IEvent {
    static constexpr const char* kTopic = "output.stopped";
    std::string outputId;
    int kind = 0;   // OutputKind
    OutputStopped() = default;
    OutputStopped(std::string o, int k) : outputId(std::move(o)), kind(k) {}
};

struct OutputLost : IEvent {
    static constexpr const char* kTopic = "output.lost";
    std::string outputId;
    std::string deviceId;
    OutputLost() = default;
    OutputLost(std::string o, std::string d) : outputId(std::move(o)), deviceId(std::move(d)) {}
};

// --- Presentation Engine (Phase 8, docs/specs/19) ---
struct PresentationOpened : IEvent {
    static constexpr const char* kTopic = "presentation.opened";
    std::string id;
    std::string name;
    size_t slides = 0;
    PresentationOpened() = default;
    PresentationOpened(std::string id_, std::string n, size_t s)
        : id(std::move(id_)), name(std::move(n)), slides(s) {}
};

struct PresentationClosed : IEvent {
    static constexpr const char* kTopic = "presentation.closed";
    std::string id;
    PresentationClosed() = default;
    explicit PresentationClosed(std::string id_) : id(std::move(id_)) {}
};

struct PresentationCompiled : IEvent {
    static constexpr const char* kTopic = "presentation.compiled";
    std::string id;
    size_t slides = 0;
    size_t warnings = 0;
    PresentationCompiled() = default;
    PresentationCompiled(std::string id_, size_t s, size_t w)
        : id(std::move(id_)), slides(s), warnings(w) {}
};

struct PresentationValidated : IEvent {
    static constexpr const char* kTopic = "presentation.validated";
    std::string id;
    size_t warnings = 0;
    size_t errors = 0;
    PresentationValidated() = default;
    PresentationValidated(std::string id_, size_t w, size_t e)
        : id(std::move(id_)), warnings(w), errors(e) {}
};

struct PresentationStarted : IEvent {
    static constexpr const char* kTopic = "presentation.started";
    std::string id;
    int slideIndex = 0;
    PresentationStarted() = default;
    PresentationStarted(std::string id_, int s) : id(std::move(id_)), slideIndex(s) {}
};

struct PresentationPaused : IEvent {
    static constexpr const char* kTopic = "presentation.paused";
    std::string id;
    PresentationPaused() = default;
    explicit PresentationPaused(std::string id_) : id(std::move(id_)) {}
};

struct PresentationResumed : IEvent {
    static constexpr const char* kTopic = "presentation.resumed";
    std::string id;
    PresentationResumed() = default;
    explicit PresentationResumed(std::string id_) : id(std::move(id_)) {}
};

struct PresentationStopped : IEvent {
    static constexpr const char* kTopic = "presentation.stopped";
    std::string id;
    PresentationStopped() = default;
    explicit PresentationStopped(std::string id_) : id(std::move(id_)) {}
};

struct PresentationCompleted : IEvent {
    static constexpr const char* kTopic = "presentation.completed";
    std::string id;
    PresentationCompleted() = default;
    explicit PresentationCompleted(std::string id_) : id(std::move(id_)) {}
};

struct PresentationRecovered : IEvent {
    static constexpr const char* kTopic = "presentation.recovered";
    std::string id;
    int slideIndex = 0;
    PresentationRecovered() = default;
    PresentationRecovered(std::string id_, int s) : id(std::move(id_)), slideIndex(s) {}
};

struct PresentationTransitionStarted : IEvent {
    static constexpr const char* kTopic = "presentation.transition_started";
    std::string id;
    int fromIndex = 0;
    int toIndex = 0;
    std::string transition;   // TransitionKind name
    PresentationTransitionStarted() = default;
    PresentationTransitionStarted(std::string id_, int f, int t, std::string tr)
        : id(std::move(id_)), fromIndex(f), toIndex(t), transition(std::move(tr)) {}
};

struct PresentationTransitionCompleted : IEvent {
    static constexpr const char* kTopic = "presentation.transition_completed";
    std::string id;
    int toIndex = 0;
    PresentationTransitionCompleted() = default;
    PresentationTransitionCompleted(std::string id_, int t)
        : id(std::move(id_)), toIndex(t) {}
};

struct PresentationCueTriggered : IEvent {
    static constexpr const char* kTopic = "presentation.cue_triggered";
    std::string id;
    std::string cueId;
    int kind = 0;   // CueKind
    double atSec = 0.0;
    PresentationCueTriggered() = default;
    PresentationCueTriggered(std::string id_, std::string c, int k, double t)
        : id(std::move(id_)), cueId(std::move(c)), kind(k), atSec(t) {}
};

// --- Search & Indexing Engine (Phase 9, docs/specs/20) ---
struct SearchIndexUpdated : IEvent {
    static constexpr const char* kTopic = "search.index_updated";
    size_t documents = 0;
    size_t terms = 0;
    SearchIndexUpdated() = default;
    SearchIndexUpdated(size_t d, size_t t) : documents(d), terms(t) {}
};

struct SearchStarted : IEvent {
    static constexpr const char* kTopic = "search.started";
    std::string query;
    SearchStarted() = default;
    explicit SearchStarted(std::string q) : query(std::move(q)) {}
};

struct SearchCompleted : IEvent {
    static constexpr const char* kTopic = "search.completed";
    std::string query;
    size_t results = 0;
    double elapsedMs = 0.0;
    SearchCompleted() = default;
    SearchCompleted(std::string q, size_t r, double ms)
        : query(std::move(q)), results(r), elapsedMs(ms) {}
};

struct SearchIndexRebuilt : IEvent {
    static constexpr const char* kTopic = "search.index_rebuilt";
    size_t documents = 0;
    SearchIndexRebuilt() = default;
    explicit SearchIndexRebuilt(size_t d) : documents(d) {}
};

struct SearchSuggestionsUpdated : IEvent {
    static constexpr const char* kTopic = "search.suggestions_updated";
    size_t suggestions = 0;
    SearchSuggestionsUpdated() = default;
    explicit SearchSuggestionsUpdated(size_t s) : suggestions(s) {}
};

// --- Media Engine (Phase 10, docs/specs/21) ---
struct MediaImported : IEvent {
    static constexpr const char* kTopic = "media.imported";
    std::string mediaId;
    std::string name;
    std::string type;    // MediaType name
    MediaImported() = default;
    MediaImported(std::string id_, std::string n, std::string t)
        : mediaId(std::move(id_)), name(std::move(n)), type(std::move(t)) {}
};

struct MediaReady : IEvent {
    static constexpr const char* kTopic = "media.ready";
    std::string mediaId;
    std::string detail;
    MediaReady() = default;
    MediaReady(std::string id_, std::string d) : mediaId(std::move(id_)), detail(std::move(d)) {}
};

struct MediaRemoved : IEvent {
    static constexpr const char* kTopic = "media.removed";
    std::string mediaId;
    MediaRemoved() = default;
    explicit MediaRemoved(std::string id_) : mediaId(std::move(id_)) {}
};

struct MediaThumbnailGenerated : IEvent {
    static constexpr const char* kTopic = "media.thumbnail_generated";
    std::string mediaId;
    int width = 0;
    int height = 0;
    MediaThumbnailGenerated() = default;
    MediaThumbnailGenerated(std::string id_, int w, int h)
        : mediaId(std::move(id_)), width(w), height(h) {}
};

struct MediaPlaybackStarted : IEvent {
    static constexpr const char* kTopic = "media.playback_started";
    std::string mediaId;
    std::string detail;
    MediaPlaybackStarted() = default;
    MediaPlaybackStarted(std::string id_, std::string d)
        : mediaId(std::move(id_)), detail(std::move(d)) {}
};

struct MediaPlaybackStopped : IEvent {
    static constexpr const char* kTopic = "media.playback_stopped";
    std::string mediaId;
    MediaPlaybackStopped() = default;
    explicit MediaPlaybackStopped(std::string id_) : mediaId(std::move(id_)) {}
};

struct MediaDecodeFailed : IEvent {
    static constexpr const char* kTopic = "media.decode_failed";
    std::string mediaId;
    std::string detail;
    MediaDecodeFailed() = default;
    MediaDecodeFailed(std::string id_, std::string d)
        : mediaId(std::move(id_)), detail(std::move(d)) {}
};

// --- Scene Composition Engine (Phase 11, docs/specs/23) ---
struct SceneComposed : IEvent {
    static constexpr const char* kTopic = "scene.composed";
    std::string sceneId;
    std::string contentId;
    std::string contentType;
    size_t widgets = 0;
    SceneComposed() = default;
    SceneComposed(std::string s, std::string c, std::string t, size_t w)
        : sceneId(std::move(s)), contentId(std::move(c)), contentType(std::move(t)),
          widgets(w) {}
};

struct SceneLayoutApplied : IEvent {
    static constexpr const char* kTopic = "scene.layout_applied";
    std::string sceneId;
    std::string layoutId;
    std::string outputId;
    SceneLayoutApplied() = default;
    SceneLayoutApplied(std::string s, std::string l, std::string o)
        : sceneId(std::move(s)), layoutId(std::move(l)), outputId(std::move(o)) {}
};

struct SceneRuleApplied : IEvent {
    static constexpr const char* kTopic = "scene.rule_applied";
    std::string ruleId;
    std::string outputId;
    SceneRuleApplied() = default;
    SceneRuleApplied(std::string r, std::string o)
        : ruleId(std::move(r)), outputId(std::move(o)) {}
};

struct SceneThemeApplied : IEvent {
    static constexpr const char* kTopic = "scene.theme_applied";
    std::string themeId;
    SceneThemeApplied() = default;
    explicit SceneThemeApplied(std::string t) : themeId(std::move(t)) {}
};

// --- Bible Engine (Phase 12, docs/specs/24) ---
struct BibleImported : IEvent {
    static constexpr const char* kTopic = "bible.imported";
    std::string bibleId;
    std::string name;
    size_t verses = 0;
    BibleImported() = default;
    BibleImported(std::string id_, std::string n, size_t v)
        : bibleId(std::move(id_)), name(std::move(n)), verses(v) {}
};

struct BibleLoaded : IEvent {
    static constexpr const char* kTopic = "bible.loaded";
    std::string bibleId;
    std::string name;
    BibleLoaded() = default;
    BibleLoaded(std::string id_, std::string n)
        : bibleId(std::move(id_)), name(std::move(n)) {}
};

struct BibleUpdated : IEvent {
    static constexpr const char* kTopic = "bible.updated";
    std::string bibleId;
    std::string name;
    BibleUpdated() = default;
    BibleUpdated(std::string id_, std::string n)
        : bibleId(std::move(id_)), name(std::move(n)) {}
};

struct BibleRemoved : IEvent {
    static constexpr const char* kTopic = "bible.removed";
    std::string bibleId;
    BibleRemoved() = default;
    explicit BibleRemoved(std::string id_) : bibleId(std::move(id_)) {}
};

struct BibleIndexed : IEvent {
    static constexpr const char* kTopic = "bible.indexed";
    std::string bibleId;
    size_t documents = 0;
    BibleIndexed() = default;
    BibleIndexed(std::string id_, size_t d)
        : bibleId(std::move(id_)), documents(d) {}
};

struct BiblePassageResolved : IEvent {
    static constexpr const char* kTopic = "bible.passage_resolved";
    std::string reference;
    std::string bookId;
    int chapter = 0;
    int verseStart = 0;
    int verseEnd = 0;
    BiblePassageResolved() = default;
    BiblePassageResolved(std::string ref, std::string book, int ch, int vs, int ve)
        : reference(std::move(ref)), bookId(std::move(book)), chapter(ch),
          verseStart(vs), verseEnd(ve) {}
};

struct BibleSearchCompleted : IEvent {
    static constexpr const char* kTopic = "bible.search_completed";
    std::string query;
    size_t results = 0;
    BibleSearchCompleted() = default;
    BibleSearchCompleted(std::string q, size_t r)
        : query(std::move(q)), results(r) {}
};

struct BibleValidationCompleted : IEvent {
    static constexpr const char* kTopic = "bible.validation_completed";
    std::string bibleId;
    size_t warnings = 0;
    BibleValidationCompleted() = default;
    BibleValidationCompleted(std::string id_, size_t w)
        : bibleId(std::move(id_)), warnings(w) {}
};

struct BibleValidationFailed : IEvent {
    static constexpr const char* kTopic = "bible.validation_failed";
    std::string bibleId;
    std::string reason;
    BibleValidationFailed() = default;
    BibleValidationFailed(std::string id_, std::string r)
        : bibleId(std::move(id_)), reason(std::move(r)) {}
};

// --- Song & Lyrics Engine (Phase 13, docs/specs/25) ---
struct SongImported : IEvent {
    static constexpr const char* kTopic = "song.imported";
    std::string songId;
    std::string title;
    SongImported() = default;
    SongImported(std::string id_, std::string t)
        : songId(std::move(id_)), title(std::move(t)) {}
};

struct SongLoaded : IEvent {
    static constexpr const char* kTopic = "song.loaded";
    std::string songId;
    std::string title;
    SongLoaded() = default;
    SongLoaded(std::string id_, std::string t)
        : songId(std::move(id_)), title(std::move(t)) {}
};

struct SongIndexed : IEvent {
    static constexpr const char* kTopic = "song.indexed";
    std::string songId;
    size_t documents = 0;
    SongIndexed() = default;
    SongIndexed(std::string id_, size_t d)
        : songId(std::move(id_)), documents(d) {}
};

struct SongUpdated : IEvent {
    static constexpr const char* kTopic = "song.updated";
    std::string songId;
    std::string title;
    SongUpdated() = default;
    SongUpdated(std::string id_, std::string t)
        : songId(std::move(id_)), title(std::move(t)) {}
};

struct SongDeleted : IEvent {
    static constexpr const char* kTopic = "song.deleted";
    std::string songId;
    SongDeleted() = default;
    explicit SongDeleted(std::string id_) : songId(std::move(id_)) {}
};

struct SongArrangementChanged : IEvent {
    static constexpr const char* kTopic = "song.arrangement_changed";
    std::string songId;
    std::string arrangementId;
    SongArrangementChanged() = default;
    SongArrangementChanged(std::string id_, std::string a)
        : songId(std::move(id_)), arrangementId(std::move(a)) {}
};

struct SongKeyChanged : IEvent {
    static constexpr const char* kTopic = "song.key_changed";
    std::string songId;
    std::string key;
    SongKeyChanged() = default;
    SongKeyChanged(std::string id_, std::string k)
        : songId(std::move(id_)), key(std::move(k)) {}
};

struct SongValidated : IEvent {
    static constexpr const char* kTopic = "song.validated";
    std::string songId;
    size_t warnings = 0;
    SongValidated() = default;
    SongValidated(std::string id_, size_t w)
        : songId(std::move(id_)), warnings(w) {}
};

struct SongValidationFailed : IEvent {
    static constexpr const char* kTopic = "song.validation_failed";
    std::string songId;
    std::string reason;
    SongValidationFailed() = default;
    SongValidationFailed(std::string id_, std::string r)
        : songId(std::move(id_)), reason(std::move(r)) {}
};

// --- Service Flow, Playlist & Automation Engine (Phase 14, docs/specs/26) ---
struct FlowStarted : IEvent {
    static constexpr const char* kTopic = "flow.started";
    std::string flowId;
    std::string executionId;
    std::string flowName;
    FlowStarted() = default;
    FlowStarted(std::string f, std::string e, std::string n)
        : flowId(std::move(f)), executionId(std::move(e)), flowName(std::move(n)) {}
};

struct FlowPaused : IEvent {
    static constexpr const char* kTopic = "flow.paused";
    std::string executionId;
    FlowPaused() = default;
    explicit FlowPaused(std::string e) : executionId(std::move(e)) {}
};

struct FlowResumed : IEvent {
    static constexpr const char* kTopic = "flow.resumed";
    std::string executionId;
    FlowResumed() = default;
    explicit FlowResumed(std::string e) : executionId(std::move(e)) {}
};

struct FlowCompleted : IEvent {
    static constexpr const char* kTopic = "flow.completed";
    std::string executionId;
    size_t nodesExecuted = 0;
    FlowCompleted() = default;
    FlowCompleted(std::string e, size_t n)
        : executionId(std::move(e)), nodesExecuted(n) {}
};

struct FlowStopped : IEvent {
    static constexpr const char* kTopic = "flow.stopped";
    std::string executionId;
    FlowStopped() = default;
    explicit FlowStopped(std::string e) : executionId(std::move(e)) {}
};

struct FlowInterrupted : IEvent {
    static constexpr const char* kTopic = "flow.interrupted";
    std::string executionId;
    std::string reason;
    FlowInterrupted() = default;
    FlowInterrupted(std::string e, std::string r)
        : executionId(std::move(e)), reason(std::move(r)) {}
};

struct FlowRecovered : IEvent {
    static constexpr const char* kTopic = "flow.recovered";
    std::string executionId;
    std::string nodeId;
    FlowRecovered() = default;
    FlowRecovered(std::string e, std::string n)
        : executionId(std::move(e)), nodeId(std::move(n)) {}
};

struct FlowValidated : IEvent {
    static constexpr const char* kTopic = "flow.validated";
    std::string flowId;
    size_t warnings = 0;
    FlowValidated() = default;
    FlowValidated(std::string f, size_t w) : flowId(std::move(f)), warnings(w) {}
};

struct NodeStarted : IEvent {
    static constexpr const char* kTopic = "flow.node_started";
    std::string executionId;
    std::string nodeId;
    std::string label;
    NodeStarted() = default;
    NodeStarted(std::string e, std::string n, std::string l)
        : executionId(std::move(e)), nodeId(std::move(n)), label(std::move(l)) {}
};

struct NodeCompleted : IEvent {
    static constexpr const char* kTopic = "flow.node_completed";
    std::string executionId;
    std::string nodeId;
    NodeCompleted() = default;
    NodeCompleted(std::string e, std::string n)
        : executionId(std::move(e)), nodeId(std::move(n)) {}
};

struct NodeFailed : IEvent {
    static constexpr const char* kTopic = "flow.node_failed";
    std::string executionId;
    std::string nodeId;
    std::string error;
    NodeFailed() = default;
    NodeFailed(std::string e, std::string n, std::string err)
        : executionId(std::move(e)), nodeId(std::move(n)), error(std::move(err)) {}
};

struct NodeSkipped : IEvent {
    static constexpr const char* kTopic = "flow.node_skipped";
    std::string executionId;
    std::string nodeId;
    NodeSkipped() = default;
    NodeSkipped(std::string e, std::string n)
        : executionId(std::move(e)), nodeId(std::move(n)) {}
};

struct AutomationCancelled : IEvent {
    static constexpr const char* kTopic = "flow.automation_cancelled";
    std::string executionId;
    std::string reason;
    AutomationCancelled() = default;
    AutomationCancelled(std::string e, std::string r)
        : executionId(std::move(e)), reason(std::move(r)) {}
};

struct AutomationEmergencyStopped : IEvent {
    static constexpr const char* kTopic = "flow.emergency_stopped";
    std::string executionId;
    AutomationEmergencyStopped() = default;
    explicit AutomationEmergencyStopped(std::string e) : executionId(std::move(e)) {}
};

struct VariableChanged : IEvent {
    static constexpr const char* kTopic = "flow.variable_changed";
    std::string executionId;
    std::string name;
    std::string value;
    VariableChanged() = default;
    VariableChanged(std::string e, std::string n, std::string v)
        : executionId(std::move(e)), name(std::move(n)), value(std::move(v)) {}
};

// ===========================================================================
// Phase 15 — Extended Production Engine (docs/specs/27)
// ===========================================================================
struct ProductionStarted : IEvent {
    static constexpr const char* kTopic = "production.started";
    ProductionStarted() = default;
};

struct ProductionStopped : IEvent {
    static constexpr const char* kTopic = "production.stopped";
    ProductionStopped() = default;
};

struct BusChanged : IEvent {
    static constexpr const char* kTopic = "production.bus_changed";
    std::string busId;
    std::string change;   // "volume" | "mute" | "processing" | "routing" | "enabled"
    BusChanged() = default;
    BusChanged(std::string b, std::string c)
        : busId(std::move(b)), change(std::move(c)) {}
};

struct BusSceneApplied : IEvent {
    static constexpr const char* kTopic = "production.bus_scene_applied";
    std::string busId;
    std::string sceneName;
    BusSceneApplied() = default;
    BusSceneApplied(std::string b, std::string s)
        : busId(std::move(b)), sceneName(std::move(s)) {}
};

struct OutputChanged : IEvent {
    static constexpr const char* kTopic = "production.output_changed";
    std::string outputId;
    std::string videoBus;
    std::string audioBus;
    OutputChanged() = default;
    OutputChanged(std::string o, std::string v, std::string a)
        : outputId(std::move(o)), videoBus(std::move(v)), audioBus(std::move(a)) {}
};

struct OutputFailed : IEvent {
    static constexpr const char* kTopic = "production.output_failed";
    std::string outputId;
    std::string reason;
    OutputFailed() = default;
    OutputFailed(std::string o, std::string r)
        : outputId(std::move(o)), reason(std::move(r)) {}
};

struct OutputFailover : IEvent {
    static constexpr const char* kTopic = "production.output_failover";
    std::string outputId;
    std::string fromId;
    std::string toId;
    OutputFailover() = default;
    OutputFailover(std::string o, std::string f, std::string t)
        : outputId(std::move(o)), fromId(std::move(f)), toId(std::move(t)) {}
};

struct SourceFailed : IEvent {
    static constexpr const char* kTopic = "production.source_failed";
    std::string sourceId;
    std::string reason;
    SourceFailed() = default;
    SourceFailed(std::string s, std::string r)
        : sourceId(std::move(s)), reason(std::move(r)) {}
};

struct SourceFallback : IEvent {
    static constexpr const char* kTopic = "production.source_fallback";
    std::string sourceId;
    std::string fallbackId;
    SourceFallback() = default;
    SourceFallback(std::string s, std::string f)
        : sourceId(std::move(s)), fallbackId(std::move(f)) {}
};

struct SceneStateChanged : IEvent {
    static constexpr const char* kTopic = "production.scene_state_changed";
    std::string sceneId;
    std::string state;   // "preview" | "program" | "standby" | "disabled" | "emergency"
    SceneStateChanged() = default;
    SceneStateChanged(std::string s, std::string st)
        : sceneId(std::move(s)), state(std::move(st)) {}
};

struct ProductionValidated : IEvent {
    static constexpr const char* kTopic = "production.validated";
    size_t errors = 0;
    size_t warnings = 0;
    ProductionValidated() = default;
    ProductionValidated(size_t e, size_t w) : errors(e), warnings(w) {}
};

struct ProductionSnapshotSaved : IEvent {
    static constexpr const char* kTopic = "production.snapshot_saved";
    std::string name;
    size_t items = 0;
    ProductionSnapshotSaved() = default;
    ProductionSnapshotSaved(std::string n, size_t i)
        : name(std::move(n)), items(i) {}
};

struct ProductionRestored : IEvent {
    static constexpr const char* kTopic = "production.restored";
    std::string name;
    ProductionRestored() = default;
    explicit ProductionRestored(std::string n) : name(std::move(n)) {}
};

struct ProductionEmergency : IEvent {
    static constexpr const char* kTopic = "production.emergency";
    std::string reason;
    ProductionEmergency() = default;
    explicit ProductionEmergency(std::string r) : reason(std::move(r)) {}
};

struct VirtualSourceCreated : IEvent {
    static constexpr const char* kTopic = "production.virtual_source_created";
    std::string busId;
    std::string sourceId;
    VirtualSourceCreated() = default;
    VirtualSourceCreated(std::string b, std::string s)
        : busId(std::move(b)), sourceId(std::move(s)) {}
};

struct ControlSignalReceived : IEvent {
    static constexpr const char* kTopic = "production.control_signal";
    std::string kind;     // "midi" | "osc" | "keyboard" | "remote" | "api" | "plugin"
    std::string payload;
    ControlSignalReceived() = default;
    ControlSignalReceived(std::string k, std::string p)
        : kind(std::move(k)), payload(std::move(p)) {}
};

struct ClockSyncChanged : IEvent {
    static constexpr const char* kTopic = "production.clock_sync";
    bool synced = true;
    ClockSyncChanged() = default;
    explicit ClockSyncChanged(bool s) : synced(s) {}
};

struct ProductionPlanned : IEvent {
    static constexpr const char* kTopic = "production.planned";
    bool feasible = true;
    std::string bottleneck;
    ProductionPlanned() = default;
    ProductionPlanned(bool f, std::string b)
        : feasible(f), bottleneck(std::move(b)) {}
};

// ===========================================================================
// Phase 16 — Recording, Replay & Media Capture Engine (docs/specs/28)
// ===========================================================================
struct RecordingStarted : IEvent {
    static constexpr const char* kTopic = "recording.started";
    std::string recordingId;
    std::string profileId;
    std::string nodeId;
    RecordingStarted() = default;
    RecordingStarted(std::string r, std::string p, std::string n)
        : recordingId(std::move(r)), profileId(std::move(p)), nodeId(std::move(n)) {}
};

struct RecordingStopped : IEvent {
    static constexpr const char* kTopic = "recording.stopped";
    std::string recordingId;
    std::string reason;
    RecordingStopped() = default;
    RecordingStopped(std::string r, std::string reason_)
        : recordingId(std::move(r)), reason(std::move(reason_)) {}
};

struct RecordingPaused : IEvent {
    static constexpr const char* kTopic = "recording.paused";
    std::string recordingId;
    RecordingPaused() = default;
    explicit RecordingPaused(std::string r) : recordingId(std::move(r)) {}
};

struct RecordingResumed : IEvent {
    static constexpr const char* kTopic = "recording.resumed";
    std::string recordingId;
    RecordingResumed() = default;
    explicit RecordingResumed(std::string r) : recordingId(std::move(r)) {}
};

struct RecordingFailed : IEvent {
    static constexpr const char* kTopic = "recording.failed";
    std::string recordingId;
    std::string error;
    RecordingFailed() = default;
    RecordingFailed(std::string r, std::string e)
        : recordingId(std::move(r)), error(std::move(e)) {}
};

struct RecordingRecovered : IEvent {
    static constexpr const char* kTopic = "recording.recovered";
    std::string recordingId;
    size_t segments = 0;
    RecordingRecovered() = default;
    RecordingRecovered(std::string r, size_t s)
        : recordingId(std::move(r)), segments(s) {}
};

struct SegmentCreated : IEvent {
    static constexpr const char* kTopic = "recording.segment_created";
    std::string recordingId;
    size_t index = 0;
    std::string path;
    SegmentCreated() = default;
    SegmentCreated(std::string r, size_t i, std::string p)
        : recordingId(std::move(r)), index(i), path(std::move(p)) {}
};

struct DiskSpaceWarning : IEvent {
    static constexpr const char* kTopic = "recording.disk_space_warning";
    int freePercent = 100;
    std::string level;   // "warning" | "critical" | "emergency"
    DiskSpaceWarning() = default;
    DiskSpaceWarning(int f, std::string l)
        : freePercent(f), level(std::move(l)) {}
};

struct EncoderOverload : IEvent {
    static constexpr const char* kTopic = "recording.encoder_overload";
    std::string recordingId;
    std::string encoder;
    EncoderOverload() = default;
    EncoderOverload(std::string r, std::string e)
        : recordingId(std::move(r)), encoder(std::move(e)) {}
};

struct DroppedFramesDetected : IEvent {
    static constexpr const char* kTopic = "recording.dropped_frames";
    std::string recordingId;
    size_t count = 0;
    DroppedFramesDetected() = default;
    DroppedFramesDetected(std::string r, size_t c)
        : recordingId(std::move(r)), count(c) {}
};

struct ReplayBufferReady : IEvent {
    static constexpr const char* kTopic = "recording.replay_buffer_ready";
    std::string replayId;
    std::string nodeId;
    int64_t capacityMs = 0;
    ReplayBufferReady() = default;
    ReplayBufferReady(std::string r, std::string n, int64_t c)
        : replayId(std::move(r)), nodeId(std::move(n)), capacityMs(c) {}
};

struct ReplayCreated : IEvent {
    static constexpr const char* kTopic = "recording.replay_created";
    std::string replayId;
    std::string nodeId;
    std::string mode;   // "normal" | "slow_motion" | "fast" | "reverse"
    ReplayCreated() = default;
    ReplayCreated(std::string r, std::string n, std::string m)
        : replayId(std::move(r)), nodeId(std::move(n)), mode(std::move(m)) {}
};

struct CaptureDeviceConnected : IEvent {
    static constexpr const char* kTopic = "recording.capture_connected";
    std::string deviceId;
    std::string kind;
    CaptureDeviceConnected() = default;
    CaptureDeviceConnected(std::string d, std::string k)
        : deviceId(std::move(d)), kind(std::move(k)) {}
};

struct CaptureDeviceDisconnected : IEvent {
    static constexpr const char* kTopic = "recording.capture_disconnected";
    std::string deviceId;
    CaptureDeviceDisconnected() = default;
    explicit CaptureDeviceDisconnected(std::string d) : deviceId(std::move(d)) {}
};

// ---------------------------------------------------------------------------
// Phase 17 — Broadcast Engine (docs/specs/29): NDI + SDI providers.
// ---------------------------------------------------------------------------
struct BroadcastProviderRegistered : IEvent {
    static constexpr const char* kTopic = "broadcast.provider_registered";
    std::string provider;
    BroadcastProviderRegistered() = default;
    explicit BroadcastProviderRegistered(std::string p) : provider(std::move(p)) {}
};

struct BroadcastProviderUnavailable : IEvent {
    static constexpr const char* kTopic = "broadcast.provider_unavailable";
    std::string provider;
    std::string reason;
    BroadcastProviderUnavailable() = default;
    BroadcastProviderUnavailable(std::string p, std::string r)
        : provider(std::move(p)), reason(std::move(r)) {}
};

struct NdiSourcesChanged : IEvent {
    static constexpr const char* kTopic = "broadcast.ndi_sources_changed";
    size_t count = 0;
    NdiSourcesChanged() = default;
    explicit NdiSourcesChanged(size_t c) : count(c) {}
};

struct BroadcastSenderStarted : IEvent {
    static constexpr const char* kTopic = "broadcast.sender_started";
    std::string senderId;
    std::string name;
    BroadcastSenderStarted() = default;
    BroadcastSenderStarted(std::string id, std::string n)
        : senderId(std::move(id)), name(std::move(n)) {}
};

struct BroadcastSenderStopped : IEvent {
    static constexpr const char* kTopic = "broadcast.sender_stopped";
    std::string senderId;
    BroadcastSenderStopped() = default;
    explicit BroadcastSenderStopped(std::string id) : senderId(std::move(id)) {}
};

struct BroadcastReceiverConnected : IEvent {
    static constexpr const char* kTopic = "broadcast.receiver_connected";
    std::string receiverId;
    std::string source;
    BroadcastReceiverConnected() = default;
    BroadcastReceiverConnected(std::string id, std::string s)
        : receiverId(std::move(id)), source(std::move(s)) {}
};

struct BroadcastReceiverDisconnected : IEvent {
    static constexpr const char* kTopic = "broadcast.receiver_disconnected";
    std::string receiverId;
    BroadcastReceiverDisconnected() = default;
    explicit BroadcastReceiverDisconnected(std::string id) : receiverId(std::move(id)) {}
};

struct BroadcastFrameSent : IEvent {
    static constexpr const char* kTopic = "broadcast.frame_sent";
    std::string senderId;
    uint32_t width = 0;
    uint32_t height = 0;
    BroadcastFrameSent() = default;
    BroadcastFrameSent(std::string id, uint32_t w, uint32_t h)
        : senderId(std::move(id)), width(w), height(h) {}
};

struct BroadcastFrameReceived : IEvent {
    static constexpr const char* kTopic = "broadcast.frame_received";
    std::string receiverId;
    uint32_t width = 0;
    uint32_t height = 0;
    BroadcastFrameReceived() = default;
    BroadcastFrameReceived(std::string id, uint32_t w, uint32_t h)
        : receiverId(std::move(id)), width(w), height(h) {}
};

struct SdiDevicesChanged : IEvent {
    static constexpr const char* kTopic = "broadcast.sdi_devices_changed";
    size_t count = 0;
    SdiDevicesChanged() = default;
    explicit SdiDevicesChanged(size_t c) : count(c) {}
};

struct BroadcastError : IEvent {
    static constexpr const char* kTopic = "broadcast.error";
    std::string provider;
    std::string message;
    BroadcastError() = default;
    BroadcastError(std::string p, std::string m)
        : provider(std::move(p)), message(std::move(m)) {}
};

} // namespace bps::events
