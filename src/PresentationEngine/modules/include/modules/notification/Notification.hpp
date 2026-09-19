#pragma once

// Notification object (docs/specs/14 §Notification Object). One model for every
// user-visible notification. Contains NO UI information — providers decide how
// to render it.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bps::notification {

enum class Severity : int { Debug = 0, Information, Success, Warning, Error, Critical };

inline const char* ToString(Severity s) {
    switch (s) {
        case Severity::Debug:       return "Debug";
        case Severity::Information: return "Information";
        case Severity::Success:     return "Success";
        case Severity::Warning:     return "Warning";
        case Severity::Error:       return "Error";
        case Severity::Critical:    return "Critical";
    }
    return "Unknown";
}

inline Severity SeverityFromString(std::string_view s) {
    if (s == "Critical") return Severity::Critical;
    if (s == "Error") return Severity::Error;
    if (s == "Warning") return Severity::Warning;
    if (s == "Success") return Severity::Success;
    if (s == "Information") return Severity::Information;
    return Severity::Debug;
}

enum class Priority : int { Low = 0, Normal, High, Critical };

inline const char* ToString(Priority p) {
    switch (p) {
        case Priority::Low:      return "Low";
        case Priority::Normal:   return "Normal";
        case Priority::High:     return "High";
        case Priority::Critical: return "Critical";
    }
    return "Normal";
}

// Categories (docs/specs/14 §Categories).
namespace Category {
inline constexpr const char* kSystem = "System";
inline constexpr const char* kPresentation = "Presentation";
inline constexpr const char* kBible = "Bible";
inline constexpr const char* kSong = "Song";
inline constexpr const char* kMedia = "Media";
inline constexpr const char* kPlugin = "Plugin";
inline constexpr const char* kCloud = "Cloud";
inline constexpr const char* kImport = "Import";
inline constexpr const char* kExport = "Export";
inline constexpr const char* kAI = "AI";
inline constexpr const char* kDisplay = "Display";
inline constexpr const char* kStreaming = "Streaming";
inline constexpr const char* kRemote = "Remote";
inline constexpr const char* kPerformance = "Performance";
inline constexpr const char* kSecurity = "Security";
inline constexpr const char* kProject = "Project";
inline constexpr const char* kContent = "Content";
} // namespace Category

// A user-action descriptor. The service only carries the identifier; the UI
// triggers it and the owning module performs the work (docs/specs/14 §Actions).
struct NotificationAction {
    std::string id;
    std::string label;
};

// The single notification model.
struct Notification {
    uint64_t id = 0;                 // unique id (service-assigned)
    std::string correlationId;       // groups related notifications (import run, save…)
    int64_t timestampMs = 0;
    std::string sourceModule;        // e.g. "ContentManager"
    std::string sourceEvent;         // e.g. "content.asset_imported"
    Severity severity = Severity::Information;
    std::string category = Category::kSystem;
    Priority priority = Priority::Normal;
    std::string title;
    std::string message;
    json::Value payload = json::Value::Null();
    double progress = -1.0;          // -1 = none, else [0..1]
    std::vector<NotificationAction> actions;
    bool persistent = false;         // never auto-dismisses until resolved
    bool dismissed = false;
    bool read = false;
    int64_t autoDismissMs = 0;       // 0 = follow severity default
    std::vector<std::string> channels;   // "toast", "statusbar", "center", "console"…
    std::vector<std::string> tags;
    std::string groupKey;            // grouping/dedup key
    int groupCount = 1;              // merged count ("200 assets imported")
};

// Programmatic entry point used by the factory; the service assigns ids and
// timestamps. Modules publish *events*; the factory builds seeds from them.
struct NotificationSeed {
    std::string sourceModule;
    std::string sourceEvent;
    Severity severity = Severity::Information;
    std::string category = Category::kSystem;
    Priority priority = Priority::Normal;
    std::string title;
    std::string message;
    std::string correlationId;
    std::string groupKey;
    bool persistent = false;
    double progress = -1.0;
    std::vector<NotificationAction> actions;
    std::vector<std::string> tags;
    int64_t autoDismissMs = 0;
};

} // namespace bps::notification
