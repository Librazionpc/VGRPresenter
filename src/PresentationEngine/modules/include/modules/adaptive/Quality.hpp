#pragma once

// Quality profiles, user modes and configuration layers (docs/specs/16 §6, §19,
// §20, §17). The runtime maps the active mode+layer+profile onto QualitySettings
// that the optimizer uses. Custom = user overrides on top of a base profile.

#include "core/common/Common.hpp"

#include <map>
#include <mutex>
#include <string>

namespace bps::adaptive {

// --- Quality levels (docs/specs/16 §6) -------------------------------------
enum class QualityLevel : int {
    Minimal = 0, BatterySaver, Performance, Balanced, High, Ultra, Custom
};

inline const char* ToString(QualityLevel l) {
    switch (l) {
        case QualityLevel::Minimal:      return "Minimal";
        case QualityLevel::BatterySaver: return "BatterySaver";
        case QualityLevel::Performance:  return "Performance";
        case QualityLevel::Balanced:     return "Balanced";
        case QualityLevel::High:         return "High";
        case QualityLevel::Ultra:        return "Ultra";
        case QualityLevel::Custom:       return "Custom";
    }
    return "Unknown";
}

// One concrete quality setting set (all budgets in bytes).
struct QualitySettings {
    uint64_t textureBudgetBytes = 0;
    uint64_t cacheBudgetBytes = 0;
    uint64_t renderCacheBytes = 0;
    unsigned workerThreads = 2;          // recommended worker count
    unsigned thumbnailScalePct = 100;    // 100 = full resolution
    bool animationsEnabled = true;
    bool backgroundIndexing = true;
    bool backgroundThumbnails = true;
    bool shadowsEnabled = true;
    bool blurEnabled = true;
    bool gpuEffects = true;
    bool streamingEnabled = true;
    int aiModelTier = 1;                 // 0=none, 1=small/local, 2=full/local, 3=cloud
    int searchTier = 1;                  // 0=simple, 1=fts, 2=vector
    double qualityScale = 1.0;           // 0..1 render resolution scale
};

// --- User modes (docs/specs/16 §20) ----------------------------------------
enum class UserMode : int {
    Automatic = 0, Balanced, Performance, Quality, Battery, Presentation,
    SafeMode, Developer, Custom
};

inline const char* ToString(UserMode m) {
    switch (m) {
        case UserMode::Automatic:    return "Automatic";
        case UserMode::Balanced:     return "Balanced";
        case UserMode::Performance:  return "Performance";
        case UserMode::Quality:      return "Quality";
        case UserMode::Battery:      return "Battery";
        case UserMode::Presentation: return "Presentation";
        case UserMode::SafeMode:     return "SafeMode";
        case UserMode::Developer:    return "Developer";
        case UserMode::Custom:       return "Custom";
    }
    return "Unknown";
}

// --- Configuration layers (docs/specs/16 §17) -------------------------------
enum class ConfigLayer : int { Automatic = 0, Assisted, Expert };

inline const char* ToString(ConfigLayer l) {
    switch (l) {
        case ConfigLayer::Automatic: return "Automatic";
        case ConfigLayer::Assisted:  return "Assisted";
        case ConfigLayer::Expert:    return "Expert";
    }
    return "Unknown";
}

// --- Assisted preferences (docs/specs/16 §Layer 2) ---------------------------
enum class Preference : int {
    None = 0, PreferQuality, PreferPerformance, PreferBattery, PreferQuiet,
    PreferLowMemory
};

inline const char* ToString(Preference p) {
    switch (p) {
        case Preference::None:             return "None";
        case Preference::PreferQuality:    return "PreferQuality";
        case Preference::PreferPerformance:return "PreferPerformance";
        case Preference::PreferBattery:    return "PreferBattery";
        case Preference::PreferQuiet:      return "PreferQuiet";
        case Preference::PreferLowMemory:  return "PreferLowMemory";
    }
    return "Unknown";
}

// Quality profile table: level → settings (computed for a given machine size).
QualitySettings SettingsFor(QualityLevel level, uint64_t totalRamBytes, unsigned cores,
                            bool gpuAvailable);

// Map a user mode to a base quality level.
QualityLevel BaseLevelFor(UserMode mode);

// --- UserModeManager --------------------------------------------------------
class UserModeManager {
public:
    UserModeManager() = default;

    void SetMode(UserMode mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
    }
    UserMode Mode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mode_;
    }
    void SetLayer(ConfigLayer layer) {
        std::lock_guard<std::mutex> lock(mutex_);
        layer_ = layer;
    }
    ConfigLayer Layer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layer_;
    }
    void SetPreference(Preference p) {
        std::lock_guard<std::mutex> lock(mutex_);
        preference_ = p;
    }
    Preference GetPreference() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return preference_;
    }

private:
    mutable std::mutex mutex_;
    UserMode mode_ = UserMode::Automatic;
    ConfigLayer layer_ = ConfigLayer::Automatic;
    Preference preference_ = Preference::None;
};

} // namespace bps::adaptive
