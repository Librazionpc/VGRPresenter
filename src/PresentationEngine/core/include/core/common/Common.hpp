#pragma once

// ============================================================================
// bps::Common — shared vocabulary of the Believers Presentation Software engine
// core. Every system includes this header. Per docs/specs/00-cross-system-rules.md
// all systems return Result<T>, expose HealthReport GetHealth(), and publish
// dot-namespaced events.
// ============================================================================

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bps {

// ---------------------------------------------------------------------------
// Engine version
// ---------------------------------------------------------------------------
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string pre; // optional pre-release tag

    static Version Parse(std::string_view s);
    std::string ToString() const;
    int Compare(const Version& other) const noexcept;
    bool operator==(const Version& o) const noexcept { return Compare(o) == 0; }
    bool operator!=(const Version& o) const noexcept { return Compare(o) != 0; }
    bool operator<(const Version& o) const noexcept { return Compare(o) < 0; }
    bool operator<=(const Version& o) const noexcept { return Compare(o) <= 0; }
    bool operator>(const Version& o) const noexcept { return Compare(o) > 0; }
    bool operator>=(const Version& o) const noexcept { return Compare(o) >= 0; }
};

inline const Version kEngineVersion{1, 0, 0, ""};

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------
enum class LogLevel : int { Trace = 0, Debug, Info, Warning, Error, Fatal };

inline const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:   return "TRACE";
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Engine-wide enums (shared so events can reference them without cycles)
// ---------------------------------------------------------------------------
enum class KernelState : int { Booting, Starting, Running, Paused, ShuttingDown, Stopped, CrashRecovery };

inline const char* ToString(KernelState s) {
    switch (s) {
        case KernelState::Booting:        return "Booting";
        case KernelState::Starting:       return "Starting";
        case KernelState::Running:        return "Running";
        case KernelState::Paused:         return "Paused";
        case KernelState::ShuttingDown:   return "ShuttingDown";
        case KernelState::Stopped:        return "Stopped";
        case KernelState::CrashRecovery:  return "CrashRecovery";
    }
    return "Unknown";
}

enum class ResourceMode : int { Balanced = 0, Performance, Strict, Battery, Developer };

inline const char* ToString(ResourceMode m) {
    switch (m) {
        case ResourceMode::Balanced:    return "Balanced";
        case ResourceMode::Performance: return "Performance";
        case ResourceMode::Strict:      return "Strict";
        case ResourceMode::Battery:     return "Battery";
        case ResourceMode::Developer:   return "Developer";
    }
    return "Unknown";
}

enum class PressureLevel : int { None = 0, Low, Medium, High, Critical };

inline const char* ToString(PressureLevel p) {
    switch (p) {
        case PressureLevel::None:     return "None";
        case PressureLevel::Low:      return "Low";
        case PressureLevel::Medium:   return "Medium";
        case PressureLevel::High:     return "High";
        case PressureLevel::Critical: return "Critical";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Error codes (stable ints; namespaced per system per docs/specs/00 §1)
// ---------------------------------------------------------------------------
using ErrorCode = int;
namespace Err {
inline constexpr ErrorCode Ok = 0;
// common
inline constexpr ErrorCode InvalidArgument   = 1;
inline constexpr ErrorCode NotFound          = 2;
inline constexpr ErrorCode AlreadyExists     = 3;
inline constexpr ErrorCode InvalidState      = 4;
inline constexpr ErrorCode Timeout           = 5;
inline constexpr ErrorCode OutOfMemory       = 6;
inline constexpr ErrorCode LimitExceeded     = 7;
inline constexpr ErrorCode IoError           = 8;
inline constexpr ErrorCode ParseError        = 9;
inline constexpr ErrorCode NotImplemented    = 10;
inline constexpr ErrorCode VersionMismatch   = 11;
inline constexpr ErrorCode Unsupported       = 12;
// Kernel (01)
inline constexpr ErrorCode Kernel_InvalidStateTransition = 100;
inline constexpr ErrorCode Kernel_BootFailed             = 101;
// Logger (02)
inline constexpr ErrorCode Logger_SinkWriteFailed        = 200;
// Config (03)
inline constexpr ErrorCode Config_ParseFailed            = 300;
inline constexpr ErrorCode Config_ValidationFailed       = 301;
inline constexpr ErrorCode Config_MigrationMissing       = 302;
// Services (04)
inline constexpr ErrorCode Services_NotRegistered        = 400;
inline constexpr ErrorCode Services_CyclicDependency     = 401;
// EventBus (05)
inline constexpr ErrorCode EventBus_RequestTimeout       = 500;
inline constexpr ErrorCode EventBus_NoResponder          = 501;
// ThreadPool (06)
inline constexpr ErrorCode ThreadPool_Cancelled          = 600;
inline constexpr ErrorCode ThreadPool_Backpressure       = 601;
// Scheduler (07)
inline constexpr ErrorCode Scheduler_TaskNotFound        = 700;
inline constexpr ErrorCode Scheduler_Submit             = 701;
// Memory (11)
inline constexpr ErrorCode Memory_LimitExceeded          = 800;
inline constexpr ErrorCode Memory_BadFree                = 801;
inline constexpr ErrorCode Memory_Leak                   = 802;
// Plugins (09)
inline constexpr ErrorCode Plugin_LoadFailed             = 900;
inline constexpr ErrorCode Plugin_SignatureFailed        = 901;
inline constexpr ErrorCode Plugin_CapabilityDenied       = 902;
// Modules (08)
inline constexpr ErrorCode Module_LoadFailed             = 1000;
inline constexpr ErrorCode Module_DependencyMissing      = 1001;
// Lifecycle (12)
inline constexpr ErrorCode Lifecycle_InvalidTransition   = 1100;
inline constexpr ErrorCode Lifecycle_HookFailed          = 1101;
inline constexpr ErrorCode Lifecycle_DependencyBlocked   = 1102;
// Resources (10)
inline constexpr ErrorCode Resource_ModeChangeFailed     = 1200;
// Content & Asset Management (Phase 3, docs/specs/13)
inline constexpr ErrorCode Content_NotFound              = 1300;
inline constexpr ErrorCode Content_ImportFailed          = 1301;
inline constexpr ErrorCode Content_ExportFailed          = 1302;
inline constexpr ErrorCode Content_ValidationFailed      = 1303;
inline constexpr ErrorCode Content_UnsupportedFormat     = 1304;
inline constexpr ErrorCode Content_DuplicateAsset        = 1305;
inline constexpr ErrorCode Content_Cancelled             = 1306;
inline constexpr ErrorCode Content_VfsPathNotMounted     = 1307;
inline constexpr ErrorCode Content_IndexingDisabled      = 1308;
// Notification Service (Phase 4, docs/specs/14)
inline constexpr ErrorCode Notification_ProviderFailed    = 1400;
inline constexpr ErrorCode Notification_UnknownChannel    = 1401;
inline constexpr ErrorCode Notification_QueueFull         = 1402;
inline constexpr ErrorCode Notification_HistoryFull       = 1403;
inline constexpr ErrorCode Notification_NoRule            = 1404;
// Project & Data System (Phase 4, docs/specs/15)
inline constexpr ErrorCode Project_NotFound               = 1500;
inline constexpr ErrorCode Project_AlreadyOpen            = 1501;
inline constexpr ErrorCode Project_NotOpen                = 1502;
inline constexpr ErrorCode Project_SaveFailed             = 1503;
inline constexpr ErrorCode Project_PackageFailed          = 1504;
inline constexpr ErrorCode Project_UndoEmpty              = 1505;
inline constexpr ErrorCode Project_RedoEmpty              = 1506;
inline constexpr ErrorCode Project_DocumentLocked         = 1507;
inline constexpr ErrorCode Project_DependencyMissing      = 1508;inline constexpr ErrorCode Project_RecoveryNotFound      = 1509;
// Adaptive Runtime System (Phase 5, docs/specs/16)
inline constexpr ErrorCode Adaptive_FeatureDisabled       = 1600;
inline constexpr ErrorCode Adaptive_NoContract            = 1601;
inline constexpr ErrorCode Adaptive_BudgetExceeded        = 1602;
inline constexpr ErrorCode Adaptive_InvalidMode           = 1603;
inline constexpr ErrorCode Adaptive_InvalidLayer          = 1604;
// Rendering Engine (Phase 6, docs/specs/17)
inline constexpr ErrorCode Render_BackendUnavailable       = 1700;
inline constexpr ErrorCode Render_BackendInitFailed        = 1701;
inline constexpr ErrorCode Render_SceneNotFound            = 1702;
inline constexpr ErrorCode Render_ObjectNotFound           = 1703;
inline constexpr ErrorCode Render_LayerNotFound            = 1704;
inline constexpr ErrorCode Render_TextureAllocFailed       = 1705;
inline constexpr ErrorCode Render_TextureNotFound          = 1706;
inline constexpr ErrorCode Render_GpuOutOfMemory           = 1707;
inline constexpr ErrorCode Render_FontNotFound             = 1708;
inline constexpr ErrorCode Render_ShaderCompileFailed      = 1709;
inline constexpr ErrorCode Render_InvalidPass              = 1710;
inline constexpr ErrorCode Render_OutputNotFound           = 1711;
// Display & Output Engine (Phase 7, docs/specs/18)
inline constexpr ErrorCode Display_DeviceNotFound          = 1800;
inline constexpr ErrorCode Display_ProviderNotFound        = 1801;
inline constexpr ErrorCode Display_ProviderExists          = 1808;
inline constexpr ErrorCode Display_OutputNotFound          = 1802;
inline constexpr ErrorCode Display_OutputExists            = 1803;
inline constexpr ErrorCode Display_ProfileNotFound         = 1804;
inline constexpr ErrorCode Display_ProfileExists           = 1805;
inline constexpr ErrorCode Display_InvalidScaling          = 1806;
inline constexpr ErrorCode Display_Unsupported             = 1807;
// Presentation Engine (Phase 8, docs/specs/19)
inline constexpr ErrorCode Presentation_NotFound            = 1900;
inline constexpr ErrorCode Presentation_AlreadyOpen         = 1901;
inline constexpr ErrorCode Presentation_NotOpen             = 1902;
inline constexpr ErrorCode Presentation_InvalidState        = 1903;
inline constexpr ErrorCode Presentation_NoSlides            = 1904;
inline constexpr ErrorCode Presentation_CompileFailed       = 1905;
inline constexpr ErrorCode Presentation_ValidationFailed    = 1906;
inline constexpr ErrorCode Presentation_SaveFailed          = 1907;
inline constexpr ErrorCode Presentation_InvalidTransition   = 1908;
inline constexpr ErrorCode Presentation_CueFailed           = 1909;
inline constexpr ErrorCode Presentation_RecoveryNotFound    = 1910;
// Search & Indexing Engine (Phase 9, docs/specs/20)
inline constexpr ErrorCode Search_IndexUnavailable          = 2000;
inline constexpr ErrorCode Search_DocumentNotFound          = 2001;
inline constexpr ErrorCode Search_AdapterNotFound           = 2002;
inline constexpr ErrorCode Search_NoAdapter                = 2003;
inline constexpr ErrorCode Search_InvalidQuery              = 2004;
inline constexpr ErrorCode Search_CorruptIndex              = 2005;
inline constexpr ErrorCode Search_CacheFull                 = 2006;
// Media Engine (Phase 10, docs/specs/21)
inline constexpr ErrorCode Media_UnsupportedFormat          = 2100;
inline constexpr ErrorCode Media_NotFound                   = 2101;
inline constexpr ErrorCode Media_AlreadyExists              = 2102;
inline constexpr ErrorCode Media_DecodeFailed               = 2103;
inline constexpr ErrorCode Media_CorruptFile                = 2104;
inline constexpr ErrorCode Media_NoDecoder                  = 2105;
inline constexpr ErrorCode Media_CacheFull                  = 2106;
// Native .vgr format (Phase 10, docs/specs/22)
inline constexpr ErrorCode Vgr_BadSignature                 = 2200;
inline constexpr ErrorCode Vgr_UnsupportedVersion           = 2201;
inline constexpr ErrorCode Vgr_CorruptSection               = 2202;
inline constexpr ErrorCode Vgr_HashMismatch                 = 2203;
inline constexpr ErrorCode Vgr_UnknownType                  = 2204;
inline constexpr ErrorCode Vgr_NotFound                     = 2205;
// Scene Composition Engine (Phase 11, docs/specs/23)
inline constexpr ErrorCode Scene_WidgetNotFound             = 2300;
inline constexpr ErrorCode Scene_RegionNotFound             = 2301;
inline constexpr ErrorCode Scene_LayoutNotFound             = 2302;
inline constexpr ErrorCode Scene_ThemeNotFound              = 2303;
inline constexpr ErrorCode Scene_TemplateNotFound           = 2304;
inline constexpr ErrorCode Scene_RuleNotFound               = 2305;
// Bible Engine (Phase 12, docs/specs/24)
inline constexpr ErrorCode Bible_UnsupportedFormat          = 2400;
inline constexpr ErrorCode Bible_ValidationFailed           = 2401;
inline constexpr ErrorCode Bible_NotFound                   = 2402;
inline constexpr ErrorCode Bible_AlreadyExists              = 2403;
inline constexpr ErrorCode Bible_InvalidReference           = 2404;
inline constexpr ErrorCode Bible_ImportFailed               = 2405;
inline constexpr ErrorCode Bible_CorruptFile                = 2406;
inline constexpr ErrorCode Bible_ProviderNotFound           = 2407;
// Song & Lyrics Engine (Phase 13, docs/specs/25)
inline constexpr ErrorCode Song_UnsupportedFormat           = 2500;
inline constexpr ErrorCode Song_ValidationFailed            = 2501;
inline constexpr ErrorCode Song_NotFound                    = 2502;
inline constexpr ErrorCode Song_AlreadyExists               = 2503;
inline constexpr ErrorCode Song_ImportFailed                = 2504;
inline constexpr ErrorCode Song_SectionNotFound             = 2505;
inline constexpr ErrorCode Song_ArrangementNotFound         = 2506;
inline constexpr ErrorCode Song_VersionNotFound             = 2507;
inline constexpr ErrorCode Song_Duplicate                   = 2508;
inline constexpr ErrorCode Song_ProviderNotFound            = 2509;

// Phase 14 — Service Flow, Playlist & Automation Engine (docs/specs/26).
inline constexpr ErrorCode Flow_NotFound                   = 2600;
inline constexpr ErrorCode Flow_AlreadyExists              = 2601;
inline constexpr ErrorCode Flow_ValidationFailed           = 2602;
inline constexpr ErrorCode Flow_UnsupportedFormat          = 2603;
inline constexpr ErrorCode Flow_NotRunning                 = 2604;
inline constexpr ErrorCode Flow_AlreadyRunning             = 2605;
inline constexpr ErrorCode Flow_NodeFailed                 = 2606;
inline constexpr ErrorCode Flow_ActionFailed               = 2607;
inline constexpr ErrorCode Flow_ActionNotFound             = 2608;
inline constexpr ErrorCode Flow_ConditionNotFound          = 2609;
inline constexpr ErrorCode Flow_TriggerNotFound            = 2610;
inline constexpr ErrorCode Flow_RecoveryFailed             = 2611;
inline constexpr ErrorCode Flow_InvalidState               = 2612;
inline constexpr ErrorCode Flow_TemplateNotFound           = 2613;
inline constexpr ErrorCode Flow_MacroNotFound              = 2614;
inline constexpr ErrorCode Flow_VariableNotFound           = 2615;

// Phase 15 — Extended Production Engine (docs/specs/27).
inline constexpr ErrorCode Production_NodeNotFound          = 2700;
inline constexpr ErrorCode Production_AlreadyExists         = 2701;
inline constexpr ErrorCode Production_CircularRoute         = 2702;
inline constexpr ErrorCode Production_InvalidSignalType     = 2703;
inline constexpr ErrorCode Production_ValidationFailed      = 2704;
inline constexpr ErrorCode Production_OutputNotFound        = 2705;
inline constexpr ErrorCode Production_BusNotFound           = 2706;
inline constexpr ErrorCode Production_SourceNotFound        = 2707;
inline constexpr ErrorCode Production_SceneNotFound         = 2708;
inline constexpr ErrorCode Production_SnapshotNotFound      = 2709;
inline constexpr ErrorCode Production_NotRunning            = 2710;
inline constexpr ErrorCode Production_InvalidState          = 2711;
inline constexpr ErrorCode Production_EncoderNotFound       = 2712;
inline constexpr ErrorCode Production_ResourceOverload      = 2713;
inline constexpr ErrorCode Production_UnsupportedLayout     = 2714;
inline constexpr ErrorCode Production_SignalMismatch        = 2715;

// Phase 16 — Recording, Replay & Media Capture Engine (docs/specs/28).
inline constexpr ErrorCode Recording_NotFound               = 2800;
inline constexpr ErrorCode Recording_AlreadyExists          = 2801;
inline constexpr ErrorCode Recording_AlreadyActive          = 2802;
inline constexpr ErrorCode Recording_NotActive              = 2803;
inline constexpr ErrorCode Recording_NodeNotFound           = 2804;
inline constexpr ErrorCode Recording_ProfileNotFound        = 2805;
inline constexpr ErrorCode Recording_InvalidState           = 2806;
inline constexpr ErrorCode Recording_EncoderUnavailable     = 2807;
inline constexpr ErrorCode Recording_DiskFull               = 2808;
inline constexpr ErrorCode Recording_ValidationFailed       = 2809;
inline constexpr ErrorCode Recording_ReplayNotFound         = 2810;
inline constexpr ErrorCode Recording_CaptureNotFound        = 2811;
inline constexpr ErrorCode Recording_ContainerNotFound      = 2812;
inline constexpr ErrorCode Recording_TemplateNotFound       = 2813;

// --- Broadcast (Phase 17, docs/specs/29) ---
inline constexpr ErrorCode Broadcast_Unsupported            = 2900;
inline constexpr ErrorCode Broadcast_ProviderNotFound       = 2901;
inline constexpr ErrorCode Broadcast_ProviderExists         = 2902;
inline constexpr ErrorCode Broadcast_SdkLoadFailed          = 2903;
inline constexpr ErrorCode Broadcast_InvalidState           = 2904;
inline constexpr ErrorCode Broadcast_NotFound               = 2905;
inline constexpr ErrorCode Broadcast_AlreadyActive          = 2906;
inline constexpr ErrorCode Broadcast_NotActive              = 2907;
inline constexpr ErrorCode Broadcast_SendFailed             = 2908;
inline constexpr ErrorCode Broadcast_ReceiveFailed          = 2909;
inline constexpr ErrorCode Broadcast_InvalidFrame           = 2910;
inline constexpr ErrorCode Broadcast_NoSdiDevices           = 2911;
inline constexpr ErrorCode Broadcast_SourceNotFound         = 2912;
// The vendor runtime (NDI/DeckLink) is simply not on this machine — distinct from
// SdkLoadFailed (present but unusable) so the UI can point at the download page.
inline constexpr ErrorCode Broadcast_SdkNotInstalled         = 2913;
}

struct Error {
    ErrorCode code = Err::Ok;
    std::string message;
    std::string module;
    // Raw OS error code (errno / GetLastError) when a PAL backend surfaced one
    // (PAL DoD §19); 0 when none applies.
    int nativeError = 0;
    // Call-site backtrace captured by PAL backends in debug builds
    // (platform::CaptureStack()); empty in release builds and when the OS has
    // no stack API. PAL DoD §19 "Stack (Debug)".
    std::string stack;
    // Chained root cause. shared_ptr (not optional<Error>) so the type stays
    // complete-free at the member declaration (libstdc++ instantiates optional
    // eagerly, which would fail on the recursive type).
    std::shared_ptr<Error> cause;

    static Error Make(ErrorCode code, std::string_view module, std::string_view message,
                      const Error* causePtr = nullptr) {
        Error e;
        e.code = code;
        e.module = module;
        e.message = message;
        if (causePtr) e.cause = std::make_shared<Error>(*causePtr);
        return e;
    }
};

// ---------------------------------------------------------------------------
// Result<T> — the only failure transport across system boundaries (00 §1)
// ---------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : ok_(true), value_(std::move(value)) {}
    Result(Error error) : ok_(false), error_(std::move(error)) {}
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept = default;

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
    T& operator*() { return value_; }
    const T& operator*() const { return value_; }
    const Error& error() const { return error_; }

private:
    bool ok_ = false;
    T value_{};
    Error error_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default; // ok by default
    Result(Error error) : ok_(false), error_(std::move(error)) {}
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept = default;

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const { return error_; }

private:
    bool ok_ = true;
    Error error_;
};

inline Result<void> Ok() { return Result<void>{}; }

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------
enum class HealthState : int { Healthy = 0, Degraded, Failing };

inline const char* ToString(HealthState s) {
    switch (s) {
        case HealthState::Healthy:  return "Healthy";
        case HealthState::Degraded: return "Degraded";
        case HealthState::Failing:  return "Failing";
    }
    return "Unknown";
}

struct HealthReport {
    HealthState state = HealthState::Healthy;
    std::string detail;
    std::string lastError;   // most recent failure, human-readable (00 §7)
    uint64_t errorCount = 0;
    std::chrono::microseconds latencyP95{0};

    bool IsHealthy() const { return state == HealthState::Healthy; }
};

// ---------------------------------------------------------------------------
// Generic metrics (00 §4)
// ---------------------------------------------------------------------------
struct Metrics {
    double cpuPct = 0.0;
    uint64_t ramBytes = 0;
    std::chrono::microseconds latencyP95{0};
    uint64_t errorCount = 0;
    uint64_t queueLength = 0;
    uint64_t threadCount = 0;
    HealthState health = HealthState::Healthy;
};

// ---------------------------------------------------------------------------
// Subscription token
// ---------------------------------------------------------------------------
struct Subscription {
    uint64_t id = 0;
    bool Valid() const { return id != 0; }
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
using EngineClock = std::chrono::steady_clock;
using EngineTime = EngineClock::time_point;

inline EngineTime Now() { return EngineClock::now(); }

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
using ModuleId = std::string;

// ---------------------------------------------------------------------------
// Version inline implementations
// ---------------------------------------------------------------------------
inline Version Version::Parse(std::string_view s) {
    Version v;
    auto dot = [&](std::string_view str, int& out) {
        out = 0;
        for (char c : str) {
            if (c < '0' || c > '9') break;
            out = out * 10 + (c - '0');
        }
    };
    size_t p1 = s.find('.');
    size_t p2 = p1 == std::string_view::npos ? std::string_view::npos : s.find('.', p1 + 1);
    std::string_view a = s.substr(0, p1);
    std::string_view b = p1 == std::string_view::npos ? std::string_view{} : s.substr(p1 + 1, (p2 == std::string_view::npos ? std::string_view::npos : p2 - p1 - 1));
    std::string_view c;
    if (p2 != std::string_view::npos) {
        std::string_view rest = s.substr(p2 + 1);
        size_t dash = rest.find('-');
        c = rest.substr(0, dash);
        if (dash != std::string_view::npos) v.pre = std::string(rest.substr(dash + 1));
    }
    dot(a, v.major);
    dot(b, v.minor);
    dot(c, v.patch);
    return v;
}

inline std::string Version::ToString() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!pre.empty()) s += "-" + pre;
    return s;
}

inline int Version::Compare(const Version& o) const noexcept {
    if (major != o.major) return major < o.major ? -1 : 1;
    if (minor != o.minor) return minor < o.minor ? -1 : 1;
    if (patch != o.patch) return patch < o.patch ? -1 : 1;
    if (pre != o.pre) {
        if (pre.empty()) return 1;  // release > prerelease
        if (o.pre.empty()) return -1;
        return pre < o.pre ? -1 : 1;
    }
    return 0;
}

} // namespace bps
