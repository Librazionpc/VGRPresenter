#pragma once

// Learning + startup + recommendations (docs/specs/16 §11–§13, §21). The engine
// learns what the user actually uses (kind + id, per weekday), turns the pattern
// into preload hints (SmartCacheManager) and a required-module set
// (StartupOptimizer), and surfaces non-forcing recommendations.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/adaptive/Quality.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::adaptive {

// --- UsageLearningEngine (docs/specs/16 §13) ---------------------------------
class UsageLearningEngine {
public:
    UsageLearningEngine() = default;

    // Record that `id` of `kind` (module/asset/template/background/…) was used.
    void RecordUsage(std::string_view kind, std::string_view id, int weekday = -1);

    // Frequency score in [0,1] for a kind+id (0 = never used).
    double Frequency(std::string_view kind, std::string_view id) const;

    // Top-N most-used ids of a kind (preload candidates).
    std::vector<std::string> Top(std::string_view kind, int limit = 10) const;

    // Persistence (collection "adaptive-usage").
    Result<void> Save();
    Result<void> Load();
    size_t Count() const;

private:
    struct Key {
        std::string kind;
        std::string id;
        bool operator<(const Key& o) const {
            if (kind != o.kind) return kind < o.kind;
            return id < o.id;
        }
    };
    mutable std::mutex mutex_;
    std::map<Key, uint64_t> counts_;
    std::map<Key, uint64_t> weekdayCounts_[7];
    uint64_t total_ = 0;
};

// --- SmartCacheManager (docs/specs/16 §12) -----------------------------------
// Learns patterns (e.g. every Sunday: songs + bible + logo) and answers
// ShouldPreload(kind, id) / PreloadCandidates().
class SmartCacheManager {
public:
    SmartCacheManager() = default;

    void SetLearning(const UsageLearningEngine* learning) { learning_ = learning; }
    void SetPreloadEnabled(bool enabled) { preloadEnabled_.store(enabled); }
    bool PreloadEnabled() const { return preloadEnabled_.load(); }

    bool ShouldPreload(std::string_view kind, std::string_view id) const;
    std::vector<std::string> PreloadCandidates(std::string_view kind, int limit = 5) const;

private:
    const UsageLearningEngine* learning_ = nullptr;
    std::atomic<bool> preloadEnabled_{true};
};

// --- StartupOptimizer (docs/specs/16 §10, §11) --------------------------------
// Don't start everything. Returns the module set the user actually needs based
// on learning + mode; modules not in the set are candidates for lazy loading.
class StartupOptimizer {
public:
    StartupOptimizer() = default;

    void SetLearning(const UsageLearningEngine* learning) { learning_ = learning; }

    // Always-required core set (unchanging).
    std::vector<std::string> CoreSet() const {
        return {"logger", "config", "eventbus", "threadpool", "scheduler", "resources"};
    }

    // Modules the user demonstrably uses (frequency above threshold).
    std::vector<std::string> LearnedSet(double minFrequency = 0.02) const;

    // Full recommendation: core + learned, in stable order.
    std::vector<std::string> RecommendedModules(double minFrequency = 0.02) const;

private:
    const UsageLearningEngine* learning_ = nullptr;
};

// --- RecommendationEngine (docs/specs/16 §21) ---------------------------------
// "Your PC supports GPU rendering — enable it?" Never forces anything.
struct Recommendation {
    std::string message;
    std::string actionId;   // "enable-gpu", "switch-profile", ...
};

class RecommendationEngine {
public:
    RecommendationEngine() = default;

    // Build recommendations from the current machine + quality level.
    std::vector<Recommendation> Recommend(bool gpuAvailable, QualityLevel currentLevel,
                                          uint64_t totalRamBytes, bool onBattery) const;
};

} // namespace bps::adaptive
