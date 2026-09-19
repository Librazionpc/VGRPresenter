#include "modules/adaptive/Optimizer.hpp"

#include <algorithm>

namespace bps::adaptive {

namespace {
constexpr uint64_t kGb = 1024ull * 1024ull * 1024ull;
}

unsigned RuntimeOptimizer::GetRecommendedThreadCount() const {
    if (!settings_) return 2;
    QualitySettings q = LockedSettings();
    if (modes_ && modes_->Mode() == UserMode::SafeMode) return 1;
    if (modes_ && modes_->GetPreference() == Preference::PreferBattery) return 1;
    // Bounded by the active profile's worker count and the machine.
    unsigned cores = hw_ ? std::max(1u, hw_->coreCount) : 2u;
    return std::max(1u, std::min(q.workerThreads, cores));
}

uint64_t RuntimeOptimizer::GetTextureBudget() const {
    if (budgets_) return budgets_->Get(Subsystem::kRenderer).maxBytes;
    return 256ull << 20;
}

uint64_t RuntimeOptimizer::GetCacheBytes() const {
    if (budgets_) return budgets_->Get(Subsystem::kCache).maxBytes;
    return 256ull << 20;
}

uint64_t RuntimeOptimizer::GetRenderCacheBytes() const {
    // Render cache is a fraction of the renderer budget (composited frames,
    // glyph atlases). Kept inside the runtime — no subsystem computes it.
    uint64_t total = GetTextureBudget();
    return total / 4;
}

size_t RuntimeOptimizer::GetBatchSize() const {
    // Import/thumbnail batch scales with threads; big batches on big machines.
    unsigned t = GetRecommendedThreadCount();
    if (t <= 2) return 8;
    if (t <= 8) return 32;
    return 128;
}

bool RuntimeOptimizer::ShouldUseHardwareDecoder() const {
    if (!caps_) return false;
    (void)modes_;
    return caps_->Supports(Capability::HardwareVideoDecode);
}

bool RuntimeOptimizer::ShouldUseGpu(GpuTask task) const {
    if (!settings_) return false;
    QualitySettings q = LockedSettings();
    if (!q.gpuEffects) return false;
    if (!caps_) return false;
    switch (task) {
        case GpuTask::Decode:     return ShouldUseHardwareDecoder();
        case GpuTask::Text:
        case GpuTask::Blur:
        case GpuTask::Transitions:
        case GpuTask::Scaling:
        case GpuTask::Compositing:
        case GpuTask::Effects:    return q.gpuEffects;
    }
    return false;
}

int RuntimeOptimizer::GetAIModelTier() const {
    if (!settings_) return 0;
    QualitySettings q = LockedSettings();
    if (modes_ && modes_->GetPreference() == Preference::PreferLowMemory) return 0;
    if (modes_ && modes_->GetPreference() == Preference::PreferBattery) return 0;
    return q.aiModelTier;
}

int RuntimeOptimizer::GetSearchTier() const {
    if (!settings_) return 1;
    return LockedSettings().searchTier;
}

int RuntimeOptimizer::GetStreamingQuality() const {
    if (!settings_) return 720;
    QualitySettings q = LockedSettings();
    if (q.qualityScale <= 0.6) return 360;
    if (q.qualityScale <= 0.8) return 540;
    if (!caps_ || !caps_->Supports(Capability::HardwareVideoEncode)) return 720;
    return 1080;
}

unsigned RuntimeOptimizer::GetThumbnailResolutionPct() const {
    if (!settings_) return 100;
    return LockedSettings().thumbnailScalePct;
}

bool RuntimeOptimizer::ShouldPreloadFrequentlyUsed() const {
    if (!settings_) return true;
    return LockedSettings().backgroundIndexing;
}

bool RuntimeOptimizer::BackgroundWorkAllowed() const {
    if (!settings_) return true;
    // During a live presentation only critical work proceeds.
    if (modes_ && modes_->Mode() == UserMode::Presentation) return false;
    QualitySettings q = LockedSettings();
    return q.backgroundIndexing || q.backgroundThumbnails;
}

QualityLevel RuntimeOptimizer::GetQualityLevel() const {
    if (!settings_) return QualityLevel::Balanced;
    if (!modes_) return QualityLevel::Balanced;
    // Automatic mode derives from the machine: 16 GB+ GPU → High, 8 GB → Balanced,
    // 4 GB or no GPU → Performance. Assisted/Expert use the chosen profile.
    if (modes_->Mode() == UserMode::Automatic) {
        uint64_t ram = hw_ ? hw_->totalRamBytes : 0;
        bool gpu = caps_ && caps_->Supports(Capability::HardwareVideoDecode);
        if (ram >= 16 * kGb && gpu) return QualityLevel::High;
        if (ram >= 8 * kGb) return QualityLevel::Balanced;
        return QualityLevel::Performance;
    }
    return BaseLevelFor(modes_->Mode());
}

void RuntimeOptimizer::Apply() {
    // The optimizer owns the knobs it can apply today: budgets + quality
    // settings are recomputed by the facade on the heartbeat; Apply() is where
    // future subsystems (renderer, media) would re-read them. Kept as the
    // documented extension point for Phase 6+.
    std::lock_guard<std::mutex> lock(mutex_);
    (void)GetQualityLevel();
}

} // namespace bps::adaptive
