#include "modules/adaptive/Quality.hpp"

#include <algorithm>

namespace bps::adaptive {

QualitySettings SettingsFor(QualityLevel level, uint64_t totalRamBytes, unsigned cores,
                            bool gpuAvailable) {
    QualitySettings s;
    // Scale factors relative to the machine size (no magic numbers escape the
    // runtime — these live here by design).
    const uint64_t gb = 1024ull * 1024ull * 1024ull;
    const uint64_t effective = totalRamBytes > 0 ? totalRamBytes : 4 * gb;
    const unsigned effCores = cores > 0 ? cores : 4;

    switch (level) {
        case QualityLevel::Minimal:
            s.textureBudgetBytes = effective / 32;
            s.cacheBudgetBytes = effective / 32;
            s.renderCacheBytes = effective / 64;
            s.workerThreads = 1;
            s.thumbnailScalePct = 25;
            s.animationsEnabled = false;
            s.backgroundIndexing = false;
            s.backgroundThumbnails = false;
            s.shadowsEnabled = false;
            s.blurEnabled = false;
            s.gpuEffects = false;
            s.streamingEnabled = false;
            s.aiModelTier = 0;
            s.searchTier = 0;
            s.qualityScale = 0.5;
            break;
        case QualityLevel::BatterySaver:
            s.textureBudgetBytes = effective / 16;
            s.cacheBudgetBytes = effective / 16;
            s.renderCacheBytes = effective / 32;
            s.workerThreads = std::max(1u, effCores / 2);
            s.thumbnailScalePct = 50;
            s.animationsEnabled = false;
            s.backgroundIndexing = false;
            s.backgroundThumbnails = false;
            s.shadowsEnabled = false;
            s.blurEnabled = false;
            s.gpuEffects = false;
            s.streamingEnabled = false;
            s.aiModelTier = 0;
            s.searchTier = 0;
            s.qualityScale = 0.66;
            break;
        case QualityLevel::Performance:
            s.textureBudgetBytes = effective / 8;
            s.cacheBudgetBytes = effective / 8;
            s.renderCacheBytes = effective / 16;
            s.workerThreads = effCores;
            s.thumbnailScalePct = 50;
            s.animationsEnabled = true;
            s.backgroundIndexing = true;
            s.backgroundThumbnails = false;
            s.shadowsEnabled = false;
            s.blurEnabled = false;
            s.gpuEffects = gpuAvailable;
            s.streamingEnabled = true;
            s.aiModelTier = 1;
            s.searchTier = 1;
            s.qualityScale = 0.75;
            break;
        case QualityLevel::High:
            s.textureBudgetBytes = effective / 4;
            s.cacheBudgetBytes = effective / 4;
            s.renderCacheBytes = effective / 8;
            s.workerThreads = effCores;
            s.thumbnailScalePct = 100;
            s.animationsEnabled = true;
            s.backgroundIndexing = true;
            s.backgroundThumbnails = true;
            s.shadowsEnabled = true;
            s.blurEnabled = true;
            s.gpuEffects = gpuAvailable;
            s.streamingEnabled = true;
            s.aiModelTier = 2;
            s.searchTier = 2;
            s.qualityScale = 1.0;
            break;
        case QualityLevel::Ultra:
            s.textureBudgetBytes = effective / 3;
            s.cacheBudgetBytes = effective / 4;
            s.renderCacheBytes = effective / 6;
            s.workerThreads = effCores + 1;
            s.thumbnailScalePct = 100;
            s.animationsEnabled = true;
            s.backgroundIndexing = true;
            s.backgroundThumbnails = true;
            s.shadowsEnabled = true;
            s.blurEnabled = true;
            s.gpuEffects = gpuAvailable;
            s.streamingEnabled = true;
            s.aiModelTier = gpuAvailable ? 3 : 2;
            s.searchTier = 2;
            s.qualityScale = 1.0;
            break;
        case QualityLevel::Custom:
        case QualityLevel::Balanced:
        default:
            s.textureBudgetBytes = effective / 8;
            s.cacheBudgetBytes = effective / 8;
            s.renderCacheBytes = effective / 16;
            s.workerThreads = std::max(1u, effCores - (effCores > 2 ? 1u : 0u));
            s.thumbnailScalePct = 75;
            s.animationsEnabled = true;
            s.backgroundIndexing = true;
            s.backgroundThumbnails = true;
            s.shadowsEnabled = true;
            s.blurEnabled = gpuAvailable;
            s.gpuEffects = gpuAvailable;
            s.streamingEnabled = true;
            s.aiModelTier = totalRamBytes >= 8 * gb ? 2 : 1;
            s.searchTier = totalRamBytes >= 8 * gb ? 2 : 1;
            s.qualityScale = 0.9;
            break;
    }
    // Hard floors: never starve the essential experience.
    s.textureBudgetBytes = std::max<uint64_t>(s.textureBudgetBytes, 64ull << 20);
    s.cacheBudgetBytes = std::max<uint64_t>(s.cacheBudgetBytes, 64ull << 20);
    s.renderCacheBytes = std::max<uint64_t>(s.renderCacheBytes, 16ull << 20);
    return s;
}

QualityLevel BaseLevelFor(UserMode mode) {
    switch (mode) {
        case UserMode::Performance:  return QualityLevel::Performance;
        case UserMode::Quality:      return QualityLevel::High;
        case UserMode::Battery:      return QualityLevel::BatterySaver;
        case UserMode::Presentation: return QualityLevel::Balanced;
        case UserMode::SafeMode:     return QualityLevel::Minimal;
        case UserMode::Developer:    return QualityLevel::High;
        case UserMode::Custom:       return QualityLevel::Custom;
        case UserMode::Automatic:
        case UserMode::Balanced:
        default:                     return QualityLevel::Balanced;
    }
}

} // namespace bps::adaptive
