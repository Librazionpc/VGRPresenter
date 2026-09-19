// Unit tests: Adaptive Runtime (docs/specs/16).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests adaptive
#include "TestHarness.hpp"

void TestAdaptiveHardware() {
    using namespace bps::adaptive;
    HardwareProfiler hw;
    hw.Refresh();
    const auto& info = hw.Info();
    // The PAL backend always reports something on the host.
    CHECK(info.coreCount >= 1);
    CHECK(info.totalRamBytes > 0);
    CHECK(!info.osName.empty());
    CHECK(!info.arch.empty());

    CapabilityDetector caps;
    caps.SetHardware(info);
    // Whatever it is, the answer is consistent and queryable.
    (void)caps.Supports(Capability::Sse4);
    (void)caps.Supports(Capability::Avx2);
    (void)caps.Supports(Capability::HardwareVideoDecode);
    auto supported = caps.Supported();
    CHECK(supported.size() > 0);
    CHECK(supported.size() <= static_cast<size_t>(Capability::Count));
    for (auto c : supported) CHECK(caps.Supports(c));
}
void TestAdaptiveProfiler() {
    using namespace bps::adaptive;
    PerformanceProfiler p;
    for (int i = 0; i < 40; ++i) {
        p.RecordCpuPct(25.0);
        p.RecordGpuPct(50.0);
        p.RecordFrameTime(16.7);
        p.RecordImportSpeed(120.0);
        p.RecordCacheHitRatio(0.9);
        p.RecordDecodeLatency(5.0);
        p.RecordTemperature(70.0);
        p.RecordMemory(100, 1000);
        p.RecordVram(20, 200);
    }
    auto s = p.Snapshot();
    CHECK(s.cpuPct >= 20.0 && s.cpuPct <= 30.0);
    CHECK(s.gpuPct >= 45.0 && s.gpuPct <= 55.0);
    CHECK(s.cacheHitRatio >= 0.8 && s.cacheHitRatio <= 1.0);
    CHECK(s.ramTotalBytes == 1000);
    CHECK(s.ramUsedBytes <= 1000);
    CHECK(s.vramTotalBytes == 200);
    CHECK(s.cpuTemperatureC > 60.0);
    CHECK(p.SampleCount() > 0);
    p.Reset();
    CHECK(p.SampleCount() == 0);
}
void TestAdaptiveFeatures() {
    using namespace bps::adaptive;
    FeatureManager fm;
    ModuleContract c;
    c.name = "ai";
    c.minRamBytes = 512ull << 20;
    c.recommendedRamBytes = 2ull << 30;
    c.gpuOptional = true;
    c.supportsLazyLoading = true;
    c.supportsSuspension = true;
    CHECK(fm.Register({"ai", "AI Search", c, FeatureState::Enabled, true}).ok());
    // Duplicate id rejected.
    CHECK(!fm.Register({"ai", "AI Search", c, FeatureState::Enabled, true}).ok());
    CHECK(fm.Count() == 1);
    CHECK(fm.IsEnabled("ai"));
    CHECK(fm.SetSuspended("ai", true).ok());
    CHECK(fm.State("ai").value() == FeatureState::Suspended);
    CHECK(fm.IsEnabled("ai"));   // suspended still counts as enabled (resumable)
    CHECK(fm.SetSuspended("ai", false).ok());
    CHECK(fm.SetEnabled("ai", false).ok());
    CHECK(fm.State("ai").value() == FeatureState::Disabled);
    CHECK(fm.Contract("ai").has_value());
    CHECK(!fm.Contract("nope").has_value());
    CHECK(!fm.SetEnabled("nope", true).ok());
    // Auto-recommend: big machine keeps it, tiny machine disables it.
    auto stBig = fm.RecommendState({"ai", "AI Search", c, FeatureState::Enabled, true},
                                   16ull << 30, true);
    CHECK(stBig != FeatureState::Disabled);
    auto stTiny = fm.RecommendState({"ai", "AI Search", c, FeatureState::Enabled, true},
                                   256ull << 20, false);
    CHECK(stTiny == FeatureState::Disabled);
    (void)fm.Unregister("ai");
    CHECK(fm.Count() == 0);
}
void TestAdaptiveQuality() {
    using namespace bps::adaptive;
    QualitySettings q = SettingsFor(QualityLevel::Minimal, 4ull << 30, 4, false);
    CHECK(q.textureBudgetBytes >= (64ull << 20));   // floor
    CHECK(q.aiModelTier == 0);
    CHECK(!q.animationsEnabled);
    CHECK(q.qualityScale < 0.7);

    QualitySettings u = SettingsFor(QualityLevel::Ultra, 64ull << 30, 16, true);
    CHECK(u.textureBudgetBytes > q.textureBudgetBytes);
    CHECK(u.aiModelTier > 0);
    CHECK(u.animationsEnabled);

    UserModeManager modes;
    CHECK(modes.Mode() == UserMode::Automatic);
    modes.SetMode(UserMode::Battery);
    CHECK(modes.Mode() == UserMode::Battery);
    CHECK(BaseLevelFor(UserMode::Battery) == QualityLevel::BatterySaver);
    CHECK(BaseLevelFor(UserMode::SafeMode) == QualityLevel::Minimal);
    CHECK(BaseLevelFor(UserMode::Quality) == QualityLevel::High);
    modes.SetLayer(ConfigLayer::Expert);
    CHECK(modes.Layer() == ConfigLayer::Expert);
    modes.SetPreference(Preference::PreferBattery);
    CHECK(modes.GetPreference() == Preference::PreferBattery);
}
void TestAdaptiveBudgets() {
    using namespace bps::adaptive;
    ResourceBudgetManager bm;
    auto q = SettingsFor(QualityLevel::Balanced, 8ull << 30, 8, true);
    bm.Recompute(8ull << 30, q, PressureLevel::None);
    CHECK(bm.Count() == 6);
    CHECK(bm.Get(Subsystem::kRenderer).maxBytes > 0);
    CHECK(bm.Get(Subsystem::kAi).enabled);
    CHECK(bm.Total() > 0);
    // Pressure shrinks budgets.
    auto before = bm.Get(Subsystem::kCache).maxBytes;
    bm.Recompute(8ull << 30, q, PressureLevel::High);
    CHECK(bm.Get(Subsystem::kCache).maxBytes < before);
    // Disabled AI gets a zero budget.
    q.aiModelTier = 0;
    bm.Recompute(8ull << 30, q, PressureLevel::None);
    CHECK(bm.Get(Subsystem::kAi).maxBytes == 0);
    CHECK(!bm.Get(Subsystem::kAi).enabled);
}
void TestAdaptiveOptimizer() {
    using namespace bps::adaptive;
    HardwareInfo hw;
    hw.coreCount = 16;
    hw.totalRamBytes = 64ull << 30;
    hw.gpuDetected = true;
    hw.gpuVramTotalBytes = 12ull << 30;
    hw.osName = "Linux";
    hw.hdrSupported = true;

    CapabilityDetector caps;
    caps.SetHardware(hw);
    CHECK(caps.Supports(Capability::HardwareVideoDecode));
    CHECK(caps.Supports(Capability::Vulkan));
    CHECK(caps.Supports(Capability::Hdr));
    CHECK(!caps.Supports(Capability::DirectX12));   // not Windows

    PerformanceProfiler prof;
    ResourceBudgetManager budgets;
    UserModeManager modes;
    QualitySettings settings = SettingsFor(QualityLevel::High, hw.totalRamBytes, 16, true);
    RuntimeOptimizer opt;
    std::mutex settingsMutex;
    opt.Bind(&hw, &caps, &prof, &budgets, &modes, &settings, &settingsMutex);
    budgets.Recompute(hw.totalRamBytes, settings, PressureLevel::None);

    CHECK(opt.GetRecommendedThreadCount() >= 1);
    CHECK(opt.GetRecommendedThreadCount() <= 16);
    CHECK(opt.GetTextureBudget() > 0);
    CHECK(opt.GetCacheBytes() > 0);
    CHECK(opt.GetBatchSize() >= 8);
    CHECK(opt.ShouldUseHardwareDecoder());
    CHECK(opt.ShouldUseGpu(GpuTask::Blur));
    CHECK(opt.GetAIModelTier() > 0);
    CHECK(opt.GetSearchTier() >= 1);
    CHECK(opt.GetStreamingQuality() >= 360);
    CHECK(opt.BackgroundWorkAllowed());
    CHECK(opt.GetQualityLevel() == QualityLevel::High);   // 64 GB + GPU

    // Automatic mode derives from the machine.
    modes.SetMode(UserMode::Automatic);
    CHECK(opt.GetQualityLevel() == QualityLevel::High);

    // Presentation mode blocks background work.
    modes.SetMode(UserMode::Presentation);
    CHECK(!opt.BackgroundWorkAllowed());

    // Battery preference collapses AI.
    modes.SetMode(UserMode::Automatic);
    modes.SetPreference(Preference::PreferBattery);
    CHECK(opt.GetAIModelTier() == 0);
    modes.SetPreference(Preference::None);

    // Safe mode → 1 worker.
    modes.SetMode(UserMode::SafeMode);
    CHECK(opt.GetRecommendedThreadCount() == 1);
}
void TestAdaptivePressure() {
    using namespace bps::adaptive;
    PowerManager pwr;
    pwr.Update(true, false, 80);
    CHECK(!pwr.ShouldThrottle());
    pwr.Update(true, true, 40);
    CHECK(pwr.ShouldThrottle());
    pwr.Update(true, true, 60);
    CHECK(!pwr.ShouldThrottle());

    ThermalManager th;
    th.Update(40.0);
    CHECK(!th.ShouldThrottle());
    th.Update(92.0);
    CHECK(th.ShouldThrottle());

    MemoryPressureManager mp;
    mp.Update(PressureLevel::None);
    CHECK(mp.SuggestedReclaimBytes(1024ull << 20) == 0);
    mp.Update(PressureLevel::High);
    CHECK(mp.SuggestedReclaimBytes(1024ull << 20) > 0);

    GPURuntime gpu;
    CHECK(!gpu.GpuAvailable());
    gpu.SetGpuAvailable(true);
    CHECK(gpu.GpuAvailable());
}
void TestAdaptiveLearning() {
    using namespace bps::adaptive;
    UsageLearningEngine learn;
    learn.RecordUsage("module", "presentation");
    learn.RecordUsage("module", "presentation");
    learn.RecordUsage("module", "bible");
    learn.RecordUsage("module", "songs");
    CHECK(learn.Count() == 3);
    CHECK(learn.Frequency("module", "presentation") > learn.Frequency("module", "bible"));
    CHECK(learn.Frequency("module", "never") == 0.0);
    auto top = learn.Top("module", 2);
    CHECK(top.size() == 2);
    CHECK(top[0] == "presentation");

    SmartCacheManager cache;
    cache.SetLearning(&learn);
    CHECK(cache.ShouldPreload("module", "presentation"));
    CHECK(cache.ShouldPreload("module", "bible"));      // 25% ≥ 2% threshold
    CHECK(!cache.ShouldPreload("module", "never-used"));
    CHECK(cache.PreloadCandidates("module", 2).size() == 2);
    cache.SetPreloadEnabled(false);
    CHECK(!cache.ShouldPreload("module", "presentation"));
    CHECK(cache.PreloadCandidates("module").empty());

    StartupOptimizer startup;
    startup.SetLearning(&learn);
    auto core = startup.CoreSet();
    CHECK(!core.empty());
    auto rec = startup.RecommendedModules(0.02);
    CHECK(rec.size() >= core.size());

    RecommendationEngine recEng;
    auto recs = recEng.Recommend(true, QualityLevel::Balanced, 2ull << 30, false);
    CHECK(recs.size() >= 2);   // GPU + low-RAM recommendations
    auto recs2 = recEng.Recommend(true, QualityLevel::Ultra, 64ull << 30, false);
    CHECK(recs2.empty());      // nothing to recommend on a strong machine
}
void TestAdaptiveRuntime() {
    using namespace bps::adaptive;
    auto& rt = AdaptiveRuntime::Instance();
    CHECK(rt.Initialize().ok());
    CHECK(rt.Start().ok());

    // The questions all answer without crashing.
    (void)rt.GetRecommendedThreadCount();
    (void)rt.GetTextureBudget();
    (void)rt.GetCacheBytes();
    (void)rt.GetBatchSize();
    (void)rt.ShouldUseHardwareDecoder();
    (void)rt.GetAIModelTier();
    (void)rt.GetSearchTier();
    (void)rt.GetStreamingQuality();
    (void)rt.BackgroundWorkAllowed();

    // User control round-trips.
    CHECK(rt.SetUserMode(UserMode::Performance).ok());
    CHECK(rt.GetUserMode() == UserMode::Performance);
    CHECK(rt.SetConfigLayer(ConfigLayer::Assisted).ok());
    CHECK(rt.GetConfigLayer() == ConfigLayer::Assisted);
    CHECK(rt.SetPreference(Preference::PreferQuality).ok());
    CHECK(rt.GetPreference() == Preference::PreferQuality);

    // Feature registry through the facade.
    ModuleContract c;
    c.name = "motion-capture";
    c.minRamBytes = 64ull << 20;
    c.supportsLazyLoading = true;
    c.supportsSuspension = true;
    CHECK(rt.RegisterFeature({"motion", "Motion Capture", c, FeatureState::Enabled, true}).ok());
    CHECK(rt.GetFeatureState("motion").value() == FeatureState::Enabled);
    CHECK(rt.SetFeatureEnabled("motion", false).ok());
    CHECK(rt.GetFeatureState("motion").value() == FeatureState::Disabled);

    // Learning through the facade.
    rt.RecordUsage("asset", "logo.png");
    rt.RecordUsage("asset", "logo.png");
    CHECK(rt.ShouldPreload("asset", "logo.png"));
    CHECK(!rt.PreloadCandidates("asset").empty());
    CHECK(rt.SaveLearning().ok());
    CHECK(rt.LoadLearning().ok());

    // Dashboard snapshot is populated.
    auto snap = rt.Snapshot();
    CHECK(!snap.mode.empty());
    CHECK(!snap.quality.empty());
    CHECK(snap.cores >= 1);
    CHECK(!snap.budgets.empty());
    CHECK(snap.workerThreads >= 1);

    // Optimization applies live systems (thread pool + cache capacity + mode).
    CHECK(rt.ApplyOptimization("test").ok());
    CHECK(rt.GetHealth().state == HealthState::Healthy);
    CHECK(rt.MetricsSnapshot().health == HealthState::Healthy);

    CHECK(rt.Stop().ok());
    CHECK(rt.Shutdown().ok());
    CHECK(rt.Reset().ok());
}

// ===========================================================================
// Phase 6 — Rendering Engine (docs/specs/17)
// ===========================================================================
